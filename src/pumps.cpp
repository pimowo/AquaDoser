#include "pumps.h"
#include "config.h"
#include <Arduino.h>

extern Config config;

void setLEDActive(uint8_t pumpIndex) {
    setPumpLED(pumpIndex, COLOR_ACTIVE);
}

void setLEDDosing(uint8_t pumpIndex) {
    setPumpLED(pumpIndex, COLOR_DOSING);
}

void setLEDService(uint8_t pumpIndex) {
    setPumpLED(pumpIndex, COLOR_SERVICE);
}

void setLEDCalibration(uint8_t pumpIndex) {
    setPumpLED(pumpIndex, COLOR_CALIBRATION);
}

void setAllLEDsService() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        setLEDService(i);
    }
}

void restoreNormalLEDs() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumpStates[i].isRunning) {
            setLEDDosing(i);
        } else {
            config.pumps[i].status ? setLEDActive(i) : setLEDInactive(i);
        }
    }
}

void initializePCF() {
    if (!pcf8574.begin()) {
        Serial.println("Could not initialize PCF8574");
        return;
    }
    for (uint8_t i = 0; i < 8; i++) {
        pcf8574.digitalWrite(i, HIGH);
    }
}

void setupPump() {}
void updatePumpState(int pumpIndex, bool state) {}
void publishCalibrationDate(int pumpIndex) {}
bool isDayEnabled(byte day, int pumpIndex) { return true; }
