/**
 * =============================================================================
 *  E_PO_test_sound — Mo phong Am Thanh Dong Co cho ESP32
 * =============================================================================
 *  Sound source: TheDIYGuy999/Rc_Engine_Sound_ESP32
 *  Vehicle:      Land Rover Defender V8 Open Pipe
 *
 *  Phan cung:
 *    - ESP32 DevKit
 *    - Bien tro (tay ga gia lap)  -> GPIO 34 (ADC1_CH6)
 *    - Mach khuech dai PAM8610   -> GPIO 25 (DAC1 - kenh L)
 *                                   -> GPIO 26 (DAC2 - kenh R, mirror)
 *    - Loa 3W                     -> ngo ra L+/L- cua PAM8610
 *
 *  Nguon dien:
 *    - PAM8610  : 12V rieng
 *    - ESP32    : 5V (qua Buck / LM2596 tu nguon 12V)
 *    - GND chung: tat ca GND noi chung nhau
 *
 *  Logic (ke thua tu Rc_Engine_Sound_ESP32):
 *    1. Doc ADC bien tro -> throttle [0.0 .. 1.0]
 *    2. Tinh currentRPM voi gia toc / giam toc co inertia
 *    3. Tinh playbackStep (Q16.16 fixed-point) theo RPM
 *    4. ISR Hardware Timer @ 22050 Hz:
 *         - Pha khoi dong: phat startSamples[] mot lan
 *         - Pha chay:      crossfade idle <-> rev theo RPM
 *         - Phat qua dacWrite(25/26, ...)
 * =============================================================================
 */

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <math.h>


// ─── CHON VEHICLE (Runtime) ──────────────────────────────────────────────────
#include "sound_registry.h"

int g_currentSoundIndex = 0;
const EngineSoundProfile *g_currentSound = &soundProfiles[0];

// ─── CAU HINH PHAN CUNG ──────────────────────────────────────────────────────

#define POT_PIN 34   // GPIO34 — ADC1_CH6
#define DAC_PIN_L 25 // GPIO25 — DAC Channel 1 (Left)
#define DAC_PIN_R 26 // GPIO26 — DAC Channel 2 (Right)

// ─── CAU HINH ADC ────────────────────────────────────────────────────────────

#define ADC_MAX_RAW 4095
#define ADC_CLAMP_LOW 80   // cat bo phi tuyen dau thap
#define ADC_CLAMP_HIGH 150 // cat bo phi tuyen dau cao
#define ADC_DEADBAND 60    // vung chet (tranh jitter o 0)

// ─── CAU HINH RPM ────────────────────────────────────────────────────────────

#define RPM_IDLE 800.0f // RPM cam chung
#define RPM_MAX                                                                \
  4500.0f // RPM toi da (tuong duong MAX_RPM_PERCENTAGE=300 cua repo)
#define RPM_ACCEL 12.0f // buoc tang RPM / chu ky 10ms (acc=2 -> doi sang ~12)
#define RPM_DECEL 12.0f // buoc giam RPM / chu ky 10ms (dec=1 -> doi sang ~6)

// Nguong chuyen tu idle sang rev (rev sound bat dau mix vao)
// Tuong duong "revSwitchPoint" cua repo
#define REV_SWITCH_POINT 0.08f // thr > 8% -> bat dau fade in rev
#define REV_FULL_POINT 0.35f   // thr > 35% -> 100% rev sound

// ─── CAU HINH VOLUME ─────────────────────────────────────────────────────────

#define VOL_IDLE 100        // % volume idle
#define VOL_REV 115         // % volume rev (rev thuong to hon)
#define VOL_START 90        // % volume tieng de may
#define VOL_ENGINE_DELTA 60 // % them vao theo throttle (0..60% extra)

// ─── TRANG THAI DONG CO ──────────────────────────────────────────────────────

enum EngineState : uint8_t {
  ENG_OFF,
  ENG_STARTING, // dang phat startSamples[]
  ENG_RUNNING
};

// ─── BIEN TOAN CUC (chia se ISR <-> loop) ────────────────────────────────────

// Fixed-point Q16.16: phan nguyen = so sample nguyen, phan le = noi suy
// Idle channel
volatile uint32_t g_idleStep = 0;   // buoc nhay phase idle (Q16.16)
volatile uint32_t g_revStep = 0;    // buoc nhay phase rev  (Q16.16)
volatile uint8_t g_revMix = 0;      // ty le mix rev: 0=idle, 255=full rev
volatile uint8_t g_masterVol = 200; // volume tong (0..255)
volatile bool g_engineOn = false;
volatile uint32_t g_isrCount = 0;

// ─── AUDIO RING BUFFER ───────────────────────────────────────────────────────
#define AUDIO_BUF_SIZE 2048
volatile uint8_t g_audioBuf[AUDIO_BUF_SIZE];
volatile int g_bufHead = 0;
volatile int g_bufTail = 0;

// Start sound state (chi ISR dung)
volatile bool g_playStart = false;
volatile uint32_t g_startPos = 0; // vi tri doc trong startSamples[]

// Phase accumulators (chi ISR dung)
static uint32_t s_idlePhase = 0; // Q16.16, phan nguyen = index
static uint32_t s_revPhase = 0;  // Q16.16, phan nguyen = index

// Timer
static hw_timer_t *s_timer = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// RPM state (chi loop dung)
static float s_currentRPM = 0.0f;
static float s_targetRPM = RPM_IDLE;
static EngineState s_state = ENG_OFF;

void fillAudioBuffer() {
  if (!g_engineOn) return;

  // Fill up to AUDIO_BUF_SIZE - 1
  while (((g_bufHead + 1) % AUDIO_BUF_SIZE) != g_bufTail) {
    int16_t output = 128; // default to mid-point

    if (g_playStart) {
      if (g_currentSound->hasStart &&
          g_startPos < g_currentSound->startSampleCount) {
        int16_t s = (int16_t)g_currentSound->startSamples[g_startPos];
        output = 128 + (int16_t)((s * (int16_t)g_masterVol) >> 8);
        g_startPos++;
      } else {
        g_playStart = false;
      }
    } else {
      uint32_t currentPhase = s_idlePhase;
      uint16_t idleIdx = (uint16_t)(currentPhase >> 16);
      if (idleIdx >= g_currentSound->idleSampleCount) {
        idleIdx = 0;
        s_idlePhase = 0;
      }
      int16_t idleVal = (int16_t)g_currentSound->idleSamples[idleIdx];

      uint16_t revIdx = (uint16_t)(s_revPhase >> 16);
      if (revIdx >= g_currentSound->revSampleCount) {
        revIdx = 0;
        s_revPhase = 0;
      }
      int16_t revVal = (int16_t)g_currentSound->revSamples[revIdx];

      int16_t mixed = (idleVal * (255 - g_revMix) + revVal * g_revMix) / 255;
      output = 128 + (int16_t)((mixed * (int16_t)g_masterVol) >> 8);

      s_idlePhase += g_idleStep;
      s_revPhase += g_revStep;
    }

    g_audioBuf[g_bufHead] = (uint8_t)output;
    g_bufHead = (g_bufHead + 1) % AUDIO_BUF_SIZE;
  }
}

#include "soc/rtc_io_reg.h"

// ─── TIMER ISR (Core 0) ──────────────────────────────────────────────────────

void IRAM_ATTR onTimerISR() {
  g_isrCount++;
  if (!g_engineOn) return;

  if (g_bufTail != g_bufHead) {
    uint32_t val = g_audioBuf[g_bufTail];
    g_bufTail = (g_bufTail + 1) % AUDIO_BUF_SIZE;
    
    // Write directly to DAC registers with a SINGLE write to avoid 0V glitches
    // DAC1 (GPIO 25)
    uint32_t reg1 = REG_READ(RTC_IO_PAD_DAC1_REG);
    reg1 = (reg1 & ~RTC_IO_PDAC1_DAC_M) | ((val << RTC_IO_PDAC1_DAC_S) & RTC_IO_PDAC1_DAC_M);
    REG_WRITE(RTC_IO_PAD_DAC1_REG, reg1);
    
    // DAC2 (GPIO 26)
    uint32_t reg2 = REG_READ(RTC_IO_PAD_DAC2_REG);
    reg2 = (reg2 & ~RTC_IO_PDAC2_DAC_M) | ((val << RTC_IO_PDAC2_DAC_S) & RTC_IO_PDAC2_DAC_M);
    REG_WRITE(RTC_IO_PAD_DAC2_REG, reg2);
  }
}

// ─── HAM TINH PLAYBACK STEP (Q16.16) tu RPM ──────────────────────────────────
static uint32_t rpmToStep(float rpm, float rpmRef) {
  if (rpm < 1.0f || rpmRef < 1.0f)
    return 0x10000; // 1x speed
  float ratio = rpm / rpmRef;
  uint32_t step = (uint32_t)(ratio * 65536.0f);
  if (step < 0x2000)
    step = 0x2000; // min 1/8x speed
  if (step > 0x80000)
    step = 0x80000; // max 8x speed
  return step;
}

// ─── HAM DOC ADC (oversampling 8x) ───────────────────────────────────────────

static int readPotSmooth() {
  int32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(POT_PIN);
    delayMicroseconds(30);
  }
  int avg = (int)(sum >> 3);

  avg -= ADC_CLAMP_LOW;
  if (avg < 0)
    avg = 0;

  int maxUsable = ADC_MAX_RAW - ADC_CLAMP_LOW - ADC_CLAMP_HIGH;
  if (avg > maxUsable)
    avg = maxUsable;

  return avg;
}

// ─── HAM TINH THROTTLE [0.0 .. 1.0] ─────────────────────────────────────────

static float adcToThrottle(int adcVal) {
  int maxUsable = ADC_MAX_RAW - ADC_CLAMP_LOW - ADC_CLAMP_HIGH;
  if (adcVal < ADC_DEADBAND)
    return 0.0f;
  float t = (float)(adcVal - ADC_DEADBAND) / (float)(maxUsable - ADC_DEADBAND);
  if (t > 1.0f)
    t = 1.0f;
  return t;
}

// ─── SETUP ───────────────────────────────────────────────────────────────────

void switchSound(int index) {
  if (index < 0 || index >= SOUND_PROFILE_COUNT)
    return;

  portENTER_CRITICAL(&s_mux);
  s_state = ENG_OFF;
  g_engineOn = false;
  g_currentSoundIndex = index;
  g_currentSound = &soundProfiles[index];

  if (s_timer != NULL) {
    timerAlarmDisable(s_timer);
    timerAlarmWrite(s_timer, 1000000UL / g_currentSound->idleSampleRate, true);
    timerAlarmEnable(s_timer);
  }
  portEXIT_CRITICAL(&s_mux);

  Serial.printf("\n[SYSTEM] Switched to sound S%d: %s\n", index + 1,
                g_currentSound->name);
}

// ─── BLE SETUP ───────────────────────────────────────────────────────────────

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Device connected");
  };

  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Device disconnected");
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();

    if (rxValue.length() > 0) {
      Serial.print("[BLE] Received: ");
      Serial.println(rxValue);

      rxValue.trim();
      if (rxValue.startsWith("S") || rxValue.startsWith("s")) {
        int index = rxValue.substring(1).toInt() - 1;
        switchSound(index);
      }
    }
  }
};

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=================================================");
  Serial.println(" E_PO Engine Sound — ESP32 + PAM8610");
  Serial.printf(" Loaded %d sound profiles.\n", SOUND_PROFILE_COUNT);
  Serial.println(" Type S1, S2, S3... to switch sounds.");
  Serial.printf(" RPM range     : %.0f – %.0f RPM\n", RPM_IDLE, RPM_MAX);
  Serial.println("=================================================");

  switchSound(0); // Load default

  Serial.println(" Xoay bien tro de khoi dong dong co...");

  // ── ADC ──
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(POT_PIN, INPUT);

  // ── DAC: xuat mid-point tranh tieng pop ──
  dacWrite(DAC_PIN_L, 128);
  dacWrite(DAC_PIN_R, 128);

  // ── BLE Setup ──
  Serial.println("[BLE] Initializing BLE...");
  BLEDevice::init("E-PO");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);

  pRxCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  pServer->getAdvertising()->start();
  Serial.println("[OK] BLE Started. Waiting for connections...");

  // ── Hardware Timer  // ── Thiet lap Timer 1 ──
  s_timer = timerBegin(1, 80, true);
  timerAttachInterrupt(s_timer, &onTimerISR, true);
  timerAlarmWrite(s_timer, 1000000UL / g_currentSound->idleSampleRate, true);
  timerAlarmEnable(s_timer);

  Serial.println("[OK] Timer ISR armed.");
  Serial.flush();

  s_state = ENG_OFF;
  g_engineOn = false;
}

// ─── LOOP (Core 1) ───────────────────────────────────────────────────────────

void loop() {
  static uint32_t lastLogMs = 0;

  // ── Xu ly lenh Serial ──
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("S") || cmd.startsWith("s")) {
      int index = cmd.substring(1).toInt() - 1;
      switchSound(index);
    }
  }

  // ── Xu ly BLE Reconnect ──
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); 
    pServer->startAdvertising(); 
    Serial.println("[BLE] Restart advertising");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // ── Doc ADC ──
  int adcVal = readPotSmooth();
  float throttle = adcToThrottle(adcVal);

  // ────────────────────────────────────────────────────────────────────────────
  switch (s_state) {

  // ── TẮT MAY ────────────────────────────────────────────────────────────────
  case ENG_OFF:
    portENTER_CRITICAL(&s_mux);
    g_engineOn = false;
    portEXIT_CRITICAL(&s_mux);

    if (throttle > 0.05f) {
      Serial.println("[ENGINE] Cranking...");
      s_currentRPM = 0.0f;

      portENTER_CRITICAL(&s_mux);
      g_startPos = 0;
      g_playStart = true;
      g_masterVol = (uint8_t)(255 * VOL_START / 100);
      g_engineOn = true;
      portEXIT_CRITICAL(&s_mux);

      s_state = ENG_STARTING;
    }
    break;

  // ── DANG DE MAY ────────────────────────────────────────────────────────────
  case ENG_STARTING: {
    bool startDone;
    portENTER_CRITICAL(&s_mux);
    startDone = !g_playStart;
    portEXIT_CRITICAL(&s_mux);

    if (startDone) {
      Serial.println("[ENGINE] Running!");
      s_currentRPM = RPM_IDLE;
      s_targetRPM = RPM_IDLE;

      portENTER_CRITICAL(&s_mux);
      s_idlePhase = 0;
      s_revPhase = 0;
      g_idleStep = rpmToStep(RPM_IDLE, RPM_IDLE);
      g_revStep = rpmToStep(RPM_IDLE, RPM_IDLE);
      g_revMix = 0;
      g_masterVol = (uint8_t)(255 * VOL_IDLE / 100);
      portEXIT_CRITICAL(&s_mux);

      s_state = ENG_RUNNING;
    }
    break;
  }

  // ── DANG CHAY ──────────────────────────────────────────────────────────────
  case ENG_RUNNING: {
    s_targetRPM = RPM_IDLE + throttle * (RPM_MAX - RPM_IDLE);

    if (s_currentRPM < s_targetRPM) {
      float accel = RPM_ACCEL * (1.0f + throttle * 1.5f);
      s_currentRPM += accel;
      if (s_currentRPM > s_targetRPM)
        s_currentRPM = s_targetRPM;
    } else {
      s_currentRPM -= RPM_DECEL;
      if (s_currentRPM < s_targetRPM)
        s_currentRPM = s_targetRPM;
    }
    // Tinh revMix: 0 = full idle, 255 = full rev
    uint8_t revMix;
    if (throttle <= REV_SWITCH_POINT) {
      revMix = 0;
    } else if (throttle >= REV_FULL_POINT) {
      revMix = 255;
    } else {
      float t =
          (throttle - REV_SWITCH_POINT) / (REV_FULL_POINT - REV_SWITCH_POINT);
      revMix = (uint8_t)(t * 255.0f);
    }

    // Tinh volume: tang theo throttle
    float rpmRatio = (s_currentRPM - RPM_IDLE) / (RPM_MAX - RPM_IDLE);
    // Volume tang dan theo RPM va throttle
    int volPct = VOL_IDLE + (int)(rpmRatio * VOL_ENGINE_DELTA);
    if (throttle < 0.02f)
      volPct = VOL_IDLE; // giu nguyen volume khi tay ga = 0
    if (volPct > 255)
      volPct = 255;

    portENTER_CRITICAL(&s_mux);
    g_idleStep = rpmToStep(s_currentRPM, RPM_IDLE);
    g_revStep = rpmToStep(s_currentRPM, RPM_IDLE);
    g_revMix = revMix;
    g_masterVol = (uint8_t)volPct;
    portEXIT_CRITICAL(&s_mux);
    break;
  }

  } // end switch

  // Luon goi ham dien day bo dem am thanh
  fillAudioBuffer();

  // ── Serial log moi 300 ms ──
  if (millis() - lastLogMs >= 300) {
    lastLogMs = millis();
    const char *st = (s_state == ENG_OFF)        ? "OFF"
                     : (s_state == ENG_STARTING) ? "STARTING"
                                                 : "RUNNING";
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "[%s] RPM: %5.0f | THR: %5.1f%% | VOL: %3d | RevMix: %3d | ISR: %lu\n", st,
             s_currentRPM, throttle * 100.0f, (unsigned)g_masterVol,
             (unsigned)g_revMix, g_isrCount);

    Serial.print(logBuf);

    if (deviceConnected) {
      pTxCharacteristic->setValue((uint8_t *)logBuf, strlen(logBuf));
      pTxCharacteristic->notify();
    }
  }

  delay(10); // chu ky dieu khien 10ms
}

/*
 * =============================================================================
 *  HUONG DAN COPY SOUND FILES VAO DU AN
 * =============================================================================
 *
 *  1. Tao thu muc:
 *       E_PO_test_sound/src/sounds/
 *
 *  2. Copy cac file sau tu:
 *       ../Rc_Engine_Sound_ESP32/src/vehicles/sounds/
 *     Vao:
 *       src/sounds/
 *
 *     File can copy cho Defender V8 Open Pipe:
 *       DefenderV8OpenPipeIdle.h
 *       DefenderV8OpenPipeRev.h
 *       DefenderV8OpenPipeStart.h
 *
 *  3. De dung vehicle khac, thay doi 3 dong #include o tren va copy
 *     cac file .h tuong ung. Vi du:
 *
 *     Scania V8:
 *       #include "sounds/ScaniaV8idle.h"
 *       #include "sounds/ScaniaV8rev.h"
 *       #include "sounds/ScaniaV8start.h"
 *
 *     CAT 3408:
 *       #include "sounds/3408CatIdle.h"
 *       #include "sounds/3408CatRev.h"
 *       (khong co start file -> comment dong start)
 *
 *     La Ferrari:
 *       #include "sounds/LaFerrariIdle.h"
 *       #include "sounds/LaFerrariRev.h"
 *       #include "sounds/LaFerrariStart.h"
 *
 *  4. Build va nap:
 *       pio run --target upload
 *
 *  NOTE: Sound file la signed char (-128..127), sample rate 22050 Hz.
 *        ISR se tu dong xu ly pitch-shift va crossfade idle/rev.
 * =============================================================================
 */
