#ifndef CONFIG_H
#define CONFIG_H

// ─── CHON VEHICLE (Runtime) ──────────────────────────────────────────────────
// Number of sound profiles is defined in sound_registry.h

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
#define RPM_MAX 4500.0f // RPM toi da (tuong duong MAX_RPM_PERCENTAGE=300 cua repo)
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

// ─── AUDIO RING BUFFER ───────────────────────────────────────────────────────
#define AUDIO_BUF_SIZE 2048

#endif // CONFIG_H
