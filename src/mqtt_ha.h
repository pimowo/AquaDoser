#pragma once

void setupHA();
void onPumpCommand(bool state, HASwitch* sender);
void onSoundSwitchCommand(bool state, HASwitch* sender);
void onServiceSwitchCommand(bool state, HASwitch* sender);
