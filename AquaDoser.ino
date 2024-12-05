// ** BIBLIOTEKI **

// Biblioteki podstawowe
#include <Arduino.h>                  // Podstawowa biblioteka Arduino - zawiera funkcje rdzenia (pinMoc:\Users\piotrek\Documents\c:\Users\piotrek\Documents\Arduino\AquaDoser\config.inoArduino\AquaDoser_5_12_24\data\index.htmlde, digitalRead itp.)
#include <Wire.h>                     // Biblioteka do komunikacji I2C (TWI) - wymagana do obsługi PCF8574

// Biblioteki do komunikacji z Home Assistant
#include <ArduinoHA.h>                // Integracja z Home Assistant przez MQTT - automatyzacja, sensory, przełączniki
#include <PCF8574.h>                  // Obsługa ekspandera I/O PCF8574 - dodaje 8 wyjść cyfrowych przez I2C

// Biblioteki do obsługi WiFi i aktualizacji OTA
#include <ESP8266WiFi.h>              // Podstawowa obsługa WiFi dla ESP8266
#include <WiFiManager.h>              // Zarządzanie WiFi - portal konfiguracyjny, automatyczne połączenie
#include <ArduinoOTA.h>               // Aktualizacja firmware przez sieć (Over The Air)
#include <ESP8266HTTPUpdateServer.h>  // Aktualizacja firmware przez przeglądarkę (poprzez stronę WWW)

// Biblioteki do interfejsu webowego
#include <ESP8266WebServer.h>         // Serwer HTTP - obsługa strony konfiguracyjnej
#include <WebSocketsServer.h>         // WebSocket - komunikacja w czasie rzeczywistym ze stroną WWW
#include <EEPROM.h>                   // Dostęp do pamięci nieulotnej - zapisywanie konfiguracji
#include <LittleFS.h>

// Zegar
#include <RTClib.h>
#include <TimeLib.h>
#include <Timezone.h>    // dla obsługi czasu letniego/zimowego

// Pozostałe
#include <Adafruit_NeoPixel.h>  // Sterowanie LED

// Struktury konfiguracyjne i statusowe

const uint8_t NUMBER_OF_PUMPS = 8;  // Ilość pomp

struct CalibrationHistory {
    uint32_t timestamp;    // unix timestamp z RTC
    float volume;         // ilość w ml
    uint16_t time;        // czas w sekundach
    float flowRate;       // przeliczona wydajność ml/min
};

// Struktura dla pojedynczej pompy
struct PumpConfig {
    bool enabled;           // czy pompa jest włączona w harmonogramie
    char name[32];         // nazwa pompy (max 31 znaków + null terminator)
    uint8_t dosage;        // dawka w ml
    float calibration;     // kalibracja (ml/min)
    uint8_t pcf8574_pin;   // numer pinu na PCF8574
    uint8_t hour;          // godzina dozowania
    uint8_t minute;        // minuta dozowania
    uint8_t weekDays;      // dni tygodnia (bitmapa: 0b0PWTŚCPSN)
    CalibrationHistory lastCalibration;  // nowe pole
};

// Konfiguracja

// Główna struktura konfiguracji
struct Config {
    char hostname[32];                  // nazwa urządzenia w sieci
    char mqtt_server[64];               // adres serwera MQTT
    int mqtt_port;                      // port MQTT
    char mqtt_user[32];                 // nazwa użytkownika MQTT
    char mqtt_password[32];             // hasło MQTT
    bool soundEnabled;                  // czy dźwięki są włączone
    PumpConfig pumps[NUMBER_OF_PUMPS];  // konfiguracja dla każdej pompy
    unsigned long lastNTPSync;  // timestamp ostatniej synchronizacji
    uint8_t configVersion;              // wersja konfiguracji (dla EEPROM)
    char checksum;                      // suma kontrolna konfiguracji
};

// Struktura dla pojedynczej pompy - stan bieżący
struct Pump {
    bool isRunning;          // czy pompa aktualnie pracuje
    unsigned long lastDose;  // czas ostatniego dozowania
    float totalDosed;        // całkowita ilość dozowanego płynu
};

// Struktura do przechowywania różnych stanów i parametrów systemu
struct Status {
  unsigned long pumpStartTime;
  unsigned long pumpDelayStartTime;
  unsigned long lastSoundAlert;
  unsigned long lastSuccessfulMeasurement;
  bool isServiceMode;
  bool isPumpActive;
  bool isPumpDelayActive;
  bool soundEnabled;
  bool pumpSafetyLock;
  Pump pumps[NUMBER_OF_PUMPS];
};

// Stan przycisku
struct ButtonState {
    bool lastState;                   // Poprzedni stan przycisku
    bool isInitialized = false; 
    bool isLongPressHandled = false;  // Flaga obsłużonego długiego naciśnięcia
    unsigned long pressedTime = 0;    // Czas wciśnięcia przycisku
    unsigned long releasedTime = 0;   // Czas puszczenia przycisku
};

// Timery dla różnych operacji
struct Timers {
    unsigned long lastOTACheck;      // ostatnie sprawdzenie aktualizacji
    unsigned long lastPumpCheck;     // ostatnie sprawdzenie harmonogramu
    unsigned long lastStateUpdate;   // ostatnia aktualizacja stanu do HA
    unsigned long lastButtonCheck;   // ostatnie sprawdzenie przycisku
    
    // Konstruktor inicjalizujący wszystkie timery na 0
    Timers() : 
        lastOTACheck(0), 
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
    
    LEDState() : currentColor(0), targetColor(0), lastUpdateTime(0), 
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

// ** DEFINICJE PINÓW **

// Przypisanie pinów do urządzeń

const int BUZZER_PIN = 13;      // GPIO 13
const int LED_PIN = 12;         // GPIO 12
const int BUTTON_PIN = 14;    // GPIO 14
const int SDA_PIN = 4;          // GPIO 4
const int SCL_PIN = 5;          // GPIO 5

PCF8574 pcf8574(0x20);

// LED
// Definicja paska LED
Adafruit_NeoPixel strip(NUMBER_OF_PUMPS, LED_PIN, NEO_GRB + NEO_KHZ800);

#define LED_UPDATE_INTERVAL 50  // ms
#define PULSE_MAX_BRIGHTNESS 255
#define PULSE_MIN_BRIGHTNESS 50

#define COLOR_OFF 0xFF0000        // Czerwony - pompa wyłączona
#define COLOR_ON 0x00FF00         // Zielony - pompa włączona
#define COLOR_WORKING 0x0000FF    // Niebieski - pompa pracuje
#define COLOR_SERVICE 0xFFFF00    // Żółty - tryb serwisowy

#define COLOR_RAINBOW_1 strip.Color(255, 0, 0)      // Czerwony
#define COLOR_RAINBOW_2 strip.Color(255, 127, 0)    // Pomarańczowy
#define COLOR_RAINBOW_3 strip.Color(255, 255, 0)    // Żółty
#define COLOR_RAINBOW_4 strip.Color(0, 255, 0)      // Zielony
#define COLOR_RAINBOW_5 strip.Color(0, 0, 255)      // Niebieski
#define COLOR_RAINBOW_6 strip.Color(139, 0, 255)    // Fioletowy

// ** USTAWIENIA CZASOWE **

// Konfiguracja timeoutów i interwałów
const unsigned long WATCHDOG_TIMEOUT = 8000;       // Timeout dla watchdoga
const unsigned long LONG_PRESS_TIME = 1000;        // Czas długiego naciśnięcia przycisku
const unsigned long OTA_CHECK_INTERVAL = 1000;     // Sprawdzanie OTA co 1s
const unsigned long MILLIS_OVERFLOW_THRESHOLD = 4294967295U - 60000; // ~49.7 dni

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
const unsigned long NTP_SYNC_INTERVAL = 24UL * 60UL * 60UL * 1000UL; // 24h w milisekundach

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
const char* SOFTWARE_VERSION = "4.12.24";  // Definiowanie wersji oprogramowania

// Globalne instancje struktur
//CustomTimeStatus currentStatus = getCustomTimeStatus();
Config config;
Status status;
ButtonState buttonState;
Timers timers;
CustomTimeStatus currentStatus;
LEDState ledStates[NUMBER_OF_PUMPS];  // Stan diod LED
unsigned long lastLedUpdate = 0;

// ** INSTANCJE URZĄDZEŃ I USŁUG **

// Serwer HTTP i WebSockets
ESP8266WebServer server(80);     // Tworzenie instancji serwera HTTP na porcie 80
WebSocketsServer webSocket(81);  // Tworzenie instancji serwera WebSockets na porcie 81

// Wi-Fi, MQTT i Home Assistant
WiFiClient client;              // Klient połączenia WiFi
HADevice device("AquaDoser");  // Definicja urządzenia dla Home Assistant
HAMqtt mqtt(client, device);    // Klient MQTT dla Home Assistant

// Czujniki i przełączniki dla Home Assistant

HABinarySensor* pumpStates[NUMBER_OF_PUMPS];  // Sensory do pokazywania aktualnego stanu pomp (włączona/wyłączona)
HASensor* calibrationSensors[NUMBER_OF_PUMPS];

HASwitch* pumpSchedules[NUMBER_OF_PUMPS];  // Przełączniki do aktywacji/deaktywacji harmonogramu dla każdej pompy
HASwitch switchService("service_mode");  // Tryb serwisowy
HASwitch switchSound("sound_switch");    // Dźwięki systemu

// ** FILTROWANIE I POMIARY **

void saveCalibration(uint8_t pumpId, float volume, uint16_t calibrationTime) {
    // Oblicz wydajność
    float mlPerMinute = (volume * 60.0) / calibrationTime;
    
    // Zapisz dane kalibracji
    config.pumps[pumpId].lastCalibration.timestamp = now(); // current time from RTC
    config.pumps[pumpId].lastCalibration.volume = volume;
    config.pumps[pumpId].lastCalibration.time = calibrationTime;
    config.pumps[pumpId].lastCalibration.flowRate = mlPerMinute;
    
    // Zapisz do EEPROM
    saveConfig();
    
    // Wyślij potwierdzenie przez WebSocket
    //webSocket.broadcastTXT("calibration_saved:" + String(pumpId));
    String wiadomosc = "calibration_saved:" + String(pumpId);
    webSocket.broadcastTXT(wiadomosc);

    // Publikuj nową datę
    publishCalibrationDate(pumpId);
}

// Funkcja do aktualizacji daty kalibracji
void publishCalibrationDate(uint8_t pumpId) {
    if (config.pumps[pumpId].lastCalibration.timestamp > 0) {
        char dateStr[11];
        time_t ts = config.pumps[pumpId].lastCalibration.timestamp;
        strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", localtime(&ts));
        
        calibrationSensors[pumpId]->setValue(dateStr);
    }
}

// ** FUNKCJE I METODY SYSTEMOWE **

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
    time_t local = CE.toLocal(now());
    char buffer[25];
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
        year(local), month(local), day(local),
        hour(local), minute(local), second(local));
    return String(buffer);
}

// Funkcje pomocnicze dla LED
uint32_t interpolateColor(uint32_t color1, uint32_t color2, float ratio) {
    uint8_t r1, g1, b1, r2, g2, b2;
    colorToRGB(color1, r1, g1, b1);
    colorToRGB(color2, r2, g2, b2);
    
    uint8_t r = r1 + (r2 - r1) * ratio;
    uint8_t g = g1 + (g2 - g1) * ratio;
    uint8_t b = b1 + (b2 - b1) * ratio;
    
    return strip.Color(r, g, b);
}

void colorToRGB(uint32_t color, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

void initializeLEDs() {
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        ledStates[i].currentColor = strip.Color(0, 0, 0);
        ledStates[i].targetColor = strip.Color(0, 0, 0);
        ledStates[i].brightness = PULSE_MIN_BRIGHTNESS;
        ledStates[i].pulseDirection = 1;
        ledStates[i].pulsing = false;
    }
    strip.begin();
    strip.show();
}

void updateLEDs() {
    unsigned long currentMillis = millis();
    if (currentMillis - lastLedUpdate < LED_UPDATE_INTERVAL) {
        return;
    }
    lastLedUpdate = currentMillis;

    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        LEDState &state = ledStates[i];
        
        // Aktualizacja jasności dla efektu pulsowania
        if (state.pulseUp) {
            state.brightness += 5;
            if (state.brightness >= PULSE_MAX_BRIGHTNESS) {
                state.brightness = PULSE_MAX_BRIGHTNESS;
                state.pulseUp = false;
            }
        } else {
            state.brightness -= 5;
            if (state.brightness <= PULSE_MIN_BRIGHTNESS) {
                state.brightness = PULSE_MIN_BRIGHTNESS;
                state.pulseUp = true;
            }
        }

        // Zastosuj jasność do koloru
        uint8_t r, g, b;
        colorToRGB(state.currentColor, r, g, b);
        float brightnessRatio = state.brightness / 255.0;
        strip.setPixelColor(i, strip.Color(
            r * brightnessRatio,
            g * brightnessRatio,
            b * brightnessRatio
        ));
    }
    strip.show();
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

// Ustawienia domyślne konfiguracji
void setDefaultConfig() {
    // Podstawowa konfiguracja
    //config.version = CONFIG_VERSION;        // Ustawienie wersji konfiguracji
    config.soundEnabled = true;             // Włączenie powiadomień dźwiękowych
    
    // MQTT
    strlcpy(config.mqtt_server, "", sizeof(config.mqtt_server));
    config.mqtt_port = 1883;
    strlcpy(config.mqtt_user, "", sizeof(config.mqtt_user));
    strlcpy(config.mqtt_password, "", sizeof(config.mqtt_password));
    
    // Domyślna konfiguracja pomp
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        config.pumps[i].enabled = false;  // Pompy wyłączone domyślnie
        config.pumps[i].dosage = 10;   // Domyślna dawka 10ml
        config.pumps[i].hour = 8;         // Domyślna godzina dozowania 8:00
        config.pumps[i].minute = 0;       // Minuta 0
    }

    // Finalizacja
    config.checksum = calculateChecksum(config);  // Obliczenie sumy kontrolnej
    saveConfig();  // Zapis do EEPROM
    
    AQUA_DEBUG_PRINT(F("Utworzono domyślną konfigurację"));
}

// Ładowanie konfiguracji z pamięci EEPROM
bool loadConfig() {
    EEPROM.begin(sizeof(Config) + 1);  // +1 dla sumy kontrolnej
    
    // Tymczasowa struktura do wczytania danych
    Config tempConfig;
    
    // Wczytaj dane z EEPROM do tymczasowej struktury
    uint8_t *p = (uint8_t*)&tempConfig;
    for (size_t i = 0; i < sizeof(Config); i++) {
        p[i] = EEPROM.read(i);
    }
    
    EEPROM.end();

    // Debug - wyświetl wartości przed sprawdzeniem sumy kontrolnej
    AQUA_DEBUG_PRINT("Wczytane dane z EEPROM:");
    AQUA_DEBUG_PRINT("MQTT Server: " + String(tempConfig.mqtt_server));
    AQUA_DEBUG_PRINT("MQTT Port: " + String(tempConfig.mqtt_port));
    AQUA_DEBUG_PRINT("MQTT User: " + String(tempConfig.mqtt_user));
    
    char calculatedChecksum = calculateChecksum(tempConfig);
    AQUA_DEBUG_PRINT("Checksum stored: " + String(tempConfig.checksum));
    AQUA_DEBUG_PRINT("Checksum calculated: " + String(calculatedChecksum));
    
    // Sprawdź sumę kontrolną
    //char calculatedChecksum = calculateChecksum(tempConfig);
    if (calculatedChecksum == tempConfig.checksum) {
        // Jeśli suma kontrolna się zgadza, skopiuj dane do głównej struktury config
        memcpy(&config, &tempConfig, sizeof(Config));
        AQUA_DEBUG_PRINTF("Konfiguracja wczytana pomyślnie");
        return true;
    } else {
        AQUA_DEBUG_PRINTF("Błąd sumy kontrolnej - ładowanie ustawień domyślnych");
        setDefaultConfig();
        return false;
    }
}

// Zapis aktualnej konfiguracji do pamięci EEPROM
void saveConfig() {
    EEPROM.begin(sizeof(Config) + 1);  // +1 dla sumy kontrolnej
    
    // Oblicz sumę kontrolną przed zapisem
    config.checksum = calculateChecksum(config);
    
    // Zapisz strukturę do EEPROM
    uint8_t *p = (uint8_t*)&config;
    for (size_t i = 0; i < sizeof(Config); i++) {
        EEPROM.write(i, p[i]);
    }
    
    // Wykonaj faktyczny zapis do EEPROM
    bool success = EEPROM.commit();
    EEPROM.end();
    
    if (success) {
        AQUA_DEBUG_PRINTF("Konfiguracja zapisana pomyślnie");
    } else {
        AQUA_DEBUG_PRINTF("Błąd zapisu konfiguracji!");
    }
}

// Oblicz sumę kontrolną dla danej konfiguracji
char calculateChecksum(const Config& cfg) {
    char sum = 0;
    const char* ptr = (const char*)&cfg;
    for (size_t i = 0; i < sizeof(Config) - 1; i++) {
        sum ^= *ptr++;
    }
    return sum;
}

// ** FUNKCJE DŹWIĘKOWE **

// Odtwórz krótki dźwięk ostrzegawczy
void playShortWarningSound() {
    if (config.soundEnabled) {
        tone(BUZZER_PIN, 2000, 100); // Krótkie piknięcie (2000Hz, 100ms)
    }
}

// Odtwórz dźwięk potwierdzenia
void playConfirmationSound() {
    if (config.soundEnabled) {
        tone(BUZZER_PIN, 2000, 200); // Dłuższe piknięcie (2000Hz, 200ms)
    }
}

// ** FUNKCJE ALARMÓW I STEROWANIA POMPĄ **

// Inicjalizacja PCF8574
void setupPump() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        pcf8574.digitalWrite(config.pumps[i].pcf8574_pin, HIGH);  // HIGH = pompa wyłączona
        status.pumps[i].isRunning = false;
        status.pumps[i].lastDose = 0;
        status.pumps[i].totalDosed = 0;
    }
}

// Włączenie pompy
void turnOnPump(uint8_t pumpIndex) {
    if (pumpIndex < NUMBER_OF_PUMPS) {
        #if DEBUG
        AQUA_DEBUG_PRINTF("Turning ON pump %d\n", pumpIndex);
        #endif
        pcf8574.digitalWrite(config.pumps[pumpIndex].pcf8574_pin, LOW);  // LOW = pompa włączona
        status.pumps[pumpIndex].isRunning = true;
        updatePumpState(pumpIndex, true);
    }  
}

// Dozowanie określonej ilości
void dosePump(uint8_t pumpIndex) {
    if (!config.pumps[pumpIndex].enabled) return;
    
    float doseTime = (config.pumps[pumpIndex].dosage / config.pumps[pumpIndex].calibration) * 60000; // czas w ms
    
    turnOnPump(pumpIndex);
    delay(doseTime);
    turnOffPump(pumpIndex);
    
    status.pumps[pumpIndex].lastDose = millis();
    status.pumps[pumpIndex].totalDosed += config.pumps[pumpIndex].dosage;
    
    // Aktualizacja MQTT
    updateHAState(pumpIndex);
}

// Wyłączenie pompy
void turnOffPump(uint8_t pumpIndex) {
    if (pumpIndex < NUMBER_OF_PUMPS) {
        #if DEBUG
        AQUA_DEBUG_PRINTF("Turning OFF pump %d\n", pumpIndex);
        #endif
        pcf8574.digitalWrite(config.pumps[pumpIndex].pcf8574_pin, HIGH);  // HIGH = pompa wyłączona
        status.pumps[pumpIndex].isRunning = false;
        updatePumpState(pumpIndex, false);
    }
}

// Bezpieczne wyłączenie wszystkich pomp
void stopAllPumps() {
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        turnOffPump(i);
    }
}

void testPump(uint8_t pumpId) {
    // Kod testujący pompę
    // Na przykład: włącz pompę na 5 sekund
    digitalWrite(config.pumps[pumpId].pcf8574_pin, HIGH);
    delay(5000);
    digitalWrite(config.pumps[pumpId].pcf8574_pin, LOW);
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
    device.setName("AquaDoser");  // Nazwa urządzenia
    device.setModel("AD ESP8266");  // Model urządzenia
    device.setManufacturer("PMW");  // Producent
    device.setSoftwareVersion(SOFTWARE_VERSION);  // Wersja oprogramowania

    // Tworzenie sensorów dla aktualnego stanu pomp
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        char uniqueId[32];
        sprintf(uniqueId, "stan_pumpy_%d", i + 1);
        
        pumpStates[i] = new HABinarySensor(uniqueId);
        //pumpStates[i]->setName(String("Stan Pompy ") + String(i + 1));
        String nazwaPompy = String("Stan Pompy ") + String(i + 1);
        pumpStates[i]->setName(nazwaPompy.c_str());
        pumpStates[i]->setDeviceClass("running");
    }
    
    // Tworzenie przełączników do aktywacji harmonogramu
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
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
    switchSound.setIcon("mdi:volume-high");        // Ikona głośnika
    switchSound.onCommand(onSoundSwitchCommand);   // Funkcja obsługi zmiany stanu

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

    pinMode(BUZZER_PIN, OUTPUT);  // Wyjście - buzzer
    digitalWrite(BUZZER_PIN, LOW);  // Wyłączenie buzzera

    pcf8574.begin();
    
    // Ustaw wszystkie piny jako wyjścia
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        pcf8574.pinMode(i, OUTPUT);
        pcf8574.digitalWrite(i, HIGH);  // HIGH = wyłączone (logika ujemna)
    }
}

// Efekt powitalny
void playWelcomeEffect() {
    // Efekt 1: Przebiegające światło (1s)
    for(int j = 0; j < 2; j++) {  // Dwa przebiegi
        for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
            strip.setPixelColor(i, COLOR_RAINBOW_1);
            if(i > 0) strip.setPixelColor(i-1, COLOR_OFF);
            strip.show();
            delay(100);
        }
        strip.setPixelColor(NUMBER_OF_PUMPS-1, COLOR_OFF);
        strip.show();
    }
    
    // Efekt 2: Tęczowa fala (1s)
    for(int j = 0; j < 2; j++) {  // Dwa przebiegi
        uint32_t colors[] = {COLOR_RAINBOW_1, COLOR_RAINBOW_2, COLOR_RAINBOW_3, 
                           COLOR_RAINBOW_4, COLOR_RAINBOW_5, COLOR_RAINBOW_6};
        for(int c = 0; c < 6; c++) {
            for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
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
    for(uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
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
            } else {  // Przycisk zwolniony
                buttonState.releasedTime = millis();
                
                // Sprawdzenie czy to było krótkie naciśnięcie
                if (buttonState.releasedTime - buttonState.pressedTime < LONG_PRESS_TIME) {
                    // Przełącz tryb serwisowy
                    status.isServiceMode = !status.isServiceMode;
                    playConfirmationSound();  // Sygnał potwierdzenia zmiany trybu
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
                ESP.wdtFeed();  // Reset przy długim naciśnięciu
                status.pumpSafetyLock = false;  // Zdjęcie blokady pompy
                playConfirmationSound();  // Sygnał potwierdzenia zmiany trybu
                //switchPumpAlarm.setState(false, true);  // force update w HA
                buttonState.isLongPressHandled = true;  // Oznacz jako obsłużone
                AQUA_DEBUG_PRINT("Alarm pompy skasowany");
            }
        }
    }
    
    lastReading = reading;  // Zapisz ostatni odczyt dla następnego porównania
    yield();  // Oddaj sterowanie systemowi
}

// Obsługa przełącznika dźwięku (HA)
void onSoundSwitchCommand(bool state, HASwitch* sender) {   
    status.soundEnabled = state;  // Aktualizuj status lokalny
    config.soundEnabled = state;  // Aktualizuj konfigurację
    saveConfig();  // Zapisz do EEPROM
    
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
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if(sender == pumpSchedules[i]) {
            pumpIndex = i;
            break;
        }
    }
    
    if(pumpIndex >= 0) {
        // Tu później dodamy kod obsługi włączania/wyłączania harmonogramu
        // Na razie tylko aktualizujemy stan w HA
        sender->setState(state);
    }
}

// Obsługuje komendę przełącznika trybu serwisowego
void onServiceSwitchCommand(bool state, HASwitch* sender) {
    playConfirmationSound();  // Sygnał potwierdzenia zmiany trybu
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
    } else {  // Wyłączanie trybu serwisowego
        // Reset stanu opóźnienia pompy aby umożliwić normalne uruchomienie
        //status.isPumpDelayActive = false;
        //status.pumpDelayStartTime = 0;
        // Normalny tryb pracy - pompa uruchomi się automatycznie 
        // jeśli czujnik poziomu wykryje wodę
    }
    
    AQUA_DEBUG_PRINTF("Tryb serwisowy: %s (przez HA)\n", state ? "WŁĄCZONY" : "WYŁĄCZONY");
}

String getConfigPage() {
    if (!LittleFS.exists("/index.html")) {
        return F("Error: index.html not found in LittleFS");
    }

    File file = LittleFS.open("/index.html", "r");
    String html = file.readString();
    file.close();

    // Zastąp placeholdery dla statusu
    html.replace(F("%MQTT_STATUS%"), client.connected() ? F("Połączony") : F("Rozłączony"));
    html.replace(F("%MQTT_STATUS_CLASS%"), client.connected() ? F("success") : F("error"));
    html.replace(F("%SOUND_STATUS%"), config.soundEnabled ? F("Włączony") : F("Wyłączony"));
    html.replace(F("%SOUND_STATUS_CLASS%"), config.soundEnabled ? F("success") : F("error"));
    html.replace(F("%SOFTWARE_VERSION%"), SOFTWARE_VERSION);
    html.replace(F("%CURRENT_TIME%"), getFormattedDateTime());
    
    // Generuj formularze konfiguracyjne
    String configForms = F("<form method='POST' action='/save'>");
    
    // Sekcja MQTT
    configForms += F("<div class='section'>"
                     "<h2>Konfiguracja MQTT</h2>"
                     "<table class='config-table'>");
    
    configForms += F("<tr><td>Serwer</td><td><input type='text' name='mqtt_server' value='");
    configForms += config.mqtt_server;
    configForms += F("'></td></tr>");
    
    configForms += F("<tr><td>Port</td><td><input type='number' name='mqtt_port' value='");
    configForms += String(config.mqtt_port);
    configForms += F("'></td></tr>");
    
    configForms += F("<tr><td>Użytkownik</td><td><input type='text' name='mqtt_user' value='");
    configForms += config.mqtt_user;
    configForms += F("'></td></tr>");
    
    configForms += F("<tr><td>Hasło</td><td><input type='password' name='mqtt_password' value='");
    configForms += config.mqtt_password;
    configForms += F("'></td></tr></table></div>");

    // Sekcja pomp
    configForms += F("<div class='section'>"
                     "<h2>Konfiguracja pomp</h2>");
    
    // Dla każdej pompy
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        configForms += F("<div class='pump-section'>"
                        "<h3>Pompa ");
        configForms += String(i + 1);
        configForms += F("</h3>"
                        "<table class='config-table'>");
        
        // Nazwa pompy
        configForms += F("<tr><td>Nazwa</td><td><input type='text' name='p");
        configForms += String(i);
        configForms += F("_name' value='");
        configForms += config.pumps[i].name[0] ? String(config.pumps[i].name) : String("Pompa ") + String(i + 1);
        configForms += F("'></td></tr>");
        
        // Aktywna
        configForms += F("<tr><td>Aktywna</td><td><input type='checkbox' name='p");
        configForms += String(i);
        configForms += F("_enabled' ");
        configForms += config.pumps[i].enabled ? F("checked") : F("");
        configForms += F("></td></tr>");
        
        // Kalibracja
        configForms += F("<tr><td>Kalibracja (ml/min)</td><td><input type='number' step='0.1' name='p");
        configForms += String(i);
        configForms += F("_calibration' value='");
        configForms += String(config.pumps[i].calibration);
        configForms += F("'></td></tr>");
        
        // Dozowanie
        configForms += F("<tr><td>Dozowanie (ml)</td><td><input type='number' name='p");
        configForms += String(i);
        configForms += F("_dosage' value='");
        configForms += String(config.pumps[i].dosage);
        configForms += F("'></td></tr>");
        
        // Godzina i minuta
        configForms += F("<tr><td>Godzina dozowania</td><td><input type='number' min='0' max='23' name='p");
        configForms += String(i);
        configForms += F("_hour' value='");
        configForms += String(config.pumps[i].hour);
        configForms += F("'></td></tr>");
        
        configForms += F("<tr><td>Minuta dozowania</td><td><input type='number' min='0' max='59' name='p");
        configForms += String(i);
        configForms += F("_minute' value='");
        configForms += String(config.pumps[i].minute);
        configForms += F("'></td></tr>");
        
        // Dni tygodnia
        configForms += F("<tr><td>Dni tygodnia</td><td class='weekdays'>");
        const char* days[] = {"Pn", "Wt", "Śr", "Cz", "Pt", "Sb", "Nd"};
        for (int day = 0; day < 7; day++) {
            configForms += F("<label><input type='checkbox' name='p");
            configForms += String(i);
            configForms += F("_day");
            configForms += String(day);
            configForms += F("' ");
            configForms += (config.pumps[i].weekDays & (1 << day)) ? F("checked") : F("");
            configForms += F("><span>");
            configForms += days[day];
            configForms += F("</span></label>");
        }
        configForms += F("</td></tr>");

        // Status
        configForms += F("<tr><td>Status</td><td><span class='pump-status ");
        configForms += config.pumps[i].enabled ? F("active") : F("inactive");
        configForms += F("'>");
        configForms += config.pumps[i].enabled ? F("Aktywna") : F("Nieaktywna");
        configForms += F("</span></td></tr>");

        // Test pompy
        configForms += F("<tr><td colspan='2'><button type='button' class='btn btn-blue test-pump' data-pump='");
        configForms += String(i);
        configForms += F("'>Test pompy</button></td></tr>");
        
        configForms += F("</table></div>");
    }

    // Przycisk zapisu
    configForms += F("<div class='section'>"
                     "<input type='submit' value='Zapisz ustawienia' class='btn btn-blue'>"
                     "</div></form>");

    html.replace(F("%CONFIG_FORMS%"), configForms);
    
    // Dodaj przyciski
    String buttons = F("<div class='section'>"
                      "<div class='buttons-container'>"
                      "<button class='btn btn-blue' onclick='rebootDevice()'>Restart urządzenia</button>"
                      "<button class='btn btn-red' onclick='factoryReset()'>Przywróć ustawienia fabryczne</button>"
                      "</div>"
                      "</div>");
    html.replace(F("%BUTTONS%"), buttons);

    // Dodaj formularz aktualizacji
    String updateForm = F("<div class='section'>"
                         "<h2>Aktualizacja firmware</h2>"
                         "<form method='POST' action='/update' enctype='multipart/form-data'>"
                         "<table class='config-table' style='margin-bottom: 15px;'>"
                         "<tr><td colspan='2'><input type='file' name='update' accept='.bin'></td></tr>"
                         "</table>"
                         "<input type='submit' value='Aktualizuj firmware' class='btn btn-orange'>"
                         "</form>"
                         "<div id='update-progress' style='display:none'>"
                         "<div class='progress'>"
                         "<div id='progress-bar' class='progress-bar' role='progressbar' style='width: 0%'>0%</div>"
                         "</div>"
                         "</div>"
                         "</div>");
    html.replace(F("%UPDATE_FORM%"), updateForm);

    // Dodaj stopkę
    String footer = F("<div class='footer'>"
                     "<a href='https://github.com/pimowo/AquaDoser' target='_blank'>Project by PMW</a>"
                     "</div>");
    html.replace(F("%FOOTER%"), footer);

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
            String wiadomosc = "save:error:Kalibracja pompy " + String(i+1) + " musi być większa od 0";
            webSocket.broadcastTXT(wiadomosc);
            return false;
        }

        // Sprawdź dozowanie (nie może być ujemne)
        int dosage = server.arg("p" + String(i) + "_dosage").toInt();
        if (dosage < 0) {
            String message = "save:error:Dozowanie pompy " + String(i+1) + " nie może być ujemne";
            webSocket.broadcastTXT(message);
            return false;
        }

        // Sprawdź godzinę (0-23)
        int hour = server.arg("p" + String(i) + "_hour").toInt();
        if (hour < 0 || hour > 23) {
            String message2 = "save:error:Nieprawidłowa godzina dla pompy " + String(i+1);
            webSocket.broadcastTXT(message2);
            return false;
        }

        // Sprawdź minutę (0-59)
        int minute = server.arg("p" + String(i) + "_minute").toInt();
        if (minute < 0 || minute > 59) {
            String message3 = "save:error:Nieprawidłowa minuta dla pompy " + String(i+1);
            webSocket.broadcastTXT(message3);
            return false;
        }
    }
    return true;
}

// Obsługa zapisania konfiguracji po jej wprowadzeniu przez użytkownika
void handleSave() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    // Debug - pokaż otrzymane dane
    AQUA_DEBUG_PRINT("Otrzymane dane z formularza:");
    AQUA_DEBUG_PRINT("MQTT Server: " + server.arg("mqtt_server"));
    AQUA_DEBUG_PRINT("MQTT Port: " + server.arg("mqtt_port"));
    AQUA_DEBUG_PRINT("MQTT User: " + server.arg("mqtt_user"));
    
    // Zapisz poprzednie wartości na wypadek błędów
    Config oldConfig = config;
    bool needMqttReconnect = false;

    // Zapisz poprzednie wartości MQTT do porównania
    String oldServer = config.mqtt_server;
    int oldPort = config.mqtt_port;
    String oldUser = config.mqtt_user;
    String oldPassword = config.mqtt_password;

    // Zapisz ustawienia MQTT
    strlcpy(config.mqtt_server, server.arg("mqtt_server").c_str(), sizeof(config.mqtt_server));
    config.mqtt_port = server.arg("mqtt_port").toInt();
    strlcpy(config.mqtt_user, server.arg("mqtt_user").c_str(), sizeof(config.mqtt_user));
    strlcpy(config.mqtt_password, server.arg("mqtt_password").c_str(), sizeof(config.mqtt_password));
    
    // Debug - pokaż zapisane wartości
    AQUA_DEBUG_PRINT("Zapisane wartości:");
    AQUA_DEBUG_PRINT("MQTT Server: " + String(config.mqtt_server));
    AQUA_DEBUG_PRINT("MQTT Port: " + String(config.mqtt_port));
    AQUA_DEBUG_PRINT("MQTT User: " + String(config.mqtt_user));

    String pumpPrefix;
    String doseArg;

    // Zapisz konfigurację pomp
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        // Nazwa
        String pumpName = server.arg("p" + String(i) + "_name");
        strlcpy(config.pumps[i].name, pumpName.c_str(), sizeof(config.pumps[i].name));

        // Aktywna
        config.pumps[i].enabled = server.hasArg("p" + String(i) + "_enabled");

        // Kalibracja
        config.pumps[i].calibration = server.arg("p" + String(i) + "_calibration").toFloat();

        // Dozowanie
        config.pumps[i].dosage = server.arg("p" + String(i) + "_dosage").toInt();

        // Godzina i minuta
        config.pumps[i].hour = server.arg("p" + String(i) + "_hour").toInt();
        config.pumps[i].minute = server.arg("p" + String(i) + "_minute").toInt();

        // Dni tygodnia
        uint8_t weekDays = 0;
        for (int day = 0; day < 7; day++) {
            if (server.hasArg("p" + String(i) + "_day" + String(day))) {
                weekDays |= (1 << day);
            }
        }
        config.pumps[i].weekDays = weekDays;

        // Debug
        AQUA_DEBUG_PRINTF("Zapisano konfigurację pompy %d:\n", i + 1);
        AQUA_DEBUG_PRINTF("  Nazwa: %s\n", config.pumps[i].name);
        AQUA_DEBUG_PRINTF("  Aktywna: %d\n", config.pumps[i].enabled);
        AQUA_DEBUG_PRINTF("  Kalibracja: %.1f\n", config.pumps[i].calibration);
        AQUA_DEBUG_PRINTF("  Dozowanie: %d\n", config.pumps[i].dosage);
        AQUA_DEBUG_PRINTF("  Czas: %02d:%02d\n", config.pumps[i].hour, config.pumps[i].minute);
        AQUA_DEBUG_PRINTF("  Dni: 0b%08b\n", config.pumps[i].weekDays);
    }

    // Sprawdź poprawność wartości
    if (!validateConfigValues()) {
        config = oldConfig; // Przywróć poprzednie wartości
        //webSocket.broadcastTXT("save:error:Nieprawidłowe wartości! Sprawdź wprowadzone dane.");
        server.send(204); // Nie przekierowuj strony
        return;
    }

    // Sprawdź czy dane MQTT się zmieniły
    if (oldServer != config.mqtt_server ||
        oldPort != config.mqtt_port ||
        oldUser != config.mqtt_user ||
        oldPassword != config.mqtt_password) {
        needMqttReconnect = true;
    }

    // Zapisz konfigurację do EEPROM
    saveConfig();

    // Jeśli dane MQTT się zmieniły, zrestartuj połączenie
    // if (needMqttReconnect) {
    //     if (mqtt.isConnected()) {
    //         mqtt.disconnect();
    //     }
    //     connectMQTT();
    // }

    // Wyślij informację o sukcesie przez WebSocket
    //webSocket.broadcastTXT("save:success:Zapisano ustawienia!");
    server.send(204); // Nie przekierowuj strony
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
    } 
    else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
            webSocket.broadcastTXT("update:error:Write failed");
            return;
        }
        // Aktualizacja paska postępu
        int progress = (upload.totalSize * 100) / upload.contentLength;
        String progressMsg = "update:" + String(progress);
        webSocket.broadcastTXT(progressMsg);
    } 
    else if (upload.status == UPLOAD_FILE_END) {
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

void updatePumpState(uint8_t pumpIndex, bool state) {
    //pumps[pumpIndex].state = state;
    //pumps[pumpIndex].haSwitch->setState(state); // Aktualizuj stan w HA

    if (pumpIndex < NUMBER_OF_PUMPS && pumpStates[pumpIndex] != nullptr) {
        pumpStates[pumpIndex]->setState(state, true);
        
        // Aktualizacja sensora statusu
        String statusText = "Pompa_" + String(pumpIndex + 1) + 
                          (state ? "ON" : "OFF");
        //sensorPump.setValue(statusText.c_str());
        
        //mqtt.loop(); // Wymuszenie aktualizacji
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
    server.on("/update", HTTP_POST, handleUpdateResult, handleDoUpdate);
    server.on("/save", handleSave);
    
    server.on("/reboot", HTTP_POST, []() {
        server.send(200, "text/plain", "Restarting...");
        delay(1000);
        ESP.restart();
    });

    // Obsługa resetu przez WWW
    server.on("/factory-reset", HTTP_POST, []() {
        server.send(200, "text/plain", "Resetting to factory defaults...");
        delay(200);  // Daj czas na wysłanie odpowiedzi
        factoryReset();  // Wywołaj tę samą funkcję co przy resecie fizycznym
    });
    
    // Dodaj obsługę plików CSS i JS
    server.serveStatic("/css/style.css", LittleFS, "/css/style.css", "max-age=86400");
    server.serveStatic("/js/main.js", LittleFS, "/js/main.js", "max-age=86400");

    server.begin();
}

// ** WEBSOCKET I KOMUNIKACJA W SIECI **

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
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
    if(!LittleFS.begin()) {
        Serial.println(F("Błąd montowania LittleFS"));
        return;
    }
    Serial.println(F("LittleFS zamontowany pomyślnie"));
    
    // Wczytaj konfigurację na początku
    if (!loadConfig()) {
        AQUA_DEBUG_PRINTF("Błąd wczytywania konfiguracji - używam ustawień domyślnych");
        setDefaultConfig();
        saveConfig();  // Zapisz domyślną konfigurację do EEPROM
    }

    // Debug - sprawdź zawartość systemu plików
    Dir dir = LittleFS.openDir("/");
    while (dir.next()) {
        String fileName = dir.fileName();
        size_t fileSize = dir.fileSize();
        Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), String(fileSize).c_str());
    }

    setupPin();
    setupPump();
  
    WiFiManager wifiManager;
    wifiManager.autoConnect("AquaDoser");  // Samo zadba o połączenie
    setupWebServer();
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    setupHA();
    
    // Inicjalizacja RTC
    if (!initRTC()) {
        // Obsługa błędu inicjalizacji RTC
        AQUA_DEBUG_PRINT(F("Błąd inicjalizacji RTC!"));
    }
    
    // Po inicjalizacji WiFi
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    delay(2000);

    // Poczekaj na synchronizację czasu
    time_t now = time(nullptr);
    while (now < 24 * 3600) {
        delay(500);
        now = time(nullptr);
    }

    //debugPrint("Czas zsynchronizowany");

    // Wysyłamy zapisane daty kalibracji do HA
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        publishCalibrationDate(i);
    }

    // Konfiguracja OTA
    ArduinoOTA.setHostname("AquaDoser");  // Ustaw nazwę urządzenia
    ArduinoOTA.setPassword("aquadoser");  // Ustaw hasło dla OTA
    ArduinoOTA.begin();  // Uruchom OTA   

    // LED
    strip.begin();
    strip.show();
    initializeLEDs();

    // Efekty startowe
    playWelcomeEffect();
    welcomeMelody();
}

// ** Funkcja loop - główny cykl pracy urządzenia **

void loop() {
    unsigned long currentMillis = millis();
        
    handleMillisOverflow();  // Obsługa przepełnienia licznika millis()   
    mqtt.loop();  // Obsługa MQTT
    server.handleClient();  // Obsługa serwera WWW
    updateLEDs();                  // Aktualizacja diod LED
    
    // Aktualizacja stanu sensorów na podstawie PCF8574
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        //bool pumpState = !pcf8574.read(i);  // Negacja bo logika ujemna
        bool pumpState = !pcf8574.digitalRead(i); 
        pumpStates[i]->setState(pumpState);
    }

    // Sprawdź czy należy zakończyć test pompy
    if (testingPumpId >= 0 && millis() >= pumpTestEndTime) {
        pcf8574.digitalWrite(config.pumps[testingPumpId].pcf8574_pin, LOW);
        //webSocket.broadcastTXT("pump_test:finished:" + String(testingPumpId));
        String message5 = "pump_test:finished:" + String(testingPumpId);
        webSocket.broadcastTXT(message5);
        testingPumpId = -1;
    }
    
    // Dodatkowe zabezpieczenie - wyłącz wszystkie pompy w trybie serwisowym
    if (status.isServiceMode) {
        stopAllPumps();
    }

    if (currentMillis - timers.lastOTACheck >= OTA_CHECK_INTERVAL) {
        ArduinoOTA.handle();                  // Obsługa aktualizacji OTA
        timers.lastOTACheck = currentMillis;  // Aktualizacja znacznika czasu ostatniego sprawdzenia OTA
    }

    // Synchronizacja RTC z NTP raz na dobę
    if (millis() - lastNTPSync >= NTP_SYNC_INTERVAL) {
        syncTimeFromNTP();
    }

    yield();    // Obsługa zadań systemowych ESP8266
}
