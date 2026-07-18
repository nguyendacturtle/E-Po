/**
 * =============================================================================
 * E_PO_test_sound — Chế độ Hybrid (IMU 100Hz + GPS 5Hz)
 * =============================================================================
 * Động cơ tự khởi động ngay khi bật nguồn.
 * Tín hiệu ga được phối hợp thông minh:
 *   - Khi dừng xe (GPS = 0 km/h) -> Ga ảo khoá ở 0% (Idle 800 RPM) triệt tiêu trôi IMU.
 *   - Khi di chuyển -> IMU phản hồi tức thì (100Hz) để tăng/giảm ga ảo (VThrottle).
 *   - Vòng tua nền (Base RPM) được nâng lên tương ứng với tốc độ thực tế từ GPS.
 */

#include <Arduino.h>
#include <stdio.h>
#include "config.h"
#include "sound_registry.h"
#include "audio_engine.h"
#include "ble_manager.h"
#include "bmi160_sensor.h"
#include "gps_manager.h"

// ─── ENUM TRẠNG THÁI ĐỘNG CƠ ────────────────────────────────────────────────
enum EngineState : uint8_t {
  ENG_OFF,
  ENG_STARTING,
  ENG_RUNNING
};

// ─── CẤU HÌNH VIRTUAL THROTTLE ──────────────────────────────────────────────
// Tốc độ tăng/giảm của virtual throttle mỗi chu kỳ 10ms
#define VTHROTTLE_RAMP_UP    0.025f   // Tăng ga: 0→100% trong ~400ms
#define VTHROTTLE_RAMP_DOWN  0.015f   // Giảm ga: 100→0% trong ~660ms
#define VTHROTTLE_DECAY      0.005f   // Suy giảm tự nhiên khi đi đều

// ─── BIẾN TOÀN CỤC ──────────────────────────────────────────────────────────
static float s_currentRPM = 0.0f;
static float s_targetRPM  = RPM_IDLE;
static EngineState s_state = ENG_OFF;

// Dữ liệu cảm biến
static BMI160_Data bmiData = {};

// Virtual Throttle: tín hiệu ga ảo điều khiển bởi BMI160
static float s_virtualThrottle = 0.0f;

void requestEngineOff() {
  s_state = ENG_OFF;
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=================================================");
  Serial.println(" E_PO Engine Sound — Hybrid IMU + GPS Mode");
  Serial.printf(" Loaded %d sound profiles.\n", SOUND_PROFILE_COUNT);
  Serial.printf(" RPM range     : %.0f – %.0f RPM\n", RPM_IDLE, RPM_MAX);
  Serial.println("=================================================\n");

  if (!BMI160_init()) {
    Serial.println("[CRITICAL] Khoi tao BMI160 that bai!");
  }

  // Khởi tạo GPS ở tần số 5Hz
  GPS_init();

  AudioEngine_init();

  Serial.println("[BLE] Initializing BLE...");
  BLEManager_init("E_PO Engine Sound");
  Serial.println("[OK] He thong san sang!");

  // ═══ TỰ ĐỘNG KHỞI ĐỘNG ĐỘNG CƠ NGAY LẬP TỨC ═══
  Serial.println("[ENGINE] Tu dong khoi dong...");
  s_currentRPM = 0.0f;
  AudioEngine_playStart();
  s_state = ENG_STARTING;
}

// ─── LOOP ────────────────────────────────────────────────────────────────────
void loop() {
  static uint32_t lastLogMs = 0;
  static uint32_t lastBmiMs = 0;

  AudioEngine_fillBuffer();

  // Đọc serial GPS liên tục (cực kỳ quan trọng để tránh đầy buffer UART)
  GPS_update();

  // 1. Đọc & xử lý BMI160 định kỳ mỗi 10ms
  if (millis() - lastBmiMs >= 10) {
    lastBmiMs = millis();
    BMI160_update(&bmiData);

    // Nếu phát hiện té ngã → tắt động cơ ngay lập tức (TẠM THỜI TẮT)
    /*
    if (bmiData.su_kien == SU_KIEN_TE_NGA) {
      s_state = ENG_OFF;
      s_virtualThrottle = 0.0f;
    }
    */

    // ═══ CẬP NHẬT VIRTUAL THROTTLE PHỐI HỢP CẢ IMU VÀ GPS ═══
    if (s_state == ENG_RUNNING) {
      if (gpsData.valid && gpsData.speed_kmh == 0.0f) {
        // Xe dừng hẳn -> Khóa chặt ga ảo về 0 để triệt tiêu trôi tĩnh từ IMU
        s_virtualThrottle = 0.0f;
      } else {
        // Xe đang di chuyển -> Tăng/giảm ga dựa trên gia tốc động học IMU
        switch (bmiData.trang_thai_toc) {
          case TOC_DO_TANG_TOC:
            s_virtualThrottle += VTHROTTLE_RAMP_UP;
            break;
          case TOC_DO_GIAM_TOC:
            s_virtualThrottle -= VTHROTTLE_RAMP_DOWN;
            break;
          default: // TOC_DO_DEU_GA
            s_virtualThrottle -= VTHROTTLE_DECAY;
            break;
        }
      }
      // Clamp 0.0 ~ 1.0
      if (s_virtualThrottle > 1.0f) s_virtualThrottle = 1.0f;
      if (s_virtualThrottle < 0.0f) s_virtualThrottle = 0.0f;
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
        // Khởi động lại sau khi đổi profile
        s_virtualThrottle = 0.0f;
        s_currentRPM = 0.0f;
        AudioEngine_playStart();
        s_state = ENG_STARTING;
      }
    }
  }

  // 3. Xử lý BLE
  BLEManager_process();

  // 4. State Machine Âm thanh động cơ (Dùng virtualThrottle phối hợp GPS)
  float total_throttle = s_virtualThrottle;

  switch (s_state) {
    case ENG_OFF:
      AudioEngine_turnOff();
      // Tự khởi động lại
      if (!bmiData.dang_bi_te_nga) {
        s_currentRPM = 0.0f;
        s_virtualThrottle = 0.0f;
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
        Serial.println("[ENGINE] Running! (Hybrid GPS + IMU mode)");
      }
      break;

    case ENG_RUNNING:
      // Tính vòng tua nền theo tốc độ thực tế của GPS (Base RPM)
      float gpsMinRPM = RPM_IDLE;
      if (gpsData.valid) {
        gpsMinRPM = RPM_IDLE + (gpsData.speed_kmh / MAX_SPEED_KMH) * (RPM_MAX - RPM_IDLE);
        if (gpsMinRPM > RPM_MAX) gpsMinRPM = RPM_MAX;
      }

      // Vòng tua mục tiêu = RPM nền từ GPS + Tỷ lệ ga từ IMU
      s_targetRPM = gpsMinRPM + total_throttle * (RPM_MAX - gpsMinRPM);

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

  // 5. Log Monitor (Chu kỳ 300ms)
  if (millis() - lastLogMs >= 300) {
    lastLogMs = millis();
    const char *st = (s_state == ENG_OFF) ? "OFF" :
                     (s_state == ENG_STARTING) ? "STARTING" : "RUNNING";

    // In dữ liệu IMU
    BMI160_printLog(&bmiData);

    // In dữ liệu GPS
    char gpsLog[256];
    GPS_printLog(gpsLog, sizeof(gpsLog));
    Serial.print(gpsLog);

    // In trạng thái Engine
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf),
             "[%s] RPM: %5.0f | vTHR: %5.1f%% | MOT: %s | VOL: %3d | RevMix: %3d\n\n",
             st, s_currentRPM, total_throttle * 100.0f,
             BMI160_motionToString(bmiData.trang_thai_toc),
             (unsigned)AudioEngine_getMasterVol(), (unsigned)AudioEngine_getRevMix());

    Serial.print(logBuf);
    
    // Gửi gộp cả log GPS và Engine qua BLE
    char bleLogBuf[512];
    snprintf(bleLogBuf, sizeof(bleLogBuf), "%s%s", gpsLog, logBuf);
    BLEManager_notifyLog(bleLogBuf);
  }

  delay(2);
}