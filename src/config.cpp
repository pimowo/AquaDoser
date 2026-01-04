#include "config.h"
#include <EEPROM.h>
#include <cstring>

extern Config config;

void loadConfig() {
    EEPROM.begin(512);
    EEPROM.get(0, config);
    EEPROM.end();
    // Podstawowa walidacja - jeśli dane są nieprawidłowe, załaduj domyślne
    if (config.mqtt_port > 65535) {
        setDefaultConfig();
        saveConfig();
    }
}

void saveConfig() {
    EEPROM.begin(512);
    EEPROM.put(0, config);
    EEPROM.commit();
    EEPROM.end();
}

void setDefaultConfig() {
    // Domyślne ustawienia MQTT
    strlcpy(config.mqtt_server, "", sizeof(config.mqtt_server));
    config.mqtt_port = 1883;
    strlcpy(config.mqtt_user, "", sizeof(config.mqtt_user));
    strlcpy(config.mqtt_password, "", sizeof(config.mqtt_password));
    config.soundEnabled = true;
    // Domyślne ustawienia pomp
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        config.pumps[i].status = 0;
        config.pumps[i].hour = 8;
        config.pumps[i].minute = 0;
        config.pumps[i].flow = 0;
        config.pumps[i].flowDec = 0;
        config.pumps[i].volume = 0;
        config.pumps[i].volumeDec = 0;
        config.pumps[i].days = 0;
        sprintf(config.pumps[i].name, "Pompa %d", i+1);
    }
}

uint32_t calculateChecksum(const Config&) { return 0; }
void resetConfig() {}
