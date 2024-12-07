// ** BIBLIOTEKI **

// Biblioteki podstawowe
#include <Arduino.h>  // Podstawowa biblioteka Arduino - zawiera funkcje rdzenia (pinMoc:\Users\piotrek\Documents\c:\Users\piotrek\Documents\Arduino\AquaDoser\config.inoArduino\AquaDoser_5_12_24\data\index.htmlde, digitalRead itp.)
#include <Wire.h>     // Biblioteka do komunikacji I2C (TWI) - wymagana do obsługi PCF8574

// Biblioteki do komunikacji z Home Assistant
#include <ArduinoHA.h>  // Integracja z Home Assistant przez MQTT - automatyzacja, sensory, przełączniki
#include <PCF8574.h>    // Obsługa ekspandera I/O PCF8574 - dodaje 8 wyjść cyfrowych przez I2C

// Biblioteki do obsługi WiFi i aktualizacji OTA
#include <ESP8266WiFi.h>              // Podstawowa obsługa WiFi dla ESP8266
#include <WiFiManager.h>              // Zarządzanie WiFi - portal konfiguracyjny, automatyczne połączenie
#include <ArduinoOTA.h>               // Aktualizacja firmware przez sieć (Over The Air)
#include <ESP8266HTTPUpdateServer.h>  // Aktualizacja firmware przez przeglądarkę (poprzez stronę WWW)

// Biblioteki do interfejsu webowego
#include <ESP8266WebServer.h>  // Serwer HTTP - obsługa strony konfiguracyjnej
#include <WebSocketsServer.h>  // WebSocket - komunikacja w czasie rzeczywistym ze stroną WWW
#include <EEPROM.h>            // Dostęp do pamięci nieulotnej - zapisywanie konfiguracji
#include <LittleFS.h>

// Zegar
#include <RTClib.h>
#include <TimeLib.h>
#include <Timezone.h>  // dla obsługi czasu letniego/zimowego

// Pozostałe
#include <Adafruit_NeoPixel.h>  // Sterowanie LED

// 0x68 DS3231
// 0x57 eeprom
// 0x20 pcf8574

// Struktury konfiguracyjne i statusowe

const uint8_t NUMBER_OF_PUMPS = 8;  // Ilość pomp
// Globalne zmienne
PumpState pumpStates[NUMBER_OF_PUMPS];
uint8_t pumpStateByte = 0; // Byte do przechowywania stanu wszystkich pomp

struct PumpSettings {
    byte status;     // 0 - wyłączona, 1 - włączona
    byte hour;       // godzina dozowania (0-23)
    byte minute;     // minuta dozowania (0-59)
    byte flow;       // przepływ w ml/min (wartość całkowita)
    byte flowDec;    // część dziesiętna przepływu (0-9)
    byte volume;     // objętość w ml (wartość całkowita)
    byte volumeDec;  // część dziesiętna objętości (0-9)
    byte days;       // bity dni tygodnia (bit 0 = niedziela, bit 6 = sobota)
    char name[32];   // nazwa pompy
};

// Stan pracy pompy
struct PumpState {
    bool isRunning;
    unsigned long startTime;
    unsigned long duration;
};

// Główna struktura konfiguracji
struct Config {
    char mqtt_server[40];
    uint16_t mqtt_port;
    char mqtt_user[40];
    char mqtt_password[40];
    bool soundEnabled;
    PumpSettings pumps[NUMBER_OF_PUMPS];
};

// Struktura do przechowywania różnych stanów i parametrów systemu
struct Status {
    PumpStatus pumps[NUMBER_OF_PUMPS];
    bool isServiceMode;
    bool pumpSafetyLock;
    bool soundEnabled;
    unsigned long pumpStartTime;
    unsigned long pumpDelayStartTime;
    unsigned long lastSoundAlert;
    unsigned long lastSuccessfulMeasurement;
};

// Stan przycisku
struct ButtonState {
  bool lastState;  // Poprzedni stan przycisku
  bool isInitialized = false;
  bool isLongPressHandled = false;  // Flaga obsłużonego długiego naciśnięcia
  unsigned long pressedTime = 0;    // Czas wciśnięcia przycisku
  unsigned long releasedTime = 0;   // Czas puszczenia przycisku
};

// Timery dla różnych operacji
struct Timers {
  unsigned long lastOTACheck;     // ostatnie sprawdzenie aktualizacji
  unsigned long lastPumpCheck;    // ostatnie sprawdzenie harmonogramu
  unsigned long lastStateUpdate;  // ostatnia aktualizacja stanu do HA
  unsigned long lastButtonCheck;  // ostatnie sprawdzenie przycisku

  // Konstruktor inicjalizujący wszystkie timery na 0
  Timers()
    : lastOTACheck(0),
      lastPumpCheck(0),
      lastStateUpdate(0),
      lastButtonCheck(0) {}
};

struct CustomTimeStatus {
  String time;
  String date;
  String season;
};

// Stan LED
struct LEDState {
  uint32_t currentColor;
  uint32_t targetColor;
  unsigned long lastUpdateTime;
  bool immediate;
  uint8_t brightness;
  bool pulsing;
  int8_t pulseDirection;
  int8_t pulseUp;

  LEDState()
    : currentColor(0), targetColor(0), lastUpdateTime(0),
      immediate(false), brightness(255), pulsing(false),
      pulseDirection(1) {}
};

// Deklaracja funkcji
static CustomTimeStatus getCustomTimeStatus() {
  CustomTimeStatus status;
  status.time = String(hour()) + ":" + String(minute()) + ":" + String(second());
  status.date = String(day()) + "/" + String(month()) + "/" + String(year());

  int currentMonth = month();
  if (currentMonth >= 3 && currentMonth <= 5) status.season = "Wiosna";
  else if (currentMonth >= 6 && currentMonth <= 8) status.season = "Lato";
  else if (currentMonth >= 9 && currentMonth <= 11) status.season = "Jesień";
  else status.season = "Zima";

  return status;
}


// Zadeklaruj zmienną globalną dla trybu serwisowego
bool isServiceMode = false;

// Tablica do przechowywania stanów pomp
bool pumpEnabled[NUMBER_OF_PUMPS] = {false}; // Inicjalizacja wszystkich na false


// ** DEFINICJE PINÓW **

// Przypisanie pinów do urządzeń

const int BUTTON_PIN = 14;  // Przycisk
const int BUZZER_PIN = 13;  // Dzwięk
const int LED_PIN = 12;     // LED 
const int SCL_PIN = 5;      // SCL
const int SDA_PIN = 4;      // SDA

PCF8574 pcf8574(0x20);

// LED
// Definicja paska LED
Adafruit_NeoPixel strip(NUMBER_OF_PUMPS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Kolory
const uint32_t COLOR_INACTIVE = strip.Color(255, 0, 0);    // Czerwony - pompa nieaktywna
const uint32_t COLOR_ACTIVE = strip.Color(0, 255, 0);      // Zielony - pompa aktywna
const uint32_t COLOR_DOSING = strip.Color(0, 0, 255);      // Niebieski - dozowanie
const uint32_t COLOR_SERVICE = strip.Color(255, 165, 0);   // Pomarańczowy - tryb serwisowy
const uint32_t COLOR_CALIBRATION = strip.Color(255, 0, 255); // Fioletowy - kalibracja

// Stan LED-ów dla każdej pompy
uint32_t pumpColors[NUMBER_OF_PUMPS];

// ** USTAWIENIA CZASOWE **

// Konfiguracja timeoutów i interwałów
const unsigned long WATCHDOG_TIMEOUT = 8000;                          // Timeout dla watchdoga
const unsigned long LONG_PRESS_TIME = 1000;                           // Czas długiego naciśnięcia przycisku
const unsigned long OTA_CHECK_INTERVAL = 1000;                        // Sprawdzanie OTA co 1s
const unsigned long MILLIS_OVERFLOW_THRESHOLD = 4294967295U - 60000;  // ~49.7 dni

void updateHAState(uint8_t pumpIndex);

// zmienne globalne do śledzenia testu pompy

unsigned long pumpTestEndTime = 0;
int8_t testingPumpId = -1;

// RTC
RTC_DS3231 rtc;

// Reguły dla czasu letniego w Polsce
// Ostatnia niedziela marca o 2:00 -> 3:00
// Ostatnia niedziela października o 3:00 -> 2:00
TimeChangeRule CEST = {"CEST", Last, Sun, Mar, 2, 120};  // UTC + 2h
TimeChangeRule CET = {"CET", Last, Sun, Oct, 3, 60};     // UTC + 1h
Timezone CE(CEST, CET);

unsigned long lastNTPSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 24UL * 60UL * 60UL * 1000UL;  // 24h w milisekundach

// ** KONFIGURACJA SYSTEMU **

// Makra debugowania
#define DEBUG 1  // 0 wyłącza debug, 1 włącza debug

#if DEBUG
#define AQUA_DEBUG_PRINT(x) Serial.println(x)
#define AQUA_DEBUG_PRINTF(format, ...) Serial.printf(format "\n", ##__VA_ARGS__)

#else
#define AQUA_DEBUG_PRINT(x)
#define AQUA_DEBUG_PRINTF(format, ...)
#endif

// Zmienna przechowująca wersję oprogramowania
const char* SOFTWARE_VERSION = "7.12.24";  // Definiowanie wersji oprogramowania

// Globalne instancje struktur
//CustomTimeStatus currentStatus = getCustomTimeStatus();
Config config;
Status status;
ButtonState buttonState;
Timers timers;
CustomTimeStatus currentStatus;
WiFiManager wifiManager;
LEDState ledStates[NUMBER_OF_PUMPS];  // Stan diod LED
unsigned long lastLedUpdate = 0;

// ** INSTANCJE URZĄDZEŃ I USŁUG **

// Serwer HTTP i WebSockets
ESP8266WebServer server(80);     // Tworzenie instancji serwera HTTP na porcie 80
WebSocketsServer webSocket(81);  // Tworzenie instancji serwera WebSockets na porcie 81

// Wi-Fi, MQTT i Home Assistant
WiFiClient client;             // Klient połączenia WiFi
HADevice device("AquaDoser");  // Definicja urządzenia dla Home Assistant
HAMqtt mqtt(client, device);   // Klient MQTT dla Home Assistant

// Czujniki i przełączniki dla Home Assistant

HABinarySensor* pumpStates[NUMBER_OF_PUMPS];  // Sensory do pokazywania aktualnego stanu pomp (włączona/wyłączona)
HASensor* calibrationSensors[NUMBER_OF_PUMPS];

HASwitch* pumpSchedules[NUMBER_OF_PUMPS];  // Przełączniki do aktywacji/deaktywacji harmonogramu dla każdej pompy
HASwitch switchService("service_mode");    // Tryb serwisowy
HASwitch switchSound("sound_switch");      // Dźwięki systemu

// ** FILTROWANIE I POMIARY **

void handleTimeAPI() {
    time_t utc = now();
    TimeChangeRule *tcr;
    time_t local = CE.toLocal(utc, &tcr);
    
    String json = "{";
    json += "\"hour\":" + String(hour(local)) + ",";
    json += "\"minute\":" + String(minute(local)) + ",";
    json += "\"second\":" + String(second(local)) + ",";
    json += "\"day\":" + String(day(local)) + ",";
    json += "\"month\":" + String(month(local)) + ",";
    json += "\"year\":" + String(year(local)) + ",";
    json += "\"isDST\":" + String(tcr->offset == 120 ? "true" : "false") + ",";
    json += "\"tzAbbrev\":\"" + String(tcr->abbrev) + "\"";
    json += "}";
    
    server.send(200, "application/json", json);
}

void setupRTC() {
    if (!rtc.begin()) {
        AQUA_DEBUG_PRINTF("Nie można znaleźć RTC");
        return;
    }

    // Sprawdź czy RTC działa
    if (rtc.lostPower()) {
        AQUA_DEBUG_PRINTF("RTC stracił zasilanie, ustawiam czas kompilacji!");
        // Ustaw czas kompilacji jako fallback
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    // Synchronizuj czas systemowy z RTC
    DateTime now = rtc.now();
    setTime(now.unixtime());
    
    AQUA_DEBUG_PRINTF("RTC zainicjalizowany, czas: %04d-%02d-%02d %02d:%02d:%02d", 
        now.year(), now.month(), now.day(),
        now.hour(), now.minute(), now.second());
}

// Zegar
bool initRTC() {
  if (!rtc.begin()) {
    AQUA_DEBUG_PRINT(F("Nie znaleziono RTC DS3231!"));
    return false;
  }

  // Jeśli RTC stracił zasilanie, ustaw czas z NTP
  if (rtc.lostPower()) {
    AQUA_DEBUG_PRINT(F("RTC stracił zasilanie, synchronizuję z NTP..."));
    syncTimeFromNTP();
  }

  return true;
}

void syncTimeFromNTP() {
  time_t ntpTime = time(nullptr);
  if (ntpTime > 0) {
    // Konwersja czasu UTC na lokalny
    time_t localTime = CE.toLocal(ntpTime);

    rtc.adjust(DateTime(localTime));
    lastNTPSync = millis();
    AQUA_DEBUG_PRINT(F("Zsynchronizowano czas z NTP"));
  }
}

String getFormattedDateTime() {
    time_t utc = now(); // Aktualny czas UTC
    time_t local = CE.toLocal(utc); // Konwersja na czas lokalny
    
    TimeChangeRule *tcr;
    time_t t = CE.toLocal(utc, &tcr); // Pobierz też regułę czasu
    
    char buf[32];
    snprintf(buf, sizeof(buf), 
        "%02d/%02d/%04d %02d:%02d:%02d %s",
        day(t), month(t), year(t),
        hour(t), minute(t), second(t),
        tcr->abbrev);
    return String(buf);
}

// Reset do ustawień fabrycznych
void factoryReset() {
  WiFi.disconnect(true);  // true = kasuj zapisane ustawienia
  WiFi.mode(WIFI_OFF);
  delay(100);

  WiFiManager wm;
  wm.resetSettings();
  ESP.eraseConfig();

  setDefaultConfig();
  saveConfig();

  delay(100);
  ESP.reset();
}

// Reset urządzenia
void rebootDevice() {
  ESP.restart();
}

// Przepełnienie licznika millis()
void handleMillisOverflow() {
  unsigned long currentMillis = millis();

  // Sprawdź przepełnienie dla wszystkich timerów
  if (currentMillis < status.pumpStartTime) status.pumpStartTime = 0;
  if (currentMillis < status.pumpDelayStartTime) status.pumpDelayStartTime = 0;
  if (currentMillis < status.lastSoundAlert) status.lastSoundAlert = 0;
  if (currentMillis < status.lastSuccessfulMeasurement) status.lastSuccessfulMeasurement = 0;

  // Jeśli zbliża się przepełnienie, zresetuj wszystkie timery
  if (currentMillis > MILLIS_OVERFLOW_THRESHOLD) {
    status.pumpStartTime = 0;
    status.pumpDelayStartTime = 0;
    status.lastSoundAlert = 0;
    status.lastSuccessfulMeasurement = 0;

    AQUA_DEBUG_PRINT(F("Reset timerów - zbliża się przepełnienie millis()"));
  }
}

// Ładowanie konfiguracji z pamięci EEPROM
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

// Zapis aktualnej konfiguracji do pamięci EEPROM
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

// ** FUNKCJE DŹWIĘKOWE **

// Odtwórz krótki dźwięk ostrzegawczy
void playShortWarningSound() {
  if (config.soundEnabled) {
    tone(BUZZER_PIN, 2000, 100);  // Krótkie piknięcie (2000Hz, 100ms)
  }
}

// Odtwórz dźwięk potwierdzenia
void playConfirmationSound() {
  if (config.soundEnabled) {
    tone(BUZZER_PIN, 2000, 200);  // Dłuższe piknięcie (2000Hz, 200ms)
  }
}

// ** FUNKCJE ALARMÓW I STEROWANIA POMPĄ **

// Funkcja do sterowania pompą
void setPump(byte pumpIndex, bool state) {
    if (pumpIndex >= NUMBER_OF_PUMPS) return;
    
    if (state) {
        pumpStateByte |= (1 << pumpIndex);  // Ustaw bit
    } else {
        pumpStateByte &= ~(1 << pumpIndex); // Wyczyść bit
    }
    
    // PCF8574 ma odwróconą logikę (LOW = włączone)
    for (uint8_t i = 0; i < 8; i++) {
        bool pinState = !(pumpStateByte & (1 << i));
        pcf8574.digitalWrite(i, pinState);
    }
}

// Funkcja do rozpoczęcia dozowania
void startDosing(byte pumpIndex) {
    if (pumpIndex >= NUMBER_OF_PUMPS) return;
    if (pumpStates[pumpIndex].isRunning) return;
    
    float volume = config.pumps[pumpIndex].volume;
    float flow = config.pumps[pumpIndex].flow;
    float dosingTime = (volume / flow) * 60 * 1000; // czas w ms
    
    pumpStates[pumpIndex].isRunning = true;
    pumpStates[pumpIndex].startTime = millis();
    pumpStates[pumpIndex].duration = (unsigned long)dosingTime;
    
    setPump(pumpIndex, true);
    setLEDDosing(pumpIndex);
}

// Funkcja do zatrzymania dozowania
void stopDosing(byte pumpIndex) {
    if (pumpIndex >= NUMBER_OF_PUMPS) return;
    
    setPump(pumpIndex, false);
    pumpStates[pumpIndex].isRunning = false;
    
    // Przywróć odpowiedni kolor LED
    if (serviceMode) {
        setLEDService(pumpIndex);
    } else {
        config.pumps[pumpIndex].status ? setLEDActive(pumpIndex) : setLEDInactive(pumpIndex);
    }
}

// Aktualizacja stanu pomp
void updatePumps() {
    unsigned long currentTime = millis();
    
    for (byte i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumpStates[i].isRunning) {
            if (currentTime - pumpStates[i].startTime >= pumpStates[i].duration) {
                stopDosing(i);
            }
        }
    }
}

// Przy zmianie stanu pompy
void updatePumpStatus(byte pumpIndex, bool enabled) {
    config.pumps[pumpIndex].status = enabled;
    if (enabled) {
        setLEDActive(pumpIndex);
    } else {
        setLEDInactive(pumpIndex);
    }
}

// Przy rozpoczęciu dozowania
void startDosing(byte pumpIndex) {
    if (!pumpStates[pumpIndex].isRunning) {
        // ... kod dozowania ...
        setLEDDosing(pumpIndex);
    }
}

// Po zakończeniu dozowania
void stopDosing(byte pumpIndex) {
    setPump(pumpIndex, false);
    pumpStates[pumpIndex].isRunning = false;
    config.pumps[pumpIndex].status ? setLEDActive(pumpIndex) : setLEDInactive(pumpIndex);
}

void initializeLEDs() {
    strip.begin();
    strip.setBrightness(50);
    
    // Ustaw początkowe kolory bazując na stanie pomp
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        pumpColors[i] = config.pumps[i].status ? COLOR_ACTIVE : COLOR_INACTIVE;
        strip.setPixelColor(i, pumpColors[i]);
    }
    
    strip.show();
}

void updateLEDs() {
    bool changed = false;
    
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        uint32_t currentColor = strip.getPixelColor(i);
        if (currentColor != pumpColors[i]) {
            strip.setPixelColor(i, pumpColors[i]);
            changed = true;
        }
    }
    
    if (changed) {
        strip.show();
    }
}

// Funkcja do ustawiania koloru
void setPumpLED(uint8_t pumpIndex, uint32_t color) {
    if (pumpIndex < NUMBER_OF_PUMPS) {
        pumpColors[pumpIndex] = color;
    }
}

// Funkcje pomocnicze dla każdego stanu
void setLEDInactive(uint8_t pumpIndex) {
    setPumpLED(pumpIndex, COLOR_INACTIVE);
}

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

// Funkcja do ustawiania wszystkich LED-ów w tryb serwisowy
void setAllLEDsService() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        setLEDService(i);
    }
}

// Funkcja do przywracania normalnego stanu LED-ów
void restoreNormalLEDs() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumpStates[i].isRunning) {
            setLEDDosing(i);
        } else {
            config.pumps[i].status ? setLEDActive(i) : setLEDInactive(i);
        }
    }
}

// Przy wejściu w tryb serwisowy
void enterServiceMode() {
    setAllLEDsService();
    // ... pozostały kod trybu serwisowego ...
}

// Przy wyjściu z trybu serwisowego
void exitServiceMode() {
    restoreNormalLEDs();
    // ... pozostały kod wyjścia z trybu serwisowego ...
}

// Przy rozpoczęciu kalibracji
void startCalibration(byte pumpIndex) {
    setLEDCalibration(pumpIndex);
    // ... kod kalibracji ...
}

// Po zakończeniu kalibracji
void stopCalibration(byte pumpIndex) {
    config.pumps[pumpIndex].status ? setLEDActive(pumpIndex) : setLEDInactive(pumpIndex);
    // ... pozostały kod zakończenia kalibracji ...
}

// ** FUNKCJE WI-FI I MQTT **

// Reset ustawień Wi-Fi
void resetWiFiSettings() {
  AQUA_DEBUG_PRINT(F("Rozpoczynam kasowanie ustawień WiFi..."));

  // Najpierw rozłącz WiFi i wyczyść wszystkie zapisane ustawienia
  WiFi.disconnect(false, true);  // false = nie wyłączaj WiFi, true = kasuj zapisane ustawienia

  // Upewnij się, że WiFi jest w trybie stacji
  WiFi.mode(WIFI_STA);

  // Reset przez WiFiManager
  WiFiManager wm;
  wm.resetSettings();

  AQUA_DEBUG_PRINT(F("Ustawienia WiFi zostały skasowane"));
  delay(100);
}

// Konfiguracja MQTT z Home Assistant
void setupHA() {
  // Konfiguracja urządzenia dla Home Assistant
  device.setName("AquaDoser");                  // Nazwa urządzenia
  device.setModel("AD ESP8266");                // Model urządzenia
  device.setManufacturer("PMW");                // Producent
  device.setSoftwareVersion(SOFTWARE_VERSION);  // Wersja oprogramowania

  // Tworzenie sensorów dla aktualnego stanu pomp
  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
    char uniqueId[32];
    sprintf(uniqueId, "stan_pumpy_%d", i + 1);

    pumpStates[i] = new HABinarySensor(uniqueId);
    //pumpStates[i]->setName(String("Stan Pompy ") + String(i + 1));
    String nazwaPompy = String("Stan Pompy ") + String(i + 1);
    pumpStates[i]->setName(nazwaPompy.c_str());
    pumpStates[i]->setDeviceClass("running");
  }

  // Tworzenie przełączników do aktywacji harmonogramu
  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
    char uniqueId[32];
    sprintf(uniqueId, "pompa_%d", i + 1);

    pumpSchedules[i] = new HASwitch(uniqueId);
    //pumpSchedules[i]->setName(String("Pompa ") + String(i + 1));
    String nazwaPompy = String("Pompa ") + String(i + 1);
    pumpSchedules[i]->setName(nazwaPompy.c_str());
    pumpSchedules[i]->onCommand(onPumpCommand);
  }

  // Inicjalizacja sensorów kalibracji
  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
    String sensorId = String(F("pump_")) + String(i + 1) + String(F("_calibration"));
    calibrationSensors[i] = new HASensor(sensorId.c_str());
    calibrationSensors[i]->setName((String(config.pumps[i].name) + " Last Calibration").c_str());
    calibrationSensors[i]->setIcon("mdi:calendar");
  }

  switchSound.setName("Dźwięk");
  switchSound.setIcon("mdi:volume-high");       // Ikona głośnika
  switchSound.onCommand(onSoundSwitchCommand);  // Funkcja obsługi zmiany stanu

  // Konfiguracja przełączników w HA
  switchService.setName("Serwis");
  switchService.setIcon("mdi:account-wrench-outline");
  switchService.onCommand(onServiceSwitchCommand);  // Funkcja obsługi zmiany stanu

  mqtt.begin(config.mqtt_server, config.mqtt_port, config.mqtt_user, config.mqtt_password);  // Połącz z MQTT
}

// ** FUNKCJE ZWIĄZANE Z PINAMI **

// Konfiguracja pinów wejścia/wyjścia
void setupPin() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);  // Wejście z podciąganiem - przycisk

    pinMode(BUZZER_PIN, OUTPUT);    // Wyjście - buzzer
    digitalWrite(BUZZER_PIN, LOW);  // Wyłączenie buzzera

    pcf8574.begin();

    initializePCF();
    initializeLEDs();
    
    // Przywróć stany pomp z konfiguracji
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        setPump(i, false); // Na początku wszystkie pompy wyłączone
        config.pumps[i].status ? setLEDActive(i) : setLEDInactive(i);
    }
}

    // Inicjalizacja PCF8574
    void initializePCF() {
        if (!pcf8574.begin()) {
            Serial.println("Could not initialize PCF8574");
            return;
        }
        
        // Ustaw wszystkie piny jako wyjścia w stanie wysokim (pompy wyłączone)
        for (uint8_t i = 0; i < 8; i++) {
            pcf8574.digitalWrite(i, HIGH);
        }
    }

// Efekt powitalny
void playWelcomeEffect() {
  // Efekt 1: Przebiegające światło (1s)
  for (int j = 0; j < 2; j++) {  // Dwa przebiegi
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
      strip.setPixelColor(i, COLOR_RAINBOW_1);
      if (i > 0) strip.setPixelColor(i - 1, COLOR_OFF);
      strip.show();
      delay(100);
    }
    strip.setPixelColor(NUMBER_OF_PUMPS - 1, COLOR_OFF);
    strip.show();
  }

  // Efekt 2: Tęczowa fala (1s)
  for (int j = 0; j < 2; j++) {  // Dwa przebiegi
    uint32_t colors[] = { COLOR_RAINBOW_1, COLOR_RAINBOW_2, COLOR_RAINBOW_3,
                          COLOR_RAINBOW_4, COLOR_RAINBOW_5, COLOR_RAINBOW_6 };
    for (int c = 0; c < 6; c++) {
      for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        strip.setPixelColor(i, colors[(i + c) % 6]);
      }
      strip.show();
      delay(160);
    }
  }
}

// Odtwarzaj melodię powitalną
void welcomeMelody() {
  tone(BUZZER_PIN, 1397, 100);  // F6
  delay(150);
  tone(BUZZER_PIN, 1568, 100);  // G6
  delay(150);
  tone(BUZZER_PIN, 1760, 150);  // A6
  delay(200);
}

// Wyślij pierwszą aktualizację stanu do Home Assistant
void firstUpdateHA() {
  for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
    updatePumpState(i, false);
  }

  // Wymuś stan OFF na początku
  //sensorAlarm.setValue("OFF");
  switchSound.setState(false);  // Dodane - wymuś stan początkowy

  // Ustawienie końcowych stanów i wysyłka do HA
  switchSound.setState(status.soundEnabled);  // Dodane - ustaw aktualny stan dźwięku
}

// ** FUNKCJE ZWIĄZANE Z PRZYCISKIEM **

// Obsługa przycisku
void handleButton() {
  static unsigned long lastDebounceTime = 0;
  static bool lastReading = HIGH;
  const unsigned long DEBOUNCE_DELAY = 50;  // 50ms debounce

  bool reading = digitalRead(BUTTON_PIN);

  // Jeśli odczyt się zmienił, zresetuj timer debounce
  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  // Kontynuuj tylko jeśli minął czas debounce
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    // Jeśli stan się faktycznie zmienił po debounce
    if (reading != buttonState.lastState) {
      buttonState.lastState = reading;

      if (reading == LOW) {  // Przycisk naciśnięty
        buttonState.pressedTime = millis();
        buttonState.isLongPressHandled = false;  // Reset flagi długiego naciśnięcia
      } else {                                   // Przycisk zwolniony
        buttonState.releasedTime = millis();

        // Sprawdzenie czy to było krótkie naciśnięcie
        if (buttonState.releasedTime - buttonState.pressedTime < LONG_PRESS_TIME) {
          // Przełącz tryb serwisowy
          status.isServiceMode = !status.isServiceMode;
          playConfirmationSound();                             // Sygnał potwierdzenia zmiany trybu
          switchService.setState(status.isServiceMode, true);  // force update w HA

          // Log zmiany stanu
          AQUA_DEBUG_PRINTF("Tryb serwisowy: %s (przez przycisk)\n", status.isServiceMode ? "WŁĄCZONY" : "WYŁĄCZONY");

          // Jeśli włączono tryb serwisowy podczas pracy pompy
          // if (status.isServiceMode && status.isPumpActive) {
          //     pcf8574.digitalWrite(pumpPin, LOW);  // Wyłącz pompę
          //     status.isPumpActive = false;  // Reset flagi aktywności
          //     status.pumpStartTime = 0;  // Reset czasu startu
          //     sensorPump.setValue("OFF");  // Aktualizacja w HA
          // }
        }
      }
    }

    // Obsługa długiego naciśnięcia (reset blokady pompy)
    if (reading == LOW && !buttonState.isLongPressHandled) {
      if (millis() - buttonState.pressedTime >= LONG_PRESS_TIME) {
        ESP.wdtFeed();                  // Reset przy długim naciśnięciu
        status.pumpSafetyLock = false;  // Zdjęcie blokady pompy
        playConfirmationSound();        // Sygnał potwierdzenia zmiany trybu
        //switchPumpAlarm.setState(false, true);  // force update w HA
        buttonState.isLongPressHandled = true;  // Oznacz jako obsłużone
        AQUA_DEBUG_PRINT("Alarm pompy skasowany");
      }
    }
  }

  lastReading = reading;  // Zapisz ostatni odczyt dla następnego porównania
  yield();                // Oddaj sterowanie systemowi
}

// Obsługa przełącznika dźwięku (HA)
void onSoundSwitchCommand(bool state, HASwitch* sender) {
  status.soundEnabled = state;  // Aktualizuj status lokalny
  config.soundEnabled = state;  // Aktualizuj konfigurację
  saveConfig();                 // Zapisz do EEPROM

  // Aktualizuj stan w Home Assistant
  switchSound.setState(state, true);  // force update

  // Zagraj dźwięk potwierdzenia tylko gdy włączamy dźwięk
  if (state) {
    playConfirmationSound();
  }

  AQUA_DEBUG_PRINTF("Zmieniono stan dźwięku na: ", state ? "WŁĄCZONY" : "WYŁĄCZONY");
}

// Callback dla przełączników aktywacji pomp
void onPumpCommand(bool state, HASwitch* sender) {
  // Znajdź indeks pompy
  int pumpIndex = -1;
  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
    if (sender == pumpSchedules[i]) {
      pumpIndex = i;
      break;
    }
  }

  if (pumpIndex >= 0) {
    // Tu później dodamy kod obsługi włączania/wyłączania harmonogramu
    // Na razie tylko aktualizujemy stan w HA
    sender->setState(state);
  }
}

// Obsługuje komendę przełącznika trybu serwisowego
void onServiceSwitchCommand(bool state, HASwitch* sender) {
  playConfirmationSound();       // Sygnał potwierdzenia zmiany trybu
  status.isServiceMode = state;  // Ustawienie flagi trybu serwisowego
  buttonState.lastState = HIGH;  // Reset stanu przycisku

  // Aktualizacja stanu w Home Assistant
  switchService.setState(state);  // Synchronizacja stanu przełącznika

  if (state) {  // Włączanie trybu serwisowego
                // if (status.isPumpActive) {
                //     pcf8574.digitalWrite(pumpPin, LOW);  // Wyłączenie pompy
                //     status.isPumpActive = false;  // Reset flagi aktywności
                //     status.pumpStartTime = 0;  // Reset czasu startu
                //     sensorPump.setValue("OFF");  // Aktualizacja stanu w HA
                // }
  } else {      // Wyłączanie trybu serwisowego
                // Reset stanu opóźnienia pompy aby umożliwić normalne uruchomienie
                //status.isPumpDelayActive = false;
                //status.pumpDelayStartTime = 0;
                // Normalny tryb pracy - pompa uruchomi się automatycznie
                // jeśli czujnik poziomu wykryje wodę
  }

  AQUA_DEBUG_PRINTF("Tryb serwisowy: %s (przez HA)\n", state ? "WŁĄCZONY" : "WYŁĄCZONY");
}

String getConfigPage() {
    String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                   "<title>Test</title></head><body>");
    
    // Sprawdź index.html
    html += F("<h2>Checking index.html:</h2>");
    if (!LittleFS.exists("/index.html")) {
        html += F("<p style='color: red'>Error: index.html not found!</p>");
    } else {
        File file = LittleFS.open("/index.html", "r");
        html += F("<p style='color: green'>index.html exists - size: ");
        html += String(file.size());
        html += F(" bytes</p>");
        file.close();
    }
    
    // Sprawdź css/style.css
    html += F("<h2>Checking css/style.css:</h2>");
    if (!LittleFS.exists("/css/style.css")) {
        html += F("<p style='color: red'>Error: css/style.css not found!</p>");
    } else {
        File cssFile = LittleFS.open("/css/style.css", "r");
        html += F("<p style='color: green'>style.css exists - size: ");
        html += String(cssFile.size());
        html += F(" bytes</p>");
        cssFile.close();
    }
    
    // Sprawdź js/main.js
    html += F("<h2>Checking js/main.js:</h2>");
    if (!LittleFS.exists("/js/main.js")) {
        html += F("<p style='color: red'>Error: js/main.js not found!</p>");
    } else {
        File jsFile = LittleFS.open("/js/main.js", "r");
        html += F("<p style='color: green'>main.js exists - size: ");
        html += String(jsFile.size());
        html += F(" bytes</p>");
        jsFile.close();
    }
    
    // Pokaż listę wszystkich plików
    html += F("<h2>All files in LittleFS:</h2><ul>");
    Dir dir = LittleFS.openDir("/");
    while (dir.next()) {
        html += F("<li>");
        html += dir.fileName();
        html += F(" (");
        html += String(dir.fileSize());
        html += F(" bytes)</li>");
    }
    html += F("</ul>");
    
    html += F("</body></html>");
    return html;
}

// Obsługa żądania HTTP do głównej strony konfiguracji
void handleRoot() {
  String content = getConfigPage();

  server.send(200, "text/html", content);
}

// Waliduje wartości konfiguracji wprowadzone przez użytkownika
// W validateConfigValues():
bool validateConfigValues() {
  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
    // Sprawdź kalibrację (nie może być 0 lub ujemna)
    float calibration = server.arg("p" + String(i) + "_calibration").toFloat();
    if (calibration <= 0) {
      //webSocket.broadcastTXT("save:error:Kalibracja pompy " + //String(i+1) + " musi być większa od 0");
      String wiadomosc = "save:error:Kalibracja pompy " + String(i + 1) + " musi być większa od 0";
      webSocket.broadcastTXT(wiadomosc);
      return false;
    }

    // Sprawdź dozowanie (nie może być ujemne)
    int dosage = server.arg("p" + String(i) + "_dosage").toInt();
    if (dosage < 0) {
      String message = "save:error:Dozowanie pompy " + String(i + 1) + " nie może być ujemne";
      webSocket.broadcastTXT(message);
      return false;
    }

    // Sprawdź godzinę (0-23)
    int hour = server.arg("p" + String(i) + "_hour").toInt();
    if (hour < 0 || hour > 23) {
      String message2 = "save:error:Nieprawidłowa godzina dla pompy " + String(i + 1);
      webSocket.broadcastTXT(message2);
      return false;
    }

    // Sprawdź minutę (0-59)
    int minute = server.arg("p" + String(i) + "_minute").toInt();
    if (minute < 0 || minute > 59) {
      String message3 = "save:error:Nieprawidłowa minuta dla pompy " + String(i + 1);
      webSocket.broadcastTXT(message3);
      return false;
    }
  }
  return true;
}

// Funkcja zapisująca tylko konfigurację MQTT
void handleSaveMQTT() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    bool changed = false;
    
    // Tymczasowa kopia konfiguracji MQTT
    char temp_server[40] = {0};
    uint16_t temp_port = config.mqtt_port;
    char temp_user[32] = {0};
    char temp_password[32] = {0};
    
    // Zapisz nowe ustawienia do zmiennych tymczasowych
    if (server.hasArg("mqtt_server")) {
        strlcpy(temp_server, server.arg("mqtt_server").c_str(), sizeof(temp_server));
        changed = true;
    }
    if (server.hasArg("mqtt_port")) {
        temp_port = server.arg("mqtt_port").toInt();
        changed = true;
    }
    if (server.hasArg("mqtt_user")) {
        strlcpy(temp_user, server.arg("mqtt_user").c_str(), sizeof(temp_user));
        changed = true;
    }
    if (server.hasArg("mqtt_password")) {
        strlcpy(temp_password, server.arg("mqtt_password").c_str(), sizeof(temp_password));
        changed = true;
    }

    if (!changed) {
        server.send(200, "text/plain", "OK");
        webSocket.broadcastTXT("save:info:Brak zmian w konfiguracji MQTT");
        return;
    }

    bool success = false;
    
    // Zapisz do EEPROM
    EEPROM.begin(sizeof(Config));
    yield();

    if (changed) {
        // Aktualizuj główną konfigurację
        if (server.hasArg("mqtt_server")) strlcpy(config.mqtt_server, temp_server, sizeof(config.mqtt_server));
        if (server.hasArg("mqtt_port")) config.mqtt_port = temp_port;
        if (server.hasArg("mqtt_user")) strlcpy(config.mqtt_user, temp_user, sizeof(config.mqtt_user));
        if (server.hasArg("mqtt_password")) strlcpy(config.mqtt_password, temp_password, sizeof(config.mqtt_password));
        
        yield();
        
        // Zapisz do EEPROM
        uint8_t *p = (uint8_t*)&config;
        size_t mqttOffset = offsetof(Config, mqtt_server);
        size_t mqttSize = sizeof(config.mqtt_server) + sizeof(config.mqtt_port) + 
                         sizeof(config.mqtt_user) + sizeof(config.mqtt_password);
        
        for (size_t i = 0; i < mqttSize; i++) {
            EEPROM.write(mqttOffset + i, p[mqttOffset + i]);
            if (i % 16 == 0) yield();
        }

        // Oblicz i zapisz sumę kontrolną
        config.checksum = calculateChecksum(config);
        EEPROM.write(offsetof(Config, checksum), config.checksum);
        yield();

        success = EEPROM.commit();
        yield();
    }
    
    EEPROM.end();
    yield();

    // Wyślij odpowiedź
    server.send(success ? 200 : 500, "text/plain", success ? "OK" : "Error");
    yield();

    // Wyślij komunikat przez WebSocket
    if (success) {
        webSocket.broadcastTXT("save:success:Zapisano ustawienia MQTT");
    } else {
        webSocket.broadcastTXT("save:error:Błąd zapisu konfiguracji MQTT");
    }
}

// Funkcja zapisująca tylko konfigurację pomp
void handleSavePumps() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    // Struktura tymczasowa dla jednej pompy
    struct {
        char name[32];
        bool enabled;
        float calibration;
        float dosage;
        uint8_t hour;
        uint8_t minute;
        uint8_t weekDays;
    } tempPump;

    // Rozpocznij sesję EEPROM
    EEPROM.begin(sizeof(Config));
    yield();

    bool anyChanges = false;

    // Aktualizuj każdą pompę osobno
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        bool pumpChanged = false;
        
        // Kopiuj aktualne dane pompy do bufora tymczasowego
        memcpy(&tempPump, &config.pumps[i], sizeof(tempPump));
        yield();

        // Nazwa
        if (server.hasArg("p" + String(i) + "_name")) {
            strlcpy(tempPump.name, server.arg("p" + String(i) + "_name").c_str(), sizeof(tempPump.name));
            pumpChanged = true;
        }
        yield();

        // Enabled
        tempPump.enabled = server.hasArg("p" + String(i) + "_enabled");
        pumpChanged = true;
        yield();

        // Kalibracja
        if (server.hasArg("p" + String(i) + "_calibration")) {
            tempPump.calibration = server.arg("p" + String(i) + "_calibration").toFloat();
            pumpChanged = true;
        }
        yield();

        // Dawkowanie
        if (server.hasArg("p" + String(i) + "_dosage")) {
            tempPump.dosage = server.arg("p" + String(i) + "_dosage").toFloat();
            pumpChanged = true;
        }
        yield();

        // Godzina
        if (server.hasArg("p" + String(i) + "_hour")) {
            tempPump.hour = constrain(server.arg("p" + String(i) + "_hour").toInt(), 0, 23);
            pumpChanged = true;
        }
        yield();

        // Minuta
        if (server.hasArg("p" + String(i) + "_minute")) {
            tempPump.minute = constrain(server.arg("p" + String(i) + "_minute").toInt(), 0, 59);
            pumpChanged = true;
        }
        yield();

        // Dni tygodnia
        uint8_t weekDays = 0;
        for (int day = 0; day < 7; day++) {
            if (server.hasArg("p" + String(i) + "_day" + String(day))) {
                weekDays |= (1 << day);
            }
        }
        tempPump.weekDays = weekDays;
        pumpChanged = true;
        yield();

        // Jeśli były zmiany w pompie, zapisz ją
        if (pumpChanged) {
            anyChanges = true;
            
            // Zapisz do głównej konfiguracji
            memcpy(&config.pumps[i], &tempPump, sizeof(tempPump));
            yield();

            // Zapisz do EEPROM
            size_t pumpOffset = offsetof(Config, pumps) + (i * sizeof(tempPump));
            uint8_t *pumpData = (uint8_t*)&tempPump;
            
            for (size_t j = 0; j < sizeof(tempPump); j++) {
                EEPROM.write(pumpOffset + j, pumpData[j]);
                if (j % 4 == 0) yield(); // Częstsze yield
            }
            yield();
        }
    }

    // Przekieruj z powrotem na główną stronę
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
}

// Obsługa aktualizacji oprogramowania przez HTTP
void handleDoUpdate() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    if (upload.filename == "") {
      webSocket.broadcastTXT("update:error:No file selected");
      server.send(204);
      return;
    }

    if (!Update.begin(upload.contentLength)) {
      Update.printError(Serial);
      webSocket.broadcastTXT("update:error:Update initialization failed");
      server.send(204);
      return;
    }
    webSocket.broadcastTXT("update:0");  // Start progress
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      webSocket.broadcastTXT("update:error:Write failed");
      return;
    }
    // Aktualizacja paska postępu
    int progress = (upload.totalSize * 100) / upload.contentLength;
    String progressMsg = "update:" + String(progress);
    webSocket.broadcastTXT(progressMsg);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      webSocket.broadcastTXT("update:100");  // Completed
      server.send(204);
      delay(1000);
      ESP.restart();
    } else {
      Update.printError(Serial);
      webSocket.broadcastTXT("update:error:Update failed");
      server.send(204);
    }
  }
}

// Obsługa wyniku procesu aktualizacji oprogramowania
void handleUpdateResult() {
  if (Update.hasError()) {
    server.send(200, "text/html",
                "<h1>Aktualizacja nie powiodła się</h1>"
                "<a href='/'>Powrót</a>");
  } else {
    server.send(200, "text/html",
                "<h1>Aktualizacja zakończona powodzeniem</h1>"
                "Urządzenie zostanie zrestartowane...");
    delay(1000);
    ESP.restart();
  }
}

// Konfiguracja serwera weboweg do obsługi żądań HTTP
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/api/time", HTTP_GET, handleTimeAPI);
  server.on("/update", HTTP_POST, handleUpdateResult, handleDoUpdate);
  //server.on("/save", handleSave);
  server.on("/save-mqtt", HTTP_POST, handleSaveMQTT);
  server.on("/save-pumps", HTTP_POST, handleSavePumps);

  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "Restarting...");
    delay(1000);
    ESP.restart();
  });

  server.on("/factory-reset", HTTP_POST, []() {
    server.send(200, "text/plain", "Resetting to factory defaults...");
    delay(200);
    resetConfig();
  });

  server.on("/reset-wifi", HTTP_POST, []() {
    server.send(200, "text/plain", "Resetting WiFi settings...");
    delay(200);
    wifiManager.resetSettings();
    ESP.restart();
  });

  // Dodaj obsługę plików statycznych
  server.serveStatic("/css/style.css", LittleFS, "/css/style.css");
  server.serveStatic("/js/main.js", LittleFS, "/js/main.js");

  server.begin();
}

// ** WEBSOCKET I KOMUNIKACJA W SIECI **

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[%u] Connected\n", num);
      break;

    case WStype_TEXT:
      {
        String message = String((char*)payload);

        if (message.startsWith("test_pump:")) {
          int pumpId = message.substring(10).toInt();
          if (pumpId >= 0 && pumpId < NUMBER_OF_PUMPS) {
            // Włącz pompę
            pcf8574.digitalWrite(config.pumps[pumpId].pcf8574_pin, HIGH);
            Serial.printf("Rozpoczęto test pompy %d\n", pumpId + 1);

            // Zaplanuj wyłączenie pompy za 5 sekund
            pumpTestEndTime = millis() + 5000;
            testingPumpId = pumpId;

            // Wyślij potwierdzenie do przeglądarki
            //webSocket.broadcastTXT("pump_test:started:" + String(pumpId));
            String message4 = "pump_test:started:" + String(pumpId);
            webSocket.broadcastTXT(message4);
          }
        }
      }
      break;
  }
}

// ** Funkcja setup - inicjalizacja urządzeń i konfiguracja **

void setup() {
  //ESP.wdtEnable(WATCHDOG_TIMEOUT);  // Aktywacja watchdoga
  Serial.begin(115200);  // Inicjalizacja portu szeregowego
  Serial.println("\nStart AquaDoser...");

  currentStatus = getCustomTimeStatus();

  // Inicjalizacja LittleFS
  if (!LittleFS.begin()) {
    Serial.println(F("Błąd montowania LittleFS"));
    return;
  }
  Serial.println(F("LittleFS zamontowany pomyślnie"));

  // Wczytaj konfigurację lub zresetuj do wartości domyślnych
  if (!loadConfig()) {
    Serial.println(F("Błąd wczytywania konfiguracji - resetowanie do wartości domyślnych"));
    resetConfig();
  }

  // Konfiguracja WiFiManager
  wifiManager.setDebugOutput(true);
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setAPCallback([](WiFiManager* myWiFiManager) {
    Serial.println(F("Uruchomiono tryb konfiguracji AP"));
    Serial.println(WiFi.softAPIP());
  });

  wifiManager.setSaveConfigCallback([]() {
    Serial.println(F("Zapisano nową konfigurację WiFi"));
  });

  if (!wifiManager.autoConnect("AquaDoser")) {
    Serial.println(F("Nie udało się połączyć i timeout został osiągnięty"));
    delay(3000);
    ESP.restart();
    delay(5000);
  }

  setupPin();
  setupPump();

  WiFiManager wifiManager;
  wifiManager.autoConnect("AquaDoser");  // Samo zadba o połączenie
  setupWebServer();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  setupHA();
  setupRTC();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Poczekaj na synchronizację czasu
  time_t now = time(nullptr);
  while (now < 24 * 3600) {
    delay(500);
    now = time(nullptr);
  }

  // Wysyłamy zapisane daty kalibracji do HA
  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
    publishCalibrationDate(i);
  }

  // Konfiguracja OTA
  ArduinoOTA.setHostname("AquaDoser");  // Ustaw nazwę urządzenia
  ArduinoOTA.setPassword("aquadoser");  // Ustaw hasło dla OTA
  ArduinoOTA.begin();                   // Uruchom OTA

  // LED
  strip.begin();
  strip.show();
  initializeLEDs();

  // Efekty startowe
  playWelcomeEffect();
  //welcomeMelody();
}

// ** Funkcja loop - główny cykl pracy urządzenia **

void loop() {
  unsigned long currentMillis = millis();

  handleMillisOverflow();  // Obsługa przepełnienia licznika millis()
  mqtt.loop();             // Obsługa MQTT
  server.handleClient();   // Obsługa serwera WWW
  updateLEDs();            // Aktualizacja diod LED

  // Aktualizacja stanu sensorów na podstawie PCF8574
  for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
    //bool pumpState = !pcf8574.read(i);  // Negacja bo logika ujemna
    bool pumpState = !pcf8574.digitalRead(i);
    pumpStates[i]->setState(pumpState);
  }

    if (!serviceMode) {
        if (currentMillis - lastCheckTime >= 1000) {
            DateTime now = rtc.now();
            
            for (byte i = 0; i < NUMBER_OF_PUMPS; i++) {
                if (!pumpStates[i].isRunning && 
                    config.pumps[i].status && 
                    isDayEnabled(config.pumps[i].days, now.dayOfTheWeek()) &&
                    now.hour() == config.pumps[i].hour && 
                    now.minute() == config.pumps[i].minute && 
                    now.second() == 0) {
                    
                    startDosing(i);
                }
            }
            lastCheckTime = currentMillis;
        }
    }
    
    updatePumps();

  if (currentMillis - timers.lastOTACheck >= OTA_CHECK_INTERVAL) {
    ArduinoOTA.handle();                  // Obsługa aktualizacji OTA
    timers.lastOTACheck = currentMillis;  // Aktualizacja znacznika czasu ostatniego sprawdzenia OTA
  }

  // Synchronizacja RTC z NTP raz na dobę
  if (millis() - lastNTPSync >= NTP_SYNC_INTERVAL) {
        DateTime now = rtc.now();
        setTime(now.unixtime());
        lastNTPSync = millis();
    }
 
  yield();  // Obsługa zadań systemowych ESP8266
}
