// AUTO-GENERATED FILE. DO NOT EDIT.
#pragma once
#include <Arduino.h>

struct EngineSoundProfile {
    const char* name;
    
    const signed char* idleSamples;
    uint32_t idleSampleCount;
    uint32_t idleSampleRate;
    
    const signed char* revSamples;
    uint32_t revSampleCount;
    uint32_t revSampleRate;
    
    const signed char* startSamples;
    uint32_t startSampleCount;
    uint32_t startSampleRate;
    
    bool hasStart;
};

namespace Sound_0 {
  #include "sounds/DefenderV8OpenPipeIdle.h"
  #include "sounds/DefenderV8OpenPipeRev.h"
  #include "sounds/DefenderV8OpenPipeStart.h"
}

namespace Sound_1 {
  #include "sounds/ScaniaV8idle.h"
  #include "sounds/ScaniaV8rev.h"
  #include "sounds/ScaniaV8start.h"
}

namespace Sound_2 {
  #include "sounds/1965FordMustangV8idle.h"
  #include "sounds/1965FordMustangV8rev.h"
  #include "sounds/1965FordMustangV8start.h"
}

namespace Sound_3 {
  #include "sounds/JeepWranglerRubicon392V8Idle.h"
  #include "sounds/JeepWranglerRubicon392V8Rev.h"
  #include "sounds/JeepWranglerRubicon392V8Start.h"
}

namespace Sound_4 {
  #include "sounds/VWBeetleIdle.h"
  #include "sounds/VWBeetleRev.h"
  #include "sounds/VWBeetleStart.h"
}

namespace Sound_5 {
  #include "sounds/HarleyDavidsonFXSBIdle.h"
  #include "sounds/HarleyDavidsonFXSBrev.h"
  #include "sounds/HarleyDavidsonFXSBStart.h"
}

namespace Sound_6 {
  #include "sounds/LaFerrariIdle.h"
  #include "sounds/LaFerrariRev.h"
  #include "sounds/LaFerrariStart.h"
}

namespace Sound_7 {
  #include "sounds/1000HpScaniaV8idle.h"
  #include "sounds/1000HpScaniaV8rev.h"
}

const int SOUND_PROFILE_COUNT = 8;
const EngineSoundProfile soundProfiles[] = {
    { "DefenderV8OpenPipe",
      Sound_0::samples, Sound_0::sampleCount, Sound_0::sampleRate,
      Sound_0::revSamples, Sound_0::revSampleCount, Sound_0::revSampleRate,
      Sound_0::startSamples, Sound_0::startSampleCount, Sound_0::startSampleRate, true },
    { "ScaniaV8",
      Sound_1::samples, Sound_1::sampleCount, Sound_1::sampleRate,
      Sound_1::revSamples, Sound_1::revSampleCount, Sound_1::revSampleRate,
      Sound_1::startSamples, Sound_1::startSampleCount, Sound_1::startSampleRate, true },
    { "1965FordMustangV8",
      Sound_2::samples, Sound_2::sampleCount, Sound_2::sampleRate,
      Sound_2::revSamples, Sound_2::revSampleCount, Sound_2::revSampleRate,
      Sound_2::startSamples, Sound_2::startSampleCount, Sound_2::startSampleRate, true },
    { "JeepWranglerRubicon392V8",
      Sound_3::samples, Sound_3::sampleCount, Sound_3::sampleRate,
      Sound_3::revSamples, Sound_3::revSampleCount, Sound_3::revSampleRate,
      Sound_3::startSamples, Sound_3::startSampleCount, Sound_3::startSampleRate, true },
    { "VwBeetle",
      Sound_4::samples, Sound_4::sampleCount, Sound_4::sampleRate,
      Sound_4::revSamples, Sound_4::revSampleCount, Sound_4::revSampleRate,
      Sound_4::startSamples, Sound_4::startSampleCount, Sound_4::startSampleRate, true },
    { "HarleyDavidsonFXSB",
      Sound_5::samples, Sound_5::sampleCount, Sound_5::sampleRate,
      Sound_5::revSamples, Sound_5::revSampleCount, Sound_5::revSampleRate,
      Sound_5::startSamples, Sound_5::startSampleCount, Sound_5::startSampleRate, true },
    { "LaFerrari",
      Sound_6::samples, Sound_6::sampleCount, Sound_6::sampleRate,
      Sound_6::revSamples, Sound_6::revSampleCount, Sound_6::revSampleRate,
      Sound_6::startSamples, Sound_6::startSampleCount, Sound_6::startSampleRate, true },
    { "1000HpScaniaV8",
      Sound_7::samples, Sound_7::sampleCount, Sound_7::sampleRate,
      Sound_7::revSamples, Sound_7::revSampleCount, Sound_7::revSampleRate,
      nullptr, 0, 0, false },
};
