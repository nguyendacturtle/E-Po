#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <Arduino.h>

void AudioEngine_init();
void AudioEngine_switchSound(int index);

// Called periodically by the state machine
// throttle is 0.0 to 1.0, currentRPM is the current RPM value
void AudioEngine_update(float currentRPM, float throttle);

// Fill the audio buffer - should be called every loop
void AudioEngine_fillBuffer();

// Check if the starting phase is done
bool AudioEngine_isStartDone();

// Switch to start phase
void AudioEngine_playStart();

// Turn the engine completely off
void AudioEngine_turnOff();

// Get the current ISR count for logging
uint32_t AudioEngine_getISRCount();

// Get master volume for logging
uint8_t AudioEngine_getMasterVol();

// Get rev mix for logging
uint8_t AudioEngine_getRevMix();

#endif // AUDIO_ENGINE_H
