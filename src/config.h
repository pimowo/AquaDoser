#pragma once

#include <Arduino.h>


struct Config {
    char mqtt_server[40];
    uint16_t mqtt_port;
    char mqtt_user[40];
    char mqtt_password[40];
    bool soundEnabled;
    PumpSettings pumps[NUMBER_OF_PUMPS];
    uint32_t checksum;
};

void loadConfig();
void saveConfig();
void setDefaultConfig();
uint32_t calculateChecksum(const Config& cfg);
void resetConfig();
