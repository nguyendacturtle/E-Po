#ifndef BMI160_SENSOR_H
#define BMI160_SENSOR_H

/**
 * =============================================================================
 * BMI160 Sensor Module — Driver I2C Legacy + Phát hiện sự kiện + Modifier
 * =============================================================================
 * Module này đóng gói toàn bộ việc giao tiếp và xử lý dữ liệu từ cảm biến
 * gia tốc/con quay hồi chuyển BMI160 qua giao thức I2C.
 *
 * Tính năng:
 *   - Đọc dữ liệu 6 trục (Accel XYZ + Gyro XYZ)
 *   - Tính góc nghiêng Pitch & Roll
 *   - Phát hiện sự kiện: Bình thường / Sốc ổ gà / Té ngã
 *   - Xuất hệ số modifier cho hệ thống hòa trộn tín hiệu ga
 */

#include <Arduino.h>
#include <math.h>
#include "driver/i2c.h"
#include "esp_timer.h"

// ─── CẤU HÌNH CHÂN I2C ──────────────────────────────────────────────────────
#define BMI_I2C_SDA               GPIO_NUM_21
#define BMI_I2C_SCL               GPIO_NUM_22
#define BMI_I2C_FREQ_HZ           400000
#define BMI_I2C_PORT              I2C_NUM_0
#define BMI160_ADDR               0x69

// ─── THANH GHI BMI160 ────────────────────────────────────────────────────────
#define BMI_REG_CHIP_ID           0x00
#define BMI_REG_GYR_DATA          0x0C
#define BMI_REG_ACC_RANGE         0x41
#define BMI_REG_GYR_RANGE         0x43
#define BMI_REG_CMD               0x7E

// ─── CẤU HÌNH BỘ LỌC VÀ NGƯỠNG ─────────────────────────────────────────────
#define BMI_LPF_ALPHA             0.05f  // LPF mạnh hơn (giảm từ 0.15) để triệt tiêu rung lắc tần số cao
#define BMI_ACCEL_THRESHOLD       0.08f  // Ngưỡng ga lớn hơn (tăng từ 0.03) làm vùng chết (deadband) chống nhiễu
#define BMI_SHOCK_THRESHOLD       1.2f
#define BMI_FALL_ANGLE_THRESHOLD  35.0f
#define BMI_FALL_CONFIRM_MS       700
#define BMI_BUMP_RECOVER_MS       250
#define BMI_FALL_SAFE_HOLD_MS     1000   // Giữ trạng thái ngã an toàn 1 giây

// ─── CẤU HÌNH MODIFIER (±20% RPM) ───────────────────────────────────────────
#define BMI_MODIFIER_BOOST        1.20f  // Tăng tốc  → RPM cao hơn 20%
#define BMI_MODIFIER_BRAKE        0.80f  // Phanh      → RPM giảm 20%
#define BMI_MODIFIER_NEUTRAL      1.00f  // Đi đều    → Không thay đổi

// ─── ENUM SỰ KIỆN & TRẠNG THÁI ──────────────────────────────────────────────
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

// ─── CẤU TRÚC DỮ LIỆU BMI160 ───────────────────────────────────────────────
// Chứa toàn bộ kết quả đo và trạng thái phân tích
typedef struct {
  // Dữ liệu thô (đã quy đổi)
  float ax, ay, az;       // Gia tốc (g)
  float gx, gy, gz;       // Con quay (dps)

  // Góc nghiêng
  float pitch, roll;      // Độ

  // Trạng thái phân tích
  SuKienVaCham   su_kien;
  TrangThaiTocDo trang_thai_toc;
  float          modifier;        // Hệ số nhân cho throttle
  float          lpf_accel;       // Giá trị LPF (để hiển thị log)

  // Trạng thái an toàn - Té ngã
  bool           dang_bi_te_nga;
} BMI160_Data;

// ═══════════════════════════════════════════════════════════════════════════════
//  PHẦN TRIỂN KHAI (Implementation) — Chỉ được include 1 lần trong main.cpp
// ═══════════════════════════════════════════════════════════════════════════════

// ─── BIẾN NỘI BỘ MODULE ─────────────────────────────────────────────────────
static float     _G_x                   = 0.0f;  // Vectơ trọng lực trục X ước lượng
static float     _G_y                   = 0.0f;  // Vectơ trọng lực trục Y ước lượng
static float     _G_z                   = 0.0f;  // Vectơ trọng lực trục Z ước lượng
static float     _bmi_lpf_accel         = 0.0f;
static bool      _bmi_dang_theo_doi_soc = false;
static int64_t   _bmi_thoi_diem_soc     = 0;
static bool      _bmi_dang_bi_te_nga    = false;
static uint32_t  _bmi_thoi_diem_te_nga  = 0;
static uint32_t  _bmi_last_micros       = 0;  // Lưu mốc thời gian vi giây để tính dt động

// ─── HÀM I2C CẤP THẤP ──────────────────────────────────────────────────────

static esp_err_t _bmi_write_reg(uint8_t reg, uint8_t data) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (BMI160_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg, true);
  i2c_master_write_byte(cmd, data, true);
  i2c_master_stop(cmd);
  esp_err_t ret = i2c_master_cmd_begin(BMI_I2C_PORT, cmd, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(cmd);
  return ret;
}

static esp_err_t _bmi_read_regs(uint8_t reg, uint8_t *data, size_t len) {
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
  esp_err_t ret = i2c_master_cmd_begin(BMI_I2C_PORT, cmd, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(cmd);
  return ret;
}

// ─── HÀM XỬ LÝ NỘI BỘ ──────────────────────────────────────────────────────

static float _bmi_apply_lpf(float raw_value) {
  _bmi_lpf_accel = BMI_LPF_ALPHA * raw_value + (1.0f - BMI_LPF_ALPHA) * _bmi_lpf_accel;
  return _bmi_lpf_accel;
}

static SuKienVaCham _bmi_detect_impact(float ax, float ay, float az, float roll, float pitch) {
  float magnitude = sqrtf(ax * ax + ay * ay + az * az);
  int64_t now_ms = esp_timer_get_time() / 1000;
  bool goc_nghieng_lon = (fabsf(roll) > BMI_FALL_ANGLE_THRESHOLD) || (fabsf(pitch) > BMI_FALL_ANGLE_THRESHOLD);

  if (!_bmi_dang_theo_doi_soc) {
    if (magnitude > BMI_SHOCK_THRESHOLD) {
      _bmi_dang_theo_doi_soc = true;
      _bmi_thoi_diem_soc = now_ms;
    }
    return SU_KIEN_BINH_THUONG;
  } else {
    int64_t elapsed = now_ms - _bmi_thoi_diem_soc;
    if (goc_nghieng_lon && elapsed > BMI_FALL_CONFIRM_MS) {
      _bmi_dang_theo_doi_soc = false;
      return SU_KIEN_TE_NGA;
    }
    if (!goc_nghieng_lon && elapsed > BMI_BUMP_RECOVER_MS) {
      _bmi_dang_theo_doi_soc = false;
      return SU_KIEN_SOC_O_GA;
    }
    return SU_KIEN_BINH_THUONG;
  }
}

static TrangThaiTocDo _bmi_detect_motion(float accel_filtered) {
  if (accel_filtered > BMI_ACCEL_THRESHOLD) {
    return TOC_DO_TANG_TOC;
  } else if (accel_filtered < -BMI_ACCEL_THRESHOLD) {
    return TOC_DO_GIAM_TOC;
  } else {
    return TOC_DO_DEU_GA;
  }
}

// ─── API CÔNG KHAI ───────────────────────────────────────────────────────────

/**
 * Khởi tạo bus I2C và cảm biến BMI160.
 * Trả về true nếu thành công, false nếu không tìm thấy chip.
 */
static bool BMI160_init() {
  // Khởi tạo I2C
  i2c_config_t conf;
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = (gpio_num_t)BMI_I2C_SDA;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = (gpio_num_t)BMI_I2C_SCL;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = BMI_I2C_FREQ_HZ;
  conf.clk_flags = 0;

  ESP_ERROR_CHECK(i2c_param_config(BMI_I2C_PORT, &conf));
  ESP_ERROR_CHECK(i2c_driver_install(BMI_I2C_PORT, conf.mode, 0, 0, 0));

  // Khởi tạo BMI160
  uint8_t chip_id = 0;
  _bmi_read_regs(BMI_REG_CHIP_ID, &chip_id, 1);
  if (chip_id != 0xD1) {
    Serial.printf("[BMI160] ERROR: Sai CHIP_ID: 0x%02X (Phai la 0xD1)\n", chip_id);
    return false;
  }
  _bmi_write_reg(BMI_REG_CMD, 0x11); delay(100); // Bat accelerometer
  _bmi_write_reg(BMI_REG_CMD, 0x15); delay(100); // Bat gyroscope
  _bmi_write_reg(BMI_REG_ACC_RANGE, 0x03);        // ±2g
  _bmi_write_reg(BMI_REG_GYR_RANGE, 0x00);        // ±2000 dps

  // Khởi tạo vectơ trọng lực G từ cảm biến gia tốc để tránh trôi bộ lọc khi khởi động
  uint8_t raw[12];
  _bmi_read_regs(BMI_REG_GYR_DATA, raw, 12);
  int16_t accel_x_raw = (int16_t)((raw[7]  << 8) | raw[6]);
  int16_t accel_y_raw = (int16_t)((raw[9]  << 8) | raw[8]);
  int16_t accel_z_raw = (int16_t)((raw[11] << 8) | raw[10]);
  _G_x = accel_x_raw / 16384.0f;
  _G_y = accel_y_raw / 16384.0f;
  _G_z = accel_z_raw / 16384.0f;
  Serial.printf("[BMI160] Khoi tao Gravity baseline: Gx=%.3f, Gy=%.3f, Gz=%.3f\n", _G_x, _G_y, _G_z);
  
  _bmi_last_micros = micros(); // Khởi tạo mốc thời gian vi giây

  Serial.println("[BMI160] Khoi tao cam bien thanh cong!");
  return true;
}

/**
 * Đọc dữ liệu cảm biến, tính góc nghiêng, phát hiện sự kiện,
 * và cập nhật modifier. Gọi hàm này định kỳ mỗi 10ms.
 *
 * @param data  Con trỏ tới struct BMI160_Data sẽ được cập nhật.
 */
static void BMI160_update(BMI160_Data *data) {
  // 1. Đọc dữ liệu thô
  uint8_t raw[12];
  _bmi_read_regs(BMI_REG_GYR_DATA, raw, 12);

  int16_t gyro_x_raw  = (int16_t)((raw[1]  << 8) | raw[0]);
  int16_t gyro_y_raw  = (int16_t)((raw[3]  << 8) | raw[2]);
  int16_t gyro_z_raw  = (int16_t)((raw[5]  << 8) | raw[4]);
  int16_t accel_x_raw = (int16_t)((raw[7]  << 8) | raw[6]);
  int16_t accel_y_raw = (int16_t)((raw[9]  << 8) | raw[8]);
  int16_t accel_z_raw = (int16_t)((raw[11] << 8) | raw[10]);

  data->ax = accel_x_raw / 16384.0f;
  data->ay = accel_y_raw / 16384.0f;
  data->az = accel_z_raw / 16384.0f;
  data->gx = gyro_x_raw / 16.4f;
  data->gy = gyro_y_raw / 16.4f;
  data->gz = gyro_z_raw / 16.4f;

  // 2. Tính dt động học giữa các lần gọi hàm
  uint32_t now = micros();
  if (_bmi_last_micros == 0) {
    _bmi_last_micros = now;
  }
  float dt = (float)(now - _bmi_last_micros) / 1000000.0f;
  _bmi_last_micros = now;

  // Giới hạn dt tránh nhảy vọt (outliers) khi khởi động hoặc delay lâu
  if (dt > 0.1f) dt = 0.01f;

  // Dự đoán vectơ trọng lực dựa trên vận tốc xoay từ Gyro (đơn vị rad/s)
  float wx = data->gx * M_PI / 180.0f;
  float wy = data->gy * M_PI / 180.0f;
  float wz = data->gz * M_PI / 180.0f;

  // Công thức tích phân quay: dG/dt = -w x G (tính chéo vectơ)
  float pred_Gx = _G_x + (wz * _G_y - wy * _G_z) * dt;
  float pred_Gy = _G_y + (wx * _G_z - wz * _G_x) * dt;
  float pred_Gz = _G_z + (wy * _G_x - wx * _G_y) * dt;

  // Bù lỗi trôi Gyro bằng dữ liệu gia tốc thực tế (Complementary Filter 3D)
  _G_x = 0.98f * pred_Gx + 0.02f * data->ax;
  _G_y = 0.98f * pred_Gy + 0.02f * data->ay;
  _G_z = 0.98f * pred_Gz + 0.02f * data->az;

  // Tính góc Pitch, Roll mượt mà từ vectơ trọng lực ước lượng
  data->pitch = atan2f(-_G_x, sqrtf(_G_y * _G_y + _G_z * _G_z)) * 180.0f / M_PI;
  data->roll  = atan2f(_G_y, _G_z) * 180.0f / M_PI;

  // 3. Triệt tiêu hoàn toàn trọng lực trên trục Z (hướng di chuyển)
  // Gia tốc động học thực tế = Gia tốc đo được - Vectơ trọng lực ước lượng trục Z
  float dynamic_az = data->az - _G_z;

  // Lọc thông thấp (LPF nhanh) để khử nhiễu tần số cao của gia tốc động học
  float accel_filtered = _bmi_apply_lpf(dynamic_az);
  data->lpf_accel = _bmi_lpf_accel;

  // 4. Phát hiện sự kiện va chạm
  data->su_kien = _bmi_detect_impact(data->ax, data->ay, data->az, data->roll, data->pitch);

  // 5. Phát hiện trạng thái chuyển động
  data->trang_thai_toc = _bmi_detect_motion(accel_filtered);

  // 6. Cập nhật modifier
  switch (data->trang_thai_toc) {
    case TOC_DO_TANG_TOC:  data->modifier = BMI_MODIFIER_BOOST;   break;
    case TOC_DO_GIAM_TOC:  data->modifier = BMI_MODIFIER_BRAKE;   break;
    default:               data->modifier = BMI_MODIFIER_NEUTRAL;  break;
  }

  // 7. Xử lý trạng thái té ngã
  if (data->su_kien == SU_KIEN_TE_NGA) {
    _bmi_dang_bi_te_nga = true;
    _bmi_thoi_diem_te_nga = millis();
  }
  if (_bmi_dang_bi_te_nga && (millis() - _bmi_thoi_diem_te_nga > BMI_FALL_SAFE_HOLD_MS)) {
    _bmi_dang_bi_te_nga = false;
  }
  data->dang_bi_te_nga = _bmi_dang_bi_te_nga;
}

// ─── HÀM TIỆN ÍCH HIỂN THỊ LOG ─────────────────────────────────────────────

static const char* BMI160_impactToString(SuKienVaCham e) {
  switch (e) {
    case SU_KIEN_TE_NGA:   return "TE NGA";
    case SU_KIEN_SOC_O_GA: return "SOC O GA";
    default:                return "Binh thuong";
  }
}

static const char* BMI160_motionToString(TrangThaiTocDo s) {
  switch (s) {
    case TOC_DO_TANG_TOC: return "TANG TOC";
    case TOC_DO_GIAM_TOC: return "GIAM TOC";
    default:               return "DEU GA";
  }
}

/**
 * In dữ liệu BMI160 ra Serial Monitor.
 */
static void BMI160_printLog(const BMI160_Data *data) {
  printf("===== BMI160 SENSOR DATA =====\r\n");
  printf("Accel (g)     : X=%.3f  Y=%.3f  Z=%.3f\r\n", data->ax, data->ay, data->az);
  printf("Gyro (dps)    : X=%.3f  Y=%.3f  Z=%.3f\r\n", data->gx, data->gy, data->gz);
  printf("Goc nghieng   : Pitch=%.2f  Roll=%.2f\r\n", data->pitch, data->roll);
  printf("Trang thai toc: %s (loc LPF=%.3fg)\r\n", BMI160_motionToString(data->trang_thai_toc), data->lpf_accel);
  printf("Su kien va cham: %s\r\n", BMI160_impactToString(data->su_kien));
  printf("===============================\r\n");
}

#endif // BMI160_SENSOR_H
