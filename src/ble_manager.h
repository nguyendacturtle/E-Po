#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>

void BLEManager_init(const char* deviceName);
void BLEManager_process();
void BLEManager_notifyLog(const char* logBuffer);
bool BLEManager_isConnected();

#endif // BLE_MANAGER_H
