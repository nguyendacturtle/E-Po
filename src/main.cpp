/**
 * =============================================================================
 *  E_PO_test_sound — Mo phong Am Thanh Dong Co cho ESP32
 * =============================================================================
 *  Modularized Version
 * =============================================================================
 */

#include <Arduino.h>
#include "config.h"
#include "sound_registry.h"
#include "audio_engine.h"
#include "ble_manager.h"

// ─── TRANG THAI DONG CO ──────────────────────────────────────────────────────
enum EngineState : uint8_t {
  ENG_OFF,
  ENG_STARTING, 
  ENG_RUNNING
};

static float s_currentRPM = 0.0f;
static float s_targetRPM = RPM_IDLE;
static EngineState s_state = ENG_OFF;

void requestEngineOff() {
  s_state = ENG_OFF;
}

// ─── HAM DOC ADC ───────────────────────────────────────────────────────────
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
  Serial.println(" E_PO Engine Sound \u2014 ESP32 + PAM8610");
  Serial.printf(" Loaded %d sound profiles.\n", SOUND_PROFILE_COUNT);
  Serial.println(" Type S1, S2, S3... to switch sounds.");
  Serial.printf(" RPM range     : %.0f \u2013 %.0f RPM\n", RPM_IDLE, RPM_MAX);
  Serial.println("=================================================\n");

  AudioEngine_init();

  Serial.println("[BLE] Initializing BLE...");
  BLEManager_init("E_PO Engine Sound");
  Serial.println("[OK] BLE Started. Waiting for connections...");
  Serial.println("[OK] Timer ISR armed.");
}

// ─── LOOP ────────────────────────────────────────────────────────────────────
void loop() {
  static uint32_t lastLogMs = 0;

  // 1. Process Serial Commands
  if (Serial.available()) {
    String rxValue = Serial.readStringUntil('\n');
    rxValue.trim();
    if (rxValue.length() > 0 && (rxValue.startsWith("S") || rxValue.startsWith("s"))) {
      int idx = rxValue.substring(1).toInt() - 1;
      if (idx >= 0 && idx < SOUND_PROFILE_COUNT) {
        Serial.printf("[SYSTEM] Switched to sound S%d: %s\n", idx + 1, soundProfiles[idx].name);
        AudioEngine_switchSound(idx);
        s_state = ENG_OFF;
      } else {
        Serial.printf("[ERROR] Invalid sound index: %d\n", idx + 1);
      }
    }
  }

  // 2. Process BLE
  BLEManager_process();

  // 3. Read Throttle
  int adcVal = readPotSmooth();
  float throttle = adcToThrottle(adcVal);

  // 4. State Machine
  switch (s_state) {
    case ENG_OFF:
      AudioEngine_turnOff();
      if (throttle > 0.05f) {
        Serial.println("[ENGINE] Cranking...");
        s_currentRPM = 0.0f;
        AudioEngine_playStart();
        s_state = ENG_STARTING;
      }
      break;

    case ENG_STARTING:
      if (AudioEngine_isStartDone()) {
        Serial.println("[ENGINE] Running!");
        s_currentRPM = RPM_IDLE;
        s_targetRPM = RPM_IDLE;
        AudioEngine_update(s_currentRPM, 0.0f); // Init
        s_state = ENG_RUNNING;
      }
      break;

    case ENG_RUNNING:
      s_targetRPM = RPM_IDLE + throttle * (RPM_MAX - RPM_IDLE);

      if (s_currentRPM < s_targetRPM) {
        float accel = RPM_ACCEL * (1.0f + throttle * 1.5f);
        s_currentRPM += accel;
        if (s_currentRPM > s_targetRPM) s_currentRPM = s_targetRPM;
      } else {
        s_currentRPM -= RPM_DECEL;
        if (s_currentRPM < s_targetRPM) s_currentRPM = s_targetRPM;
      }

      AudioEngine_update(s_currentRPM, throttle);
      break;
  }

  // 5. Fill Audio Buffer (Must be called continuously!)
  AudioEngine_fillBuffer();

  // 6. Print Logs
  if (millis() - lastLogMs >= 300) {
    lastLogMs = millis();
    const char *st = (s_state == ENG_OFF) ? "OFF" : 
                     (s_state == ENG_STARTING) ? "STARTING" : "RUNNING";
    
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "[%s] RPM: %5.0f | THR: %5.1f%% | VOL: %3d | RevMix: %3d | ISR: %lu\n", st,
             s_currentRPM, throttle * 100.0f, (unsigned)AudioEngine_getMasterVol(),
             (unsigned)AudioEngine_getRevMix(), AudioEngine_getISRCount());

    Serial.print(logBuf);
    BLEManager_notifyLog(logBuf);
  }

  delay(10); // 10ms control cycle
}
