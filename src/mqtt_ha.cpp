#include "mqtt_ha.h"
#include "config.h"
#include <ArduinoHA.h>

extern HASwitch* pumpSchedules[];
extern HASwitch switchService;
extern HASwitch switchSound;

void setupHA() {
    // ...przenieś całą logikę z main.cpp dotyczącą Home Assistant i MQTT...
}

void onPumpCommand(bool state, HASwitch* sender) {
    // ...przenieś logikę z main.cpp...
}

void onSoundSwitchCommand(bool state, HASwitch* sender) {
    // ...przenieś logikę z main.cpp...
}

void onServiceSwitchCommand(bool state, HASwitch* sender) {
    // ...przenieś logikę z main.cpp...
}
