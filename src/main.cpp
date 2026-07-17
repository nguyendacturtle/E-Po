/**
 * =============================================================================
 * E_PO_test_sound — Mô phỏng Âm Thanh Động Cơ + Tích hợp BMI160 (Legacy Driver)
 * =============================================================================
 */

#include <Arduino.h>
#include <stdio.h>
#include <math.h>
#include "config.h"
#include "sound_registry.h"
#include "audio_engine.h"
#include "ble_manager.h"

// Sử dụng driver I2C truyền thống để tương thích tuyệt đối với Core cũ của E-Po
#include "driver/i2c.h"
#include "esp_timer.h"

// ─── CẤU HÌNH CHÂN I2C THEO BẢN GỐC BMI160 ───────────────────────────────────
#define I2C_SDA_PIN               GPIO_NUM_21
#define I2C_SCL_PIN               GPIO_NUM_22
#define I2C_FREQ_HZ               400000
#define BMI160_ADDR               0x69
#define I2C_PORT_NUM              I2C_NUM_0

/* ================== CÁC THANH GHI CỦA BMI160 ================== */
#define REG_CHIP_ID               0x00
#define REG_GYR_DATA              0x0C
#define REG_ACC_RANGE             0x41
#define REG_GYR_RANGE             0x43
#define REG_CMD                   0x7E

#define LPF_ALPHA                 0.15f
#define ACCEL_THRESHOLD           0.03f
#define SHOCK_MAGNITUDE_THRESHOLD 1.2f
#define FALL_ANGLE_THRESHOLD      35.0f
#define FALL_CONFIRM_TIME_MS      700
#define BUMP_RECOVER_TIME_MS      250

// ─── ENUM TRẠNG THÁI HỆ THỐNG ────────────────────────────────────────────────
enum EngineState : uint8_t {
  ENG_OFF,
  ENG_STARTING, 
  ENG_RUNNING
};

typedef enum {
  SU_KIEN_BINH_THUONG,
  SU_KIEN_SOC_O_GA,
  SU_KIEN_TE_NGA
} SuKienVaCham;

typedef enum {
  TOC_DO_DEU_GA,
  TOC_DO_TANG_TOC,
  TOC_DO_GIAM_TOC
} TrangThaiTocDo;

// ─── BIẾN TOÀN CỤC KHỞI TẠO ──────────────────────────────────────────────────
static float s_currentRPM = 0.0f;
static float s_targetRPM = RPM_IDLE;
static EngineState s_state = ENG_OFF;

static float lpf_forward_accel = 0.0f;
static bool dang_theo_doi_soc = false;
static int64_t thoi_diem_bat_dau_soc = 0;

static float throttle_bmi = 0.0f;

// FIX VẤN ĐỀ 2: Giữ trạng thái ngã tồn tại lâu hơn 20ms để tránh tự khởi động ngay lập tức
static bool dang_bi_te_nga = false;
static int64_t thoi_diem_te_nga = 0;
#define FALL_SAFE_HOLD_MS 1000  // Giữ trạng thái ngã an toàn 1 giây 

// Các biến lưu trữ dữ liệu trả về y đúc bản gốc để in Monitor
static float g_ax = 0, g_ay = 0, g_az = 0;
static float g_gx = 0, g_gy = 0, g_gz = 0;
static float g_pitch = 0, g_roll = 0;
static SuKienVaCham g_su_kien = SU_KIEN_BINH_THUONG;
static TrangThaiTocDo g_trang_thai_toc = TOC_DO_DEU_GA;

void requestEngineOff() {
  s_state = ENG_OFF;
}

// Giữ nguyên các hàm convert chuỗi hiển thị log của bạn
static const char* impact_to_string(SuKienVaCham e) {
  switch (e) {
    case SU_KIEN_TE_NGA:   return "TE NGA";
    case SU_KIEN_SOC_O_GA: return "SOC O GA";
    default:                return "Binh thuong";
  }
}

static const char* speed_to_string(TrangThaiTocDo s) {
  switch (s) {
    case TOC_DO_TANG_TOC: return "TANG TOC";
    case TOC_DO_GIAM_TOC: return "GIAM TOC";
    default:               return "DEU GA";
  }
}

// ─── CÁC HÀM TRUYỀN NHẬN I2C HOÀN TOÀN TRÊN DRIVER CŨ (GIỮ NGUYÊN GIÁ TRỊ) ───
static esp_err_t bmi160_write_reg(uint8_t reg, uint8_t data) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (BMI160_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg, true);
  i2c_master_write_byte(cmd, data, true);
  i2c_master_stop(cmd);
  // FIX VẤN ĐỀ 3: Giảm timeout I2C từ 1000ms xuống 50ms để tránh treo hệ thống
  esp_err_t ret = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(cmd);
  return ret;
}

static esp_err_t bmi160_read_regs(uint8_t reg, uint8_t *data, size_t len) {
  if (len == 0) return ESP_OK;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (BMI160_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg, true);
  
  i2c_master_start(cmd); 
  i2c_master_write_byte(cmd, (BMI160_ADDR << 1) | I2C_MASTER_READ, true);
  if (len > 1) {
      i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
  }
  i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
  i2c_master_stop(cmd);
  // FIX VẤN ĐỀ 3: Giảm timeout I2C từ 1000ms xuống 50ms để tránh treo hệ thống
  esp_err_t ret = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(cmd);
  return ret;
}

static void i2c_master_setup_legacy() {
  i2c_config_t conf;
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = (gpio_num_t)I2C_SDA_PIN;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = (gpio_num_t)I2C_SCL_PIN;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = I2C_FREQ_HZ;
  conf.clk_flags = 0;
  
  ESP_ERROR_CHECK(i2c_param_config(I2C_PORT_NUM, &conf));
  ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT_NUM, conf.mode, 0, 0, 0));
}

static bool bmi160_init_legacy() {
  uint8_t chip_id = 0;
  bmi160_read_regs(REG_CHIP_ID, &chip_id, 1);
  if (chip_id != 0xD1) {
    Serial.printf("[BMI160] ERROR: Sai CHIP_ID: 0x%02X (Phai la 0xD1)\n", chip_id);
    return false;
  }
  bmi160_write_reg(REG_CMD, 0x11); delay(100);
  bmi160_write_reg(REG_CMD, 0x15); delay(100);
  bmi160_write_reg(REG_ACC_RANGE, 0x03);
  bmi160_write_reg(REG_GYR_RANGE, 0x00);
  Serial.println("[BMI160] Khoi tao cam bien thanh cong!");
  return true;
}

// Hàm đọc và quy đổi dữ liệu - giữ nguyên thuật toán gốc chia 16384.0f và 16.4f của bạn
static void bmi160_read_data(float *ax, float *ay, float *az, float *gx, float *gy, float *gz) {
  uint8_t raw[12];
  bmi160_read_regs(REG_GYR_DATA, raw, 12);

  int16_t gyro_x_raw  = (int16_t)((raw[1]  << 8) | raw[0]);
  int16_t gyro_y_raw  = (int16_t)((raw[3]  << 8) | raw[2]);
  int16_t gyro_z_raw  = (int16_t)((raw[5]  << 8) | raw[4]);
  int16_t accel_x_raw = (int16_t)((raw[7]  << 8) | raw[6]);
  int16_t accel_y_raw = (int16_t)((raw[9]  << 8) | raw[8]);
  int16_t accel_z_raw = (int16_t)((raw[11] << 8) | raw[10]);

  *ax = accel_x_raw / 16384.0f;
  *ay = accel_y_raw / 16384.0f;
  *az = accel_z_raw / 16384.0f;
  *gx = gyro_x_raw / 16.4f;
  *gy = gyro_y_raw / 16.4f;
  *gz = gyro_z_raw / 16.4f;
}

static float apply_lpf(float raw_value) {
  lpf_forward_accel = LPF_ALPHA * raw_value + (1.0f - LPF_ALPHA) * lpf_forward_accel;
  return lpf_forward_accel;
}

static SuKienVaCham detect_impact_event(float ax, float ay, float az, float roll, float pitch) {
  float magnitude = sqrtf(ax * ax + ay * ay + az * az);
  int64_t now_ms = esp_timer_get_time() / 1000;
  bool goc_nghieng_lon = (fabsf(roll) > FALL_ANGLE_THRESHOLD) || (fabsf(pitch) > FALL_ANGLE_THRESHOLD);

  if (!dang_theo_doi_soc) {
    if (magnitude > SHOCK_MAGNITUDE_THRESHOLD) {
      dang_theo_doi_soc = true;
      thoi_diem_bat_dau_soc = now_ms;
    }
    return SU_KIEN_BINH_THUONG;
  } else {
    int64_t elapsed = now_ms - thoi_diem_bat_dau_soc;
    if (goc_nghieng_lon && elapsed > FALL_CONFIRM_TIME_MS) {
      dang_theo_doi_soc = false;
      return SU_KIEN_TE_NGA;
    }
    if (!goc_nghieng_lon && elapsed > BUMP_RECOVER_TIME_MS) {
      dang_theo_doi_soc = false;
      return SU_KIEN_SOC_O_GA;
    }
    return SU_KIEN_BINH_THUONG;
  }
}

static TrangThaiTocDo detect_motion_state(float accel_forward_filtered) {
  if (accel_forward_filtered > ACCEL_THRESHOLD) {
    return TOC_DO_TANG_TOC;
  } else if (accel_forward_filtered < -ACCEL_THRESHOLD) {
    return TOC_DO_GIAM_TOC;
  } else {
    return TOC_DO_DEU_GA;
  }
}

// ─── HÀM XỬ LÝ ADC BIẾN TRỞ TAY GA ──────────────────────────────────────────
static int readPotSmooth() {
  int32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(POT_PIN);
    delayMicroseconds(30);
  }
  int avg = (int)(sum >> 3);
  avg -= ADC_CLAMP_LOW;
  if (avg < 0) avg = 0;
  int maxUsable = ADC_MAX_RAW - ADC_CLAMP_LOW - ADC_CLAMP_HIGH;
  if (avg > maxUsable) avg = maxUsable;
  return avg;
}

static float adcToThrottle(int adcVal) {
  int maxUsable = ADC_MAX_RAW - ADC_CLAMP_LOW - ADC_CLAMP_HIGH;
  if (adcVal < ADC_DEADBAND) return 0.0f;
  float t = (float)(adcVal - ADC_DEADBAND) / (float)(maxUsable - ADC_DEADBAND);
  if (t > 1.0f) t = 1.0f;
  return t;
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=================================================");
  Serial.println(" E_PO Engine Sound — ESP32 + BMI160 Full Monitor");
  Serial.printf(" Loaded %d sound profiles.\n", SOUND_PROFILE_COUNT);
  Serial.printf(" RPM range     : %.0f – %.0f RPM\n", RPM_IDLE, RPM_MAX);
  Serial.println("=================================================\n");

  i2c_master_setup_legacy();
  if (!bmi160_init_legacy()) {
    Serial.println("[CRITICAL] Khoi tao BMI160 that bai!");
  }

  AudioEngine_init();

  Serial.println("[BLE] Initializing BLE...");
  BLEManager_init("E_PO Engine Sound");
  Serial.println("[OK] He thong san sang!");
}

// ─── LOOP ────────────────────────────────────────────────────────────────────
void loop() {
  static uint32_t lastLogMs = 0;
  static uint32_t lastBmiMs = 0;

  AudioEngine_fillBuffer();

  // 1. Đọc dữ liệu BMI160 định kỳ mỗi 10ms để bắt sự kiện nhanh hơn
  if (millis() - lastBmiMs >= 10) {
    lastBmiMs = millis();
    bmi160_read_data(&g_ax, &g_ay, &g_az, &g_gx, &g_gy, &g_gz);
    
    g_pitch = atan2f(-g_ax, sqrtf(g_ay * g_ay + g_az * g_az)) * 180.0f / M_PI;
    g_roll  = atan2f(g_ay, g_az) * 180.0f / M_PI;

    float accel_forward_filtered = apply_lpf(g_ax);

    // FIX VẤN ĐỀ 1: Không dùng fabsf() — chỉ tính throttle từ tăng tốc dương (phanh âm = 0)
    if (accel_forward_filtered > ACCEL_THRESHOLD) {
      throttle_bmi = (accel_forward_filtered - ACCEL_THRESHOLD) / (0.5f - ACCEL_THRESHOLD);
      if (throttle_bmi > 1.0f) throttle_bmi = 1.0f;
    } else {
      throttle_bmi = 0.0f;
    }

    g_su_kien = detect_impact_event(g_ax, g_ay, g_az, g_roll, g_pitch);
    g_trang_thai_toc = detect_motion_state(accel_forward_filtered);

    // FIX VẤN ĐỀ 2: Cập nhật flag ngã mỗi khi phát hiện ngã
    if (g_su_kien == SU_KIEN_TE_NGA) {
      dang_bi_te_nga = true;
      thoi_diem_te_nga = millis();
      s_state = ENG_OFF;
    }
    
    // Kiểm tra xem trạng thái ngã đã hết thời gian an toàn chưa
    if (dang_bi_te_nga && (millis() - thoi_diem_te_nga > FALL_SAFE_HOLD_MS)) {
      dang_bi_te_nga = false;
    }
  }

  // 2. Xử lý Serial Commands
  if (Serial.available()) {
    String rxValue = Serial.readStringUntil('\n');
    rxValue.trim();
    if (rxValue.length() > 0 && (rxValue.startsWith("S") || rxValue.startsWith("s"))) {
      int idx = rxValue.substring(1).toInt() - 1;
      if (idx >= 0 && idx < SOUND_PROFILE_COUNT) {
        AudioEngine_switchSound(idx);
        s_state = ENG_OFF;
      }
    }
  }

  // 3. Xử lý BLE
  BLEManager_process();

  // 4. Hòa trộn tín hiệu ga
  int adcVal = readPotSmooth();
  float throttle_pot = adcToThrottle(adcVal);
  float total_throttle = throttle_pot + throttle_bmi;
  if (total_throttle > 1.0f) total_throttle = 1.0f;

  // 5. State Machine Âm thanh động cơ
  switch (s_state) {
    case ENG_OFF:
      AudioEngine_turnOff();
      // FIX VẤN ĐỀ 2: Sử dụng flag dang_bi_te_nga thay vì g_su_kien (để giữ trạng thái ngã lâu hơn)
      if (total_throttle > 0.05f && !dang_bi_te_nga) {
        s_currentRPM = 0.0f;
        AudioEngine_playStart();
        s_state = ENG_STARTING;
      }
      break;

    case ENG_STARTING:
      if (AudioEngine_isStartDone()) {
        s_currentRPM = RPM_IDLE;
        s_targetRPM = RPM_IDLE;
        AudioEngine_update(s_currentRPM, 0.0f);
        s_state = ENG_RUNNING;
      }
      break;

    case ENG_RUNNING:
      s_targetRPM = RPM_IDLE + total_throttle * (RPM_MAX - RPM_IDLE);

      if (s_currentRPM < s_targetRPM) {
        float accel = RPM_ACCEL * (1.0f + total_throttle * 1.5f);
        s_currentRPM += accel;
        if (s_currentRPM > s_targetRPM) s_currentRPM = s_targetRPM;
      } else {
        s_currentRPM -= RPM_DECEL;
        if (s_currentRPM < s_targetRPM) s_currentRPM = s_targetRPM;
      }

      AudioEngine_update(s_currentRPM, total_throttle);
      break;
  }

  // 6. IN ĐẦY ĐỦ LOG MONITOR CỦA CẢ 2 BẢN GỐC (Chu kỳ 300ms)
  if (millis() - lastLogMs >= 300) {
    lastLogMs = millis();
    const char *st = (s_state == ENG_OFF) ? "OFF" : 
                     (s_state == ENG_STARTING) ? "STARTING" : "RUNNING";
    
    // In y đúc format cũ BMI160
    printf("===== BMI160 SENSOR DATA =====\r\n");
    printf("Accel (g)     : X=%.3f  Y=%.3f  Z=%.3f\r\n", g_ax, g_ay, g_az);
    printf("Gyro (dps)    : X=%.3f  Y=%.3f  Z=%.3f\r\n", g_gx, g_gy, g_gz);
    printf("Goc nghieng   : Pitch=%.2f  Roll=%.2f\r\n", g_pitch, g_roll);
    printf("Trang thai toc: %s (loc LPF=%.3fg)\r\n", speed_to_string(g_trang_thai_toc), lpf_forward_accel);
    printf("Su kien va cham: %s\r\n", impact_to_string(g_su_kien));
    printf("===============================\r\n");

    // In y đúc format cũ E-Po
    char logBuf[160];
    snprintf(logBuf, sizeof(logBuf),
             "[%s] RPM: %5.0f | THR: %5.1f%% (POT:%3.0f%%, IMU:%3.0f%%) | VOL: %3d | RevMix: %3d | ISR: %lu\n\n", 
             st, s_currentRPM, total_throttle * 100.0f, throttle_pot * 100.0f, throttle_bmi * 100.0f,
             (unsigned)AudioEngine_getMasterVol(), (unsigned)AudioEngine_getRevMix(), AudioEngine_getISRCount());

    Serial.print(logBuf);
    BLEManager_notifyLog(logBuf); 
  }

  delay(2); 
}