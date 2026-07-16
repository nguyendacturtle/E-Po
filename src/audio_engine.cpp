#include "audio_engine.h"
#include "config.h"
#include "sound_registry.h"
#include <driver/i2s.h>

#define I2S_PORT I2S_NUM_0

// ─── BIEN TOAN CUC KET NOI VOI MAIN ──────────────────────────────────────────
int g_currentSoundIndex = 0;

// ─── BIEN NOI BO (Audio Engine) ──────────────────────────────────────────────

static const EngineSoundProfile *g_currentSound = &soundProfiles[0];

volatile uint32_t g_idleStep = 0;
volatile uint32_t g_revStep = 0;
volatile uint8_t g_revMix = 0;
volatile uint8_t g_masterVol = 200;
volatile bool g_engineOn = false;
uint32_t g_isrCount = 0; // Repurposed for DMA sample count

volatile bool g_playStart = false;
volatile uint32_t g_startPos = 0;

static uint32_t s_idlePhase = 0;
static uint32_t s_revPhase = 0;

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

// ─── API CONG KHAI CHO MAIN ──────────────────────────────────────────────────

void AudioEngine_init() {
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = 22050, // Se duoc set lai trong AudioEngine_switchSound
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 512,
      .use_apll = false,
      .tx_desc_auto_clear = true};

  i2s_pin_config_t pin_config = {.bck_io_num = I2S_BCLK,
                                 .ws_io_num = I2S_LRC,
                                 .data_out_num = I2S_DOUT,
                                 .data_in_num = I2S_PIN_NO_CHANGE};

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);

  AudioEngine_switchSound(0);
}

void AudioEngine_switchSound(int index) {
  if (index < 0 || index >= SOUND_PROFILE_COUNT)
    return;

  portENTER_CRITICAL(&s_mux);
  g_engineOn = false;
  g_currentSoundIndex = index;
  g_currentSound = &soundProfiles[index];

  // Cap nhat sample rate cho I2S
  i2s_set_sample_rates(I2S_PORT, g_currentSound->idleSampleRate);
  i2s_zero_dma_buffer(I2S_PORT);

  s_idlePhase = 0;
  s_revPhase = 0;
  if (g_playStart)
    g_startPos = 0;
  portEXIT_CRITICAL(&s_mux);
}

void AudioEngine_fillBuffer() {
  const int CHUNK_SIZE = 512;
  // Khai bao mang stereo: moi khung (frame) gom 2 mau Left va Right
  int16_t samples[CHUNK_SIZE * 2];

  if (!g_engineOn) {
    // Write silence to keep I2S DMA happy and prevent static/noise
    memset(samples, 0, sizeof(samples));
    size_t bytes_written = 0;
    i2s_write(I2S_PORT, samples, sizeof(samples), &bytes_written,
              portMAX_DELAY);
    return;
  }

  for (int i = 0; i < CHUNK_SIZE; i++) {
    int16_t output = 0;

    if (g_playStart) {
      if (g_currentSound->hasStart &&
          g_startPos < g_currentSound->startSampleCount) {
        int8_t s = (int8_t)(g_currentSound->startSamples[g_startPos] - 128);
        output = (int16_t)(s * (int16_t)g_masterVol);
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
      int8_t idleVal = (int8_t)(g_currentSound->idleSamples[idleIdx] - 128);

      uint16_t revIdx = (uint16_t)(s_revPhase >> 16);
      if (revIdx >= g_currentSound->revSampleCount) {
        revIdx = 0;
        s_revPhase = 0;
      }
      int8_t revVal = (int8_t)(g_currentSound->revSamples[revIdx] - 128);

      int16_t mixed = (idleVal * (255 - g_revMix) + revVal * g_revMix) / 255;
      output = (int16_t)(mixed * (int16_t)g_masterVol);

      s_idlePhase += g_idleStep;
      s_revPhase += g_revStep;
    }

    // Nhan doi mau ra ca 2 kenh Left va Right de ho tro MAX98357A toi uu
    samples[i * 2] = output;     // Kênh Trái (Left)
    samples[i * 2 + 1] = output; // Kênh Phải (Right)
  }

  size_t bytes_written = 0;
  i2s_write(I2S_PORT, samples, sizeof(samples), &bytes_written, portMAX_DELAY);
  g_isrCount += CHUNK_SIZE;
}

void AudioEngine_update(float currentRPM, float throttle) {
  // RevMix Logic
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

  // Volume Logic
  float rpmRatio = (currentRPM - RPM_IDLE) / (RPM_MAX - RPM_IDLE);
  int volPct = VOL_IDLE + (int)(rpmRatio * VOL_ENGINE_DELTA);
  if (throttle < 0.02f)
    volPct = VOL_IDLE;
  if (volPct > 255)
    volPct = 255;

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

uint32_t AudioEngine_getISRCount() { return g_isrCount; }

uint8_t AudioEngine_getMasterVol() { return g_masterVol; }

uint8_t AudioEngine_getRevMix() { return g_revMix; }
