/**
 * =============================================================================
 * E_PO_test_sound — Chế độ BMI160-Only (Biến trở DISABLED)
 * =============================================================================
 * Động cơ tự khởi động ngay khi bật nguồn.
 * Tín hiệu ga hoàn toàn dựa vào gia tốc BMI160:
 *   - Tăng tốc → âm thanh tăng ga
 *   - Giảm tốc → âm thanh giảm ga
 *   - Đi đều   → âm thanh idle nhẹ nhàng suy giảm dần
 */

#include <Arduino.h>
#include <stdio.h>
#include "config.h"
#include "sound_registry.h"
#include "audio_engine.h"
#include "ble_manager.h"
#include "bmi160_sensor.h"

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

// Dữ liệu BMI160 (cập nhật mỗi 10ms)
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
  Serial.println(" E_PO Engine Sound — BMI160-Only Mode");
  Serial.printf(" Loaded %d sound profiles.\n", SOUND_PROFILE_COUNT);
  Serial.printf(" RPM range     : %.0f – %.0f RPM\n", RPM_IDLE, RPM_MAX);
  Serial.println(" [!] Bien tro DISABLED. Dieu khien bang BMI160.");
  Serial.println("=================================================\n");

  if (!BMI160_init()) {
    Serial.println("[CRITICAL] Khoi tao BMI160 that bai!");
  }

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

    // ═══ CẬP NHẬT VIRTUAL THROTTLE TỪ BMI160 ═══
    // Tăng tốc  → tăng ga dần dần
    // Giảm tốc  → giảm ga nhanh
    // Đi đều    → suy giảm tự nhiên về idle
    if (s_state == ENG_RUNNING) {
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

  // 4. State Machine Âm thanh động cơ (Dùng virtualThrottle thay cho biến trở)
  float total_throttle = s_virtualThrottle;

  switch (s_state) {
    case ENG_OFF:
      AudioEngine_turnOff();
      // Tự khởi động lại (Bỏ qua check té ngã)
      {
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
        Serial.println("[ENGINE] Running! (BMI160-Only mode)");
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

  // 5. Log Monitor (Chu kỳ 300ms)
  if (millis() - lastLogMs >= 300) {
    lastLogMs = millis();
    const char *st = (s_state == ENG_OFF) ? "OFF" :
                     (s_state == ENG_STARTING) ? "STARTING" : "RUNNING";

    // In dữ liệu BMI160
    BMI160_printLog(&bmiData);

    // In trạng thái Engine
    char logBuf[160];
    snprintf(logBuf, sizeof(logBuf),
             "[%s] RPM: %5.0f | vTHR: %5.1f%% | MOT: %s | VOL: %3d | RevMix: %3d\n\n",
             st, s_currentRPM, total_throttle * 100.0f,
             BMI160_motionToString(bmiData.trang_thai_toc),
             (unsigned)AudioEngine_getMasterVol(), (unsigned)AudioEngine_getRevMix());

    Serial.print(logBuf);
    BLEManager_notifyLog(logBuf);
  }

  delay(2);
}