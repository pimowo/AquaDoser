// --- Biblioteki
#include <Wire.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ArduinoHA.h>
#include <DS3231.h>
#include <PCF8574.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include <TimeLib.h>      // Do obsługi czasu
#include <functional>     // Dla std::bind

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// Definicje stałych
#define MQTT_SERVER "twój_serwer_mqtt"
#define MQTT_PORT 1883
#define MQTT_USER "użytkownik"
#define MQTT_PASSWORD "hasło"

// Tablica nazw miesięcy do parsowania daty
const char *nazwyMiesiecy[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// --- EEPROM
#define EEPROM_OFFSET_CONFIG 0 // Offset 0 dla konfiguracji MQTT
#define EEPROM_OFFSET_CALIBRATION 80 // Offset 80 dla kalibracji pomp
#define EEPROM_OFFSET_DOSE_AMOUNT 144 // Offset 144 dla ilości dawki
#define EEPROM_OFFSET_ACTIVE_DAYS 208 // Offset 208 dla dni aktywności pomp
#define EEPROM_OFFSET_ACTIVE_HOURS 272 // Offset 272 dla godzin aktywności pomp

// --- Pompy
#define NUM_PUMPS 8
#define DEFAULT_CALIBRATION 0.5 // Domyślna kalibracja pomp (ml/s)

// --- LED
#define LED_PIN D1 // Pin danych dla WS2812

// --- Przycisk
#define BUTTON_PIN D2 // Pin przycisku serwisowego
#define DEBOUNCE_TIME 50 // Czas debounce w ms

// --- Kolory LED
#define COLOR_OFF 0xFF0000 // Czerwony (pompa wyłączona)
#define COLOR_ON 0x00FF00  // Zielony (pompa włączona)
#define COLOR_WORKING 0x0000FF // Niebieski (pompa pracuje)

// --- Częstotliwość synchronizacji RTC
#define RTC_SYNC_INTERVAL 86400000 // 24 godziny (w ms)

// --- Obiekty systemowe
WiFiClient wifiClient;               // Obiekt do obsługi połączenia Wi-Fi
HADevice haDevice("AquaDoser");      // Obiekt reprezentujący urządzenie w Home Assistant
HAMqtt mqtt(wifiClient, haDevice);   // Obiekt do komunikacji MQTT
DS3231 rtc;                          // Obiekt RTC (Real Time Clock) DS3231
PCF8574 pcf8574(0x20);               // Obiekt do obsługi portu I2C PCF8574
Adafruit_NeoPixel strip(NUM_PUMPS, LED_PIN, NEO_GRB + NEO_KHZ800); // Obiekt do obsługi taśmy LED

// --- Globalne zmienne
float calibrationData[NUM_PUMPS];   // Tablica danych kalibracyjnych dla pomp
float doseAmount[NUM_PUMPS];         // Tablica ilości podawanych nawozów dla pomp
unsigned long doseStartTime[NUM_PUMPS]; // Tablica czasów rozpoczęcia dozowania dla pomp
int dosingDuration[NUM_PUMPS];       // Tablica czasu dozowania dla każdej pompy
bool pumpEnabled[NUM_PUMPS] = {true}; // Tablica stanu włączenia dla pomp (domyślnie włączone)
bool pumpRunning[NUM_PUMPS] = {false}; // Tablica stanu pracy dla pomp (domyślnie zatrzymane)
bool serviceMode = false;             // Flaga trybu serwisowego
unsigned long lastRTCUpdate = 0;     // Ostatni czas aktualizacji RTC
unsigned long lastAnimationUpdate = 0; // Ostatni czas aktualizacji animacji LED
uint8_t currentAnimationStep = 0;     // Bieżący krok animacji LED
uint8_t activeDays[NUM_PUMPS];       // Tablica aktywnych dni tygodnia dla pomp
uint8_t activeHours[NUM_PUMPS];      // Tablica aktywnych godzin dla pomp

// Deklaracje wskaźników do funkcji obsługi
typedef void (*PumpSwitchHandler)(bool state, HASwitch* sender);
typedef void (*CalibrationHandler)(HANumeric value, HANumber* sender);

// Tablice funkcji obsługi
PumpSwitchHandler pumpHandlers[NUM_PUMPS];
CalibrationHandler calibrationHandlers[NUM_PUMPS];

// --- Definicje encji Home Assistant dla każdej pompy i trybu serwisowego
HASwitch* pumpSwitch[NUM_PUMPS];      // Przełącznik dla włączania/wyłączania harmonogramu pompy
HASensor* pumpWorkingSensor[NUM_PUMPS]; // Sensor dla stanu pracy pompy (on/off)
HASensor* tankLevelSensor[NUM_PUMPS];  // Sensor dla poziomu wirtualnego zbiornika
HANumber* calibrationNumber[NUM_PUMPS]; // Liczba dla kalibracji pompy
HASwitch serviceModeSwitch("service_mode"); // Przełącznik dla trybu serwisowego
HASensor rtcAlarmSensor("rtc_alarm"); // Definicja sensora alarmu RTC

// --- Ustawienia konfiguracyjne dla połączenia MQTT
struct Config {
  char mqttServer[40]; // Adres serwera MQTT (maksymalnie 39 znaków + null terminator)
  char mqttUser[20]; // Nazwa użytkownika MQTT (maksymalnie 19 znaków + null terminator)
  char mqttPassword[20]; // Hasło użytkownika MQTT (maksymalnie 19 znaków + null terminator)
} 
config; // Inicjalizacja zmiennej konfiguracyjnej

// --- Konfiguracja menedżera Wi-Fi
void setupWiFiManager() {
  WiFiManager wifiManager; // Inicjalizacja obiektu WiFiManager

  // Ustawienia punktu dostępowego, jeśli nie połączono się z siecią Wi-Fi
  wifiManager.setAPCallback([](WiFiManager *wm) {
    Serial.println("Nie można połączyć z Wi-Fi. Tryb konfiguracji AP."); // Komunikat informujący o trybie AP
  });

  // Pola dodatkowe dla konfiguracji MQTT
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", config.mqttServer, 40); // Pole dla serwera MQTT
  WiFiManagerParameter custom_mqtt_user("user", "MQTT User", config.mqttUser, 20); // Pole dla użytkownika MQTT
  WiFiManagerParameter custom_mqtt_password("password", "MQTT Password", config.mqttPassword, 20); // Pole dla hasła MQTT

  // Dodanie pól do WiFiManager
  wifiManager.addParameter(&custom_mqtt_server); // Dodanie pola serwera
  wifiManager.addParameter(&custom_mqtt_user); // Dodanie pola użytkownika
  wifiManager.addParameter(&custom_mqtt_password); // Dodanie pola hasła

  // Uruchom konfigurator i dodaj ESP.wdtFeed() w pętli oczekiwania
  if (!wifiManager.autoConnect("AquaDoserAP")) { // Próba połączenia z Wi-Fi
    Serial.println("Nie udało się połączyć z Wi-Fi. Resetuję..."); // Komunikat o błędzie
    ESP.restart(); // Restart urządzenia w przypadku niepowodzenia
  }

  // Jeśli połączono, zapisz ustawienia MQTT z formularza
  strncpy(config.mqttServer, custom_mqtt_server.getValue(), sizeof(config.mqttServer)); // Zapis serwera MQTT
  strncpy(config.mqttUser, custom_mqtt_user.getValue(), sizeof(config.mqttUser)); // Zapis użytkownika MQTT
  strncpy(config.mqttPassword, custom_mqtt_password.getValue(), sizeof(config.mqttPassword)); // Zapis hasła MQTT
  saveConfig(); // Zapisanie konfiguracji do EEPROM

  Serial.println("Połączono z Wi-Fi"); // Komunikat o udanym połączeniu
  Serial.print("MQTT Server: "); // Wyświetlenie adresu serwera MQTT
  Serial.println(config.mqttServer);
  Serial.print("MQTT User: "); // Wyświetlenie nazwy użytkownika MQTT
  Serial.println(config.mqttUser);

  ESP.wdtFeed(); // Odśwież watchdog po zakończeniu konfiguracji
}

// Funkcje obsługi przełączników pomp
void handlePumpSwitch0(bool state, HASwitch* sender) {
    pumpEnabled[0] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitch1(bool state, HASwitch* sender) {
    pumpEnabled[1] = state;
    saveSettings();
    updateHAStates();
}

// Funkcje obsługi kalibracji
void handleCalibration0(HANumeric value, HANumber* sender) {
    calibrationData[0] = value.toInt8();
    saveSettings();
    updateHAStates();
}

void handleCalibration1(HANumeric value, HANumber* sender) {
    calibrationData[1] = value.toInt8();
    saveSettings();
    updateHAStates();
}

// Tylko jedna definicja funkcji handleActiveDaysCommand1
void handleActiveDaysCommand1(HANumeric value, HANumber* sender) {
    activeDays[1] = value.toInt8();
    saveSettings();
    updateHAStates();
}

// --- Konfiguracja połączenia z brokerem MQTT oraz definiuje encje dla sensorów i przełączników
void setupMQTT() {
  char uniqueId[16];
  // Próbujemy połączyć się z brokerem MQTT
  if (!mqtt.begin(config.mqttServer, config.mqttUser, config.mqttPassword)) {
    Serial.println("Blad! Nie mozna polaczyc z brokerem MQTT");
  }
  
  mqtt.begin(config.mqttServer, config.mqttUser, config.mqttPassword); // Inicjalizacja MQTT z danymi konfiguracyjnymi

  // Definicja sensora alarmu RTC w Home Assistant
  rtcAlarmSensor.setName("Alarm RTC"); // Ustawienie nazwy sensora
  rtcAlarmSensor.setIcon("mdi:alarm"); // Ustawienie ikony sensora
  
  // Konfiguracja encji dla trybu serwisowego
  serviceModeSwitch.setName("Serwis"); // Ustawienie nazwy przełącznika trybu serwisowego
  serviceModeSwitch.setIcon("mdi:wrench"); // Ustawienie ikony przełącznika
  serviceModeSwitch.onCommand([](bool state, HASwitch* sender) { // Ustawienie akcji na zmianę stanu przełącznika
    serviceMode = state; // Zmiana stanu trybu serwisowego
    toggleServiceMode(); // Przełączenie trybu serwisowego
  });

  // Konfiguracja encji dla każdej pompy
  for (int i = 0; i < NUM_PUMPS; i++) {
    setupPumpEntities(i); // Wywołanie funkcji konfigurującej encje dla danej pompy
  }

  // Dodanie konfiguracji dni i godzin pracy dla każdej pompy
  for (int i = 0; i < NUM_PUMPS; i++) {
    snprintf(uniqueId, sizeof(uniqueId), "active_days_%d", i);
    HANumber* days = new HANumber(uniqueId, HANumber::PrecisionP0);
    
    switch (i) {
      case 0: days->onCommand(handleActiveDaysCommand0); break;
      case 1: days->onCommand(handleActiveDaysCommand1); break;
      case 2: days->onCommand(handleActiveDaysCommand2); break;
      case 3: days->onCommand(handleActiveDaysCommand3); break;
      case 4: days->onCommand(handleActiveDaysCommand4); break;
      case 5: days->onCommand(handleActiveDaysCommand5); break;
      case 6: days->onCommand(handleActiveDaysCommand6); break;
      case 7: days->onCommand(handleActiveDaysCommand7); break;
    }
  }
}

// --- Konfiguracja encji dla poszczególnych pomp w systemie Home Assistant


// Funkcja konfiguracji elementów pompy
void setupPumpEntities(int pumpIndex) {
    char uniqueId[32];

    // Konfiguracja przełącznika pompy
    snprintf(uniqueId, sizeof(uniqueId), "pump_switch_%d", pumpIndex);
    pumpSwitch[pumpIndex] = new HASwitch(uniqueId);
    pumpSwitch[pumpIndex]->setName(("Pompa " + String(pumpIndex + 1) + " Harmonogram").c_str());
    pumpSwitch[pumpIndex]->setIcon("mdi:power");
    
    // Przypisanie odpowiedniego callbacka
    if (pumpIndex == 0) {
        pumpSwitch[pumpIndex]->onCommand(handlePumpSwitch0);
    } else {
        pumpSwitch[pumpIndex]->onCommand(handlePumpSwitch1);
    }

    // Konfiguracja kalibracji
    snprintf(uniqueId, sizeof(uniqueId), "calibration_%d", pumpIndex);
    calibrationNumber[pumpIndex] = new HANumber(uniqueId, HANumber::PrecisionP1);
    calibrationNumber[pumpIndex]->setName(("Kalibracja Pompy " + String(pumpIndex + 1)).c_str());
    calibrationNumber[pumpIndex]->setIcon("mdi:tune");
    
    // Przypisanie odpowiedniego callbacka
    if (pumpIndex == 0) {
        calibrationNumber[pumpIndex]->onCommand(handleCalibration0);
    } else {
        calibrationNumber[pumpIndex]->onCommand(handleCalibration1);
    }
}

// --- Inicjalizacja modułu RTC DS3231 oraz ustawienia czasu
void setupRTC() {
    Wire.begin();
    
    // Sprawdzenie komunikacji z RTC
    Wire.beginTransmission(0x68);
    if (Wire.endTransmission() != 0) {
        Serial.println("Błąd! Nie można znaleźć modułu RTC DS3231");
        serviceMode = true;
        rtcAlarmSensor.setValue("ALARM RTC");
        updateHAStates();
        return;
    }

    // Sprawdzenie czy RTC działa
    if (!rtc.getSecond()) {  // Jeśli odczyt się nie powiedzie
        Serial.println("RTC utracił zasilanie, ustawiam czas...");
        
        // Parsowanie czasu kompilacji
        tmElements_t tm;
        if (rozlozDate(__DATE__) && rozlozTime(__TIME__)) {
            rtc.setClockMode(false);  // tryb 24h
            rtc.setSecond(second());
            rtc.setMinute(minute());
            rtc.setHour(hour());
            rtc.setDate(day());
            rtc.setMonth(month());
            rtc.setYear(year() - 2000);
        }
    }
}

// Funkcje pomocnicze do parsowania czasu
bool rozlozTime(const char *str) {
    int godzina, minuta, sekunda;
    if (sscanf(str, "%d:%d:%d", &godzina, &minuta, &sekunda) != 3) return false;
    setTime(godzina, minuta, sekunda, day(), month(), year());
    return true;
}

bool rozlozDate(const char *str) {
    char miesiac[12];
    int dzien, rok;
    uint8_t indeksMiesiaca;

    if (sscanf(str, "%s %d %d", miesiac, &dzien, &rok) != 3) return false;
    for (indeksMiesiaca = 0; indeksMiesiaca < 12; indeksMiesiaca++) {
        if (strcmp(miesiac, nazwyMiesiecy[indeksMiesiaca]) == 0) break;
    }
    if (indeksMiesiaca >= 12) return false;
    setTime(hour(), minute(), second(), dzien, indeksMiesiaca + 1, rok);
    return true;
}

void setupNTP() {
    timeClient.begin();
    timeClient.setTimeOffset(3600); // Ustaw odpowiedni offset czasowy
}

void synchronizeRTC() {
    if (timeClient.update()) {
        unsigned long epochTime = timeClient.getEpochTime();
        
        // Konwersja czasu Unix na komponenty
        time_t rawTime = (time_t)epochTime;
        struct tm * timeinfo = localtime(&rawTime);
        
        rtc.setClockMode(false); // 24h mode
        rtc.setSecond(timeinfo->tm_sec);
        rtc.setMinute(timeinfo->tm_min);
        rtc.setHour(timeinfo->tm_hour);
        rtc.setDate(timeinfo->tm_mday);
        rtc.setMonth(timeinfo->tm_mon + 1);
        rtc.setYear(timeinfo->tm_year - 100);
        
        Serial.println("Czas zsynchronizowany z NTP");
    }
}

// --- Konfiguracja diody LED
void setupLED() {
  strip.begin(); // Inicjalizacja taśmy LED
  strip.show(); // Wyłączenie wszystkich LED na początek
  // Ta funkcja ustawia wszystkie diody na kolor wyłączony, co jest przydatne do resetu stanu taśmy LED
}

void handleActiveDaysCommand(float value, HANumber* sender, int index) {
    activeDays[index] = static_cast<uint8_t>(value);
    saveSettings();
    updateHAStates();
}

void handleActiveHoursCommand(float value, HANumber* sender, int index) {
    activeHours[index] = static_cast<uint8_t>(value);
    saveSettings();
    updateHAStates();
}

// --- Zarządzanie działaniem pomp w oparciu o harmonogram
void handlePumps() {
    if (serviceMode) return;

    bool h12, PM;
    for (int i = 0; i < NUM_PUMPS; i++) {
        int dayOfWeek = rtc.getDoW();  // Zmieniono getDayOfWeek na getDoW
        int hour = rtc.getHour(h12, PM);  // Poprawione wywołanie getHour
        
        if (pumpEnabled[i] && pumpRunning[i] && 
            (millis() - doseStartTime[i] >= dosingDuration[i] * 1000)) {
            stopDosing(i);
        } 
        else if (pumpEnabled[i] && 
                (activeDays[i] & (1 << dayOfWeek)) && 
                activeHours[i] == hour && 
                !pumpRunning[i]) {
            startDosing(i);
        }
    }
}

void handleActiveDaysCommand0(HANumeric value, HANumber* sender) {
    activeDays[0] = value.toInt8();  // Zamiast static_cast używamy metody toInt8
    saveSettings();
    updateHAStates();
}

void handleActiveDaysCommand2(HANumeric value, HANumber* sender) {
  activeDays[2] = value.toInt8();
  saveSettings();
  updateHAStates();
}

void handleActiveDaysCommand3(HANumeric value, HANumber* sender) {
  activeDays[3] = value.toInt8();
  saveSettings();
  updateHAStates();
}

void handleActiveDaysCommand4(HANumeric value, HANumber* sender) {
  activeDays[4] = value.toInt8();
  saveSettings();
  updateHAStates();
}

void handleActiveDaysCommand5(HANumeric value, HANumber* sender) {
  activeDays[5] = value.toInt8();
  saveSettings();
  updateHAStates();
}

void handleActiveDaysCommand6(HANumeric value, HANumber* sender) {
  activeDays[6] = value.toInt8();
  saveSettings();
  updateHAStates();
}

void handleActiveDaysCommand7(HANumeric value, HANumber* sender) {
  activeDays[7] = value.toInt8();
  saveSettings();
  updateHAStates();
}

void handlePumpSwitchCommand0(bool state, HASwitch* sender) {
    pumpEnabled[0] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitchCommand1(bool state, HASwitch* sender) {
    pumpEnabled[1] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitchCommand2(bool state, HASwitch* sender) {
    pumpEnabled[2] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitchCommand3(bool state, HASwitch* sender) {
    pumpEnabled[3] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitchCommand4(bool state, HASwitch* sender) {
    pumpEnabled[4] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitchCommand5(bool state, HASwitch* sender) {
    pumpEnabled[5] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitchCommand6(bool state, HASwitch* sender) {
    pumpEnabled[6] = state;
    saveSettings();
    updateHAStates();
}

void handlePumpSwitchCommand7(bool state, HASwitch* sender) {
    pumpEnabled[7] = state;
    saveSettings();
    updateHAStates();
}

// --- Rozpoczęcie podawania nawozu dla określonej pompy
void startDosing(int pumpIndex) {
  dosingDuration[pumpIndex] = doseAmount[pumpIndex] / calibrationData[pumpIndex];
  pumpRunning[pumpIndex] = true;
  doseStartTime[pumpIndex] = millis();

  pcf8574.write(pumpIndex, HIGH); // Alternatywna metoda

  strip.setPixelColor(pumpIndex, COLOR_WORKING);
  strip.show();

  Serial.print("Pompa ");
  Serial.print(pumpIndex + 1);
  Serial.println(" rozpoczela podawanie");
}

// --- Zatrzymanie podawania nawozu dla określonej pompy
void stopDosing(int pumpIndex) {
    pumpRunning[pumpIndex] = false;
    pcf8574.write(pumpIndex, LOW);  // Zmieniono digitalWrite na write
    strip.setPixelColor(pumpIndex, COLOR_ON);
    strip.show();
    
    Serial.print("Pompa ");
    Serial.print(pumpIndex + 1);
    Serial.println(" zakończyła podawanie.");
}

// --- Obsługa przycisku
void handleButton() {
  static bool lastButtonState = LOW; // Ostatni stan przycisku (początkowo LOW)
  static unsigned long lastDebounceTime = 0; // Ostatni czas zmiany stanu przycisku

  bool currentButtonState = digitalRead(BUTTON_PIN); // Odczyt aktualnego stanu przycisku
  if (currentButtonState != lastButtonState) { // Sprawdzenie, czy stan przycisku się zmienił
    lastDebounceTime = millis(); // Zapisanie czasu zmiany stanu
  }

  // Sprawdzenie, czy upłynął czas debouncingu
  if ((millis() - lastDebounceTime) > DEBOUNCE_TIME) {
    if (currentButtonState == HIGH) { // Sprawdzenie, czy przycisk jest naciśnięty
      toggleServiceMode(); // Przełącz tryb serwisowy
    }
  }

  lastButtonState = currentButtonState; // Aktualizacja ostatniego stanu przycisku
}

// --- Przełączanie trybu serwisowego
void toggleServiceMode() {
  serviceMode = !serviceMode; // Zmiana stanu trybu serwisowego (włączenie/wyłączenie)  
  updateHAStates(); // Synchronizacja stanu trybu serwisowego w Home Assistant
}

// --- Animacja LED
void showLEDAnimation() {
  static unsigned long lastUpdate = 0; // Ostatni czas aktualizacji animacji
  if (millis() - lastUpdate < 250) return; // Odczekaj 250ms przed aktualizacją

  strip.clear(); // Wyłączenie wszystkich LED
  if (!serviceMode) { // Sprawdzenie, czy nie jesteśmy w trybie serwisowym
    for (int i = 0; i < NUM_PUMPS; i++) {
      // Ustal kolor diody w zależności od kroku animacji
      uint32_t color = (currentAnimationStep % 3 == 0) ? COLOR_WORKING : 
                       (currentAnimationStep % 3 == 1) ? COLOR_OFF : 
                       COLOR_ON;
      strip.setPixelColor(i, color); // Ustawienie koloru diody
    }
  } else {
    // W trybie serwisowym podświetlenie tylko jednej diody
    strip.setPixelColor(currentAnimationStep % NUM_PUMPS, COLOR_WORKING);
  }
  strip.show(); // Aktualizacja diod LED
  currentAnimationStep++; // Przejście do następnego kroku animacji
  lastUpdate = millis(); // Zaktualizowanie czasu ostatniej aktualizacji
}

// --- Synchronizacja RTC
void updateRTC() {
  if (millis() - lastRTCUpdate >= RTC_SYNC_INTERVAL) {
    synchronizeRTC(); // Wywołanie synchronizacji RTC
    lastRTCUpdate = millis();
  }
}

// --- Odczyt ustawień
void loadSettings() {
  // Odczyt ustawień z EEPROM z ustalonym offsetem
  for (int i = 0; i < NUM_PUMPS; i++) {
    EEPROM.get(EEPROM_OFFSET_CALIBRATION + i * sizeof(float), calibrationData[i]);
    EEPROM.get(EEPROM_OFFSET_DOSE_AMOUNT + i * sizeof(float), doseAmount[i]);
    EEPROM.get(EEPROM_OFFSET_ACTIVE_DAYS + i, activeDays[i]);
    EEPROM.get(EEPROM_OFFSET_ACTIVE_HOURS + i, activeHours[i]);
    ESP.wdtFeed(); // Odśwież watchdog po odczycie każdej sekcji
  }
}

// --- Zapis ustawień
void saveSettings() {
  bool settingsChanged = false;

  for (int i = 0; i < NUM_PUMPS; i++) {
    float newCalibration = calibrationData[i];
    float newDoseAmount = doseAmount[i];
    uint8_t newActiveDays = activeDays[i];
    uint8_t newActiveHours = activeHours[i];

    float currentCalibration;
    float currentDoseAmount;
    uint8_t currentActiveDays;
    uint8_t currentActiveHours;

    // Odczyt aktualnych wartości z EEPROM
    EEPROM.get(EEPROM_OFFSET_CALIBRATION + i * sizeof(float), currentCalibration);
    EEPROM.get(EEPROM_OFFSET_DOSE_AMOUNT + i * sizeof(float), currentDoseAmount);
    EEPROM.get(EEPROM_OFFSET_ACTIVE_DAYS + i, currentActiveDays);
    EEPROM.get(EEPROM_OFFSET_ACTIVE_HOURS + i, currentActiveHours);

    // Sprawdzenie, czy kalibracja się zmieniła
    if (currentCalibration != newCalibration) {
      EEPROM.put(EEPROM_OFFSET_CALIBRATION + i * sizeof(float), newCalibration);
      settingsChanged = true;
    }

    // Sprawdzenie, czy ilość dawki się zmieniła
    if (currentDoseAmount != newDoseAmount) {
      EEPROM.put(EEPROM_OFFSET_DOSE_AMOUNT + i * sizeof(float), newDoseAmount);
      settingsChanged = true;
    }

    // Sprawdzenie, czy dni aktywności się zmieniły
    if (currentActiveDays != newActiveDays) {
      EEPROM.put(EEPROM_OFFSET_ACTIVE_DAYS + i, newActiveDays);
      settingsChanged = true;
    }

    // Sprawdzenie, czy godziny aktywności się zmieniły
    if (currentActiveHours != newActiveHours) {
      EEPROM.put(EEPROM_OFFSET_ACTIVE_HOURS + i, newActiveHours);
      settingsChanged = true;
    }

    // Odśwież watchdog po odczycie każdej sekcji
    ESP.wdtFeed();
  }

  // Zapisuj do EEPROM tylko, jeśli były zmiany
  if (settingsChanged) {
    EEPROM.commit();
    Serial.println("Ustawienia zostały zapisane do EEPROM.");
  }
}

// --- Odczyt konfiguracji MQTT z EEPROM
void loadConfig() {
  // Odczyt konfiguracji MQTT z EEPROM na zdefiniowanym offsetcie
  EEPROM.get(EEPROM_OFFSET_CONFIG, config);
}

// --- Zapis konfiguracji MQTT do EEPROM
void saveConfig() {
  EEPROM.put(EEPROM_OFFSET_CONFIG, config); // Zapis konfiguracji MQTT do EEPROM na zdefiniowanym offsetcie
  EEPROM.commit(); // Potwierdzenie zapisu do EEPROM, aby zapewnić, że dane zostały zapisane
}

// --- Aktualizacja stanu w Home Assistant
void updateHAStates() {
    serviceModeSwitch.setState(serviceMode);
    
    for (int i = 0; i < NUM_PUMPS; i++) {
        pumpSwitch[i]->setState(pumpEnabled[i]);
        pumpWorkingSensor[i]->setValue(pumpRunning[i] ? "ON" : "OFF");
        tankLevelSensor[i]->setValue(String(doseAmount[i]).c_str());
        
        // Używamy setValue z wartością numeryczną bezpośrednio
        calibrationNumber[i]->setState(calibrationData[i]);
    }
}

// --- Konfiguracja 
void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  Wire.begin();

    // Inicjalizacja funkcji obsługi dla wszystkich pomp
    for (int i = 0; i < NUM_PUMPS; i++) {
        pumpHandlers[i] = nullptr;
        calibrationHandlers[i] = nullptr;
    }

  loadConfig(); // Wczytanie konfiguracji MQTT z EEPROM
  setupWiFiManager(); // Konfiguracja i połączenie z Wi-Fi  
  setupMQTT(); // Konfiguracja MQTT z Home Assistant  
  setupRTC(); // Inicjalizacja RTC   
  setupLED(); // Konfiguracja diod LED

    // Konfiguracja wszystkich pomp
    for (int i = 0; i < NUM_PUMPS; i++) {
        setupPumpEntities(i);
    }

  pinMode(BUTTON_PIN, INPUT_PULLUP); // Konfiguracja przycisku serwisowego 
  loadSettings(); // Wczytanie ustawień pomp z EEPROM  
  lastRTCUpdate = millis(); // Ustawienie początkowego czasu synchronizacji RTC 
  ESP.wdtEnable(WDTO_8S); // Inicjalizacja watchdog timer (8s)
}

// --- Główna pętla programu
void loop() {
  // Sprawdzenie, czy MQTT jest połączone
    if (!mqtt.isConnected()) {
        mqtt.begin(MQTT_SERVER, MQTT_PORT, MQTT_USER, MQTT_PASSWORD);
    }
  
  mqtt.loop(); // Obsługuje komunikację z MQTT

  // Obsługa pomp, ale tylko jeśli nie jesteśmy w trybie serwisowym
  if (!serviceMode) {
    handlePumps(); // Sprawdzanie stanu pomp i ich działania
  }

  updateRTC(); // Synchronizacja czasu RTC
  handleButton(); // Sprawdzenie stanu przycisku serwisowego
  showLEDAnimation(); // Animacja LED
  ESP.wdtFeed(); // Odświeżanie watchdog timer
}
// --- KONIEC ---
