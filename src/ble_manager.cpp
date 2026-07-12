#include "ble_manager.h"
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "audio_engine.h"
#include "sound_registry.h"

// ─── UUIDs ───────────────────────────────────────────────────────────────────
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID_TX "8248c894-3a2d-425b-801e-2df19d08ee72"
#define CHARACTERISTIC_UUID_RX "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static BLEServer *pServer = NULL;
static BLECharacteristic *pTxCharacteristic = NULL;
static BLECharacteristic *pRxCharacteristic = NULL;

static bool deviceConnected = false;
static bool oldDeviceConnected = false;

extern int g_currentSoundIndex;

// ─── BLE CALLBACKS ───────────────────────────────────────────────────────────

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Device connected");
  }
  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Device disconnected");
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = String(pCharacteristic->getValue().c_str());
    if (rxValue.length() > 0) {
      Serial.print("[BLE] Received: ");
      Serial.println(rxValue.c_str());

      // Parse commands like "S1", "S2"
      if (rxValue.startsWith("S") || rxValue.startsWith("s")) {
        int idx = rxValue.substring(1).toInt() - 1;
        if (idx >= 0 && idx < SOUND_PROFILE_COUNT) {
          Serial.printf("[SYSTEM] Switched to sound S%d: %s\n", idx + 1, soundProfiles[idx].name);
          AudioEngine_switchSound(idx);
          extern void requestEngineOff();
          requestEngineOff();
        } else {
          Serial.printf("[ERROR] Invalid sound index: %d\n", idx + 1);
        }
      }
    }
  }
};

// ─── PUBLIC API ──────────────────────────────────────────────────────────────

void BLEManager_init(const char* deviceName) {
  BLEDevice::init(deviceName);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();
  pServer->getAdvertising()->start();
}

void BLEManager_process() {
  // Xu ly ngat ket noi de bat lai advertise
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // cho Bluetooth stack san sang
    pServer->startAdvertising();
    Serial.println("[BLE] Restarted advertising");
    oldDeviceConnected = deviceConnected;
  }
  // Xu ly khi co thiet bi moi ket noi vao
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
}

void BLEManager_notifyLog(const char* logBuffer) {
  if (deviceConnected && pTxCharacteristic) {
    pTxCharacteristic->setValue((uint8_t *)logBuffer, strlen(logBuffer));
    pTxCharacteristic->notify();
  }
}

bool BLEManager_isConnected() {
  return deviceConnected;
}
