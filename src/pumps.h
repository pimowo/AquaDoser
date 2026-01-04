#pragma once

void setupPump();
void setLEDDosing(uint8_t pumpIndex);
void setLEDActive(uint8_t pumpIndex);
void setLEDInactive(uint8_t pumpIndex);
void setLEDService(uint8_t pumpIndex);
void initializePCF();
void updatePumpState(int pumpIndex, bool state);
void publishCalibrationDate(int pumpIndex);
bool isDayEnabled(byte day, int pumpIndex);
