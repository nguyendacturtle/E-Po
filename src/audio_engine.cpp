#include "audio_engine.h"
#include "config.h"
#include "sound_registry.h"
#include "soc/rtc_io_reg.h"

// ─── BIEN TOAN CUC KET NOI VOI MAIN ──────────────────────────────────────────
int g_currentSoundIndex = 0;

// ─── BIEN NOI BO (Audio Engine) ──────────────────────────────────────────────

static const EngineSoundProfile *g_currentSound = &soundProfiles[0];

volatile uint32_t g_idleStep = 0;   
volatile uint32_t g_revStep = 0;    
volatile uint8_t g_revMix = 0;      
volatile uint8_t g_masterVol = 200; 
volatile bool g_engineOn = false;
volatile uint32_t g_isrCount = 0;

volatile uint8_t g_audioBuf[AUDIO_BUF_SIZE];
volatile int g_bufHead = 0;
volatile int g_bufTail = 0;

volatile bool g_playStart = false;
volatile uint32_t g_startPos = 0; 

static uint32_t s_idlePhase = 0; 
static uint32_t s_revPhase = 0;  

static hw_timer_t *s_timer = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

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

// ─── TIMER ISR (Core 0) ──────────────────────────────────────────────────────
void IRAM_ATTR onTimerISR() {
  g_isrCount++;
  if (!g_engineOn) return;

  if (g_bufTail != g_bufHead) {
    uint32_t val = g_audioBuf[g_bufTail];
    g_bufTail = (g_bufTail + 1) % AUDIO_BUF_SIZE;
    
    // Write directly to DAC registers with a SINGLE write to avoid 0V glitches
    uint32_t reg1 = REG_READ(RTC_IO_PAD_DAC1_REG);
    reg1 = (reg1 & ~RTC_IO_PDAC1_DAC_M) | ((val << RTC_IO_PDAC1_DAC_S) & RTC_IO_PDAC1_DAC_M);
    REG_WRITE(RTC_IO_PAD_DAC1_REG, reg1);
    
    uint32_t reg2 = REG_READ(RTC_IO_PAD_DAC2_REG);
    reg2 = (reg2 & ~RTC_IO_PDAC2_DAC_M) | ((val << RTC_IO_PDAC2_DAC_S) & RTC_IO_PDAC2_DAC_M);
    REG_WRITE(RTC_IO_PAD_DAC2_REG, reg2);
  }
}

// ─── HAM KHOI TAO DAC (Register Level) ───────────────────────────────────────
static void initDACs() {
  dacWrite(DAC_PIN_L, 128);
  dacWrite(DAC_PIN_R, 128);
  
  uint32_t reg1 = REG_READ(RTC_IO_PAD_DAC1_REG);
  reg1 |= RTC_IO_PDAC1_XPD_DAC_M; 
  REG_WRITE(RTC_IO_PAD_DAC1_REG, reg1);

  uint32_t reg2 = REG_READ(RTC_IO_PAD_DAC2_REG);
  reg2 |= RTC_IO_PDAC2_XPD_DAC_M; 
  REG_WRITE(RTC_IO_PAD_DAC2_REG, reg2);
}

// ─── API CONG KHAI CHO MAIN ──────────────────────────────────────────────────

void AudioEngine_init() {
  initDACs();
  AudioEngine_switchSound(0);
  
  // Setup Timer 1
  s_timer = timerBegin(1, 80, true);
  timerAttachInterrupt(s_timer, &onTimerISR, true);
  timerAlarmWrite(s_timer, 1000000UL / g_currentSound->idleSampleRate, true);
  timerAlarmEnable(s_timer);
}

void AudioEngine_switchSound(int index) {
  if (index < 0 || index >= SOUND_PROFILE_COUNT) return;

  portENTER_CRITICAL(&s_mux);
  g_engineOn = false;
  g_currentSoundIndex = index;
  g_currentSound = &soundProfiles[index];

  if (s_timer != NULL) {
    timerAlarmDisable(s_timer);
    timerAlarmWrite(s_timer, 1000000UL / g_currentSound->idleSampleRate, true);
    timerAlarmEnable(s_timer);
  }

  s_idlePhase = 0;
  s_revPhase = 0;
  if (g_playStart) g_startPos = 0;
  portEXIT_CRITICAL(&s_mux);
}

void AudioEngine_fillBuffer() {
  if (!g_engineOn) return;

  while (((g_bufHead + 1) % AUDIO_BUF_SIZE) != g_bufTail) {
    int16_t output = 128;

    if (g_playStart) {
      if (g_currentSound->hasStart && g_startPos < g_currentSound->startSampleCount) {
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

void AudioEngine_update(float currentRPM, float throttle) {
  // RevMix Logic
  uint8_t revMix;
  if (throttle <= REV_SWITCH_POINT) {
    revMix = 0;
  } else if (throttle >= REV_FULL_POINT) {
    revMix = 255;
  } else {
    float t = (throttle - REV_SWITCH_POINT) / (REV_FULL_POINT - REV_SWITCH_POINT);
    revMix = (uint8_t)(t * 255.0f);
  }

  // Volume Logic
  float rpmRatio = (currentRPM - RPM_IDLE) / (RPM_MAX - RPM_IDLE);
  int volPct = VOL_IDLE + (int)(rpmRatio * VOL_ENGINE_DELTA);
  if (throttle < 0.02f) volPct = VOL_IDLE;
  if (volPct > 255) volPct = 255;

  portENTER_CRITICAL(&s_mux);
  g_idleStep = rpmToStep(currentRPM, RPM_IDLE);
  g_revStep = rpmToStep(currentRPM, RPM_IDLE);
  g_revMix = revMix;
  g_masterVol = (uint8_t)volPct;
  portEXIT_CRITICAL(&s_mux);
}

bool AudioEngine_isStartDone() {
  bool done;
  portENTER_CRITICAL(&s_mux);
  done = !g_playStart;
  portEXIT_CRITICAL(&s_mux);
  return done;
}

void AudioEngine_playStart() {
  portENTER_CRITICAL(&s_mux);
  g_startPos = 0;
  g_playStart = true;
  g_masterVol = (uint8_t)(255 * VOL_START / 100);
  g_engineOn = true;
  portEXIT_CRITICAL(&s_mux);
}

void AudioEngine_turnOff() {
  portENTER_CRITICAL(&s_mux);
  g_engineOn = false;
  portEXIT_CRITICAL(&s_mux);
}

uint32_t AudioEngine_getISRCount() {
  return g_isrCount;
}

uint8_t AudioEngine_getMasterVol() {
  return g_masterVol;
}

uint8_t AudioEngine_getRevMix() {
  return g_revMix;
}
