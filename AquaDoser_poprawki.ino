/***************************************
 * BIBLIOTEKI
 ***************************************/

class HASensor;

// --- System i komunikacja
#include <Arduino.h>     // Główna biblioteka Arduino
#include <ArduinoHA.h>   // Integracja z Home Assistant
#include <ArduinoOTA.h>  // Aktualizacja przez WiFi

// --- Sieć
#include <ESP8266WiFi.h>              // WiFi dla ESP8266
#include <WiFiManager.h>              // Zarządzanie WiFi
#include <ESP8266WebServer.h>         // Serwer WWW
#include <WebSocketsServer.h>         // WebSocket
#include <ESP8266HTTPUpdateServer.h>  // Serwer aktualizacji
#include <WiFiUDP.h>                  // UDP dla NTP

// --- Pamięć
#include <EEPROM.h>      // Pamięć EEPROM

// --- I2C i czujniki
#include <Wire.h>        // Magistrala I2C
#include <RTClib.h>      // Zegar RTC
#include <PCF8574.h>     // Ekspander I/O

// --- Pozostałe
#include <ArduinoJson.h>        // Obsługa JSON
#include <Adafruit_NeoPixel.h>  // Sterowanie LED
#include <NTPClient.h>          // Klient NTP

#define SYSTEM_CONFIG_ADDR 0
#define MQTT_CONFIG_ADDR 100
#define NETWORK_CONFIG_ADDR 200
#define PUMPS_CONFIG_ADDR 300
#define DEBOUNCE_TIME 50

/***************************************
 * DEFINICJE STAŁYCH
 ***************************************/

// Zmienna przechowująca wersję oprogramowania
const char* VERSION = "1.12.24";  // Definiowanie wersji oprogramowania

// --- Piny GPIO
const int NUMBER_OF_PUMPS = 8;    // Liczba pomp
const int BUZZER_PIN = 13;        // Buzzer
const int LED_PIN = 12;           // Pasek LED
const int BUTTON_PIN = 14;        // Przycisk
const int SDA_PIN = 4;            // I2C - SDA
const int SCL_PIN = 5;            // I2C - SCL
const int PCF8574_ADDRESS = 0x20; // Adres ekspandera I2C

// --- Parametry LED
#define PULSE_MAX_BRIGHTNESS 255   // Maksymalna jasność
#define PULSE_MIN_BRIGHTNESS 50    // Minimalna jasność
#define LED_UPDATE_INTERVAL 50     // Częstotliwość odświeżania (ms)
#define FADE_STEPS 20             // Kroki animacji

// --- Kolory LED
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

// --- Parametry MQTT
#define MQTT_SERVER_LENGTH 40     // Długość adresu serwera
#define MQTT_USER_LENGTH 20       // Długość nazwy użytkownika
#define MQTT_PASSWORD_LENGTH 20   // Długość hasła

const unsigned long STATUS_PRINT_INTERVAL = 60000; // Wyświetlaj status co 1 minutę
const unsigned long MILLIS_OVERFLOW_THRESHOLD = 4294967295U - 60000; // ~49.7 dni

// --- Interwały czasowe (ms)
#define LOG_INTERVAL 60000            // Logowanie
#define SYSTEM_CHECK_INTERVAL 5000    // Sprawdzanie systemu
#define DOSE_CHECK_INTERVAL 1000      // Sprawdzanie dozowania co 1 sekundę
#define MIN_DOSE_TIME 100            // Minimalny czas dozowania (ms)
#define MAX_DOSE_TIME 60000          // Maksymalny czas dozowania (60 sekund)
#define WEBSOCKET_UPDATE_INTERVAL 1000 // Aktualizacja WebSocket
#define RTC_SYNC_INTERVAL 86400000UL  // Synchronizacja RTC (24h)
#define MQTT_LOOP_INTERVAL 100        // Pętla MQTT
#define OTA_CHECK_INTERVAL 1000       // Sprawdzanie OTA
#define PUMP_CHECK_INTERVAL 1000      // Sprawdzanie pomp
#define BUTTON_DEBOUNCE_TIME 50       // Opóźnienie przycisku
#define WIFI_RECONNECT_DELAY 5000     // Ponowne łączenie WiFi
#define MQTT_RECONNECT_DELAY 5000     // Ponowne łączenie MQTT
#define HA_UPDATE_INTERVAL 30000      // Aktualizacja Home Assistant

/***************************************
 * STRUKTURY DANYCH
 ***************************************/

// --- Konfiguracja pompy
struct PumpConfig {
    char name[20];
    bool enabled;
    float calibration;
    float dose;
    uint8_t schedule_hour;
    uint8_t minute;
    uint8_t hour;        
    uint8_t schedule_days;
    time_t lastDosing;
    bool isRunning;
};

// --- Stan pompy
struct PumpState {
    bool isActive;
    unsigned long lastDoseTime;
    HASensor* sensor;
    
    // Dodane pola dla kalibracji
    bool isCalibrating;
    unsigned long calibrationStartTime;
    unsigned long calibrationDuration;
    bool calibrationCompleted;
    
    PumpState() : isActive(false), lastDoseTime(0), sensor(nullptr),
                  isCalibrating(false), calibrationStartTime(0), 
                  calibrationDuration(0), calibrationCompleted(false) {}
};

// --- Konfiguracja MQTT
struct MQTTConfig {
    char broker[64];
    int port;
    char username[32];
    char password[32];
    
    MQTTConfig() : port(1883) {
        memset(broker, 0, sizeof(broker));
        memset(username, 0, sizeof(username));
        memset(password, 0, sizeof(password));
    }
};

// --- Konfiguracja sieci
struct NetworkConfig {
    char hostname[32];
    char ssid[32];
    char password[32];
    char mqtt_server[64];
    int mqtt_port;
    char mqtt_user[32];
    char mqtt_password[32];
    bool dhcp;    // Dodane pole dhcp
};

// --- Konfiguracja systemu
struct SystemConfig {
    bool soundEnabled;
};

// --- Stan systemu
struct SystemStatus {
    bool mqtt_connected;
    bool wifi_connected;
    unsigned long uptime;
};

// --- Informacje systemowe
struct SystemInfo {
    unsigned long uptime;
    bool mqtt_connected;
};

// --- Stan LED
struct LEDState {
    uint32_t currentColor;
    uint32_t targetColor;
    unsigned long lastUpdateTime;
    bool immediate;
    uint8_t brightness;
    bool pulsing;
    int8_t pulseDirection;
    
    LEDState() : currentColor(0), targetColor(0), lastUpdateTime(0), 
                 immediate(false), brightness(255), pulsing(false), 
                 pulseDirection(1) {}
};

/***************************************
 * ZMIENNE GLOBALNE
 ***************************************/

// --- Obiekty sprzętowe
RTC_DS3231 rtc;                // Zegar RTC
PCF8574 pcf8574(PCF8574_ADDRESS);  // Ekspander I/O
Adafruit_NeoPixel strip(NUMBER_OF_PUMPS, LED_PIN, NEO_GRB + NEO_KHZ800); // LED

ESP8266HTTPUpdateServer httpUpdateServer;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600);

// --- Obiekty sieciowe
WiFiClient client;
ESP8266WebServer server(80);
WebSocketsServer webSocket(81);
HADevice device("aquadoser");
HAMqtt mqtt(client, device);
HASwitch* pumpSwitches[NUMBER_OF_PUMPS];
HANumber* pumpCalibrations[NUMBER_OF_PUMPS];
HASwitch* serviceModeSwitch;

/***************************************
 * ZMIENNE GLOBALNE - STAN SYSTEMU
 ***************************************/

// --- Stan pomp
PumpConfig pumps[NUMBER_OF_PUMPS];           // Konfiguracja pomp
PumpState pumpStates[NUMBER_OF_PUMPS];       // Stan pomp
bool pumpRunning[NUMBER_OF_PUMPS] = {false}; // Flagi pracy pomp
unsigned long doseStartTime[NUMBER_OF_PUMPS] = {0}; // Czas rozpoczęcia dozowania
bool pumpInitialized = false;                // Flaga inicjalizacji pomp

// --- Stan sieci i MQTT
NetworkConfig networkConfig;         // Konfiguracja sieci
MQTTConfig mqttConfig;              // Konfiguracja MQTT
bool mqttEnabled = false;           // Stan MQTT
char mqttServer[MQTT_SERVER_LENGTH] = "";
uint16_t mqttPort = 1883;
char mqttUser[MQTT_USER_LENGTH] = "";
char mqttPassword[MQTT_PASSWORD_LENGTH] = "";

// --- Stan systemu
SystemConfig systemConfig;           // Konfiguracja systemowa
SystemStatus systemStatus;           // Status systemu
SystemInfo sysInfo = {0, false};    // Informacje systemowe
bool serviceMode = false;           // Tryb serwisowy
bool shouldRestart = false;         // Flaga restartu
bool firstRun = true;               // Pierwsze uruchomienie

// --- Stan LED
LEDState ledStates[NUMBER_OF_PUMPS];  // Stan diod LED

/***************************************
 * ZMIENNE CZASOWE I LICZNIKI
 ***************************************/

// --- Czas systemowy
DateTime now;                    // Aktualny czas
uint8_t currentDay;             // Aktualny dzień
uint8_t currentHour;            // Aktualna godzina
uint8_t currentMinute;          // Aktualna minuta
const char* dayNames[] = {"Pn", "Wt", "Śr", "Cz", "Pt", "Sb", "Nd"};

// --- Liczniki i ostatnie wykonania
unsigned long lastButtonPress = 0;    // Ostatnie naciśnięcie przycisku
bool lastButtonState = HIGH;
unsigned long lastMQTTLoop = 0;       // Ostatnia pętla MQTT
unsigned long lastMeasurement = 0;    // Ostatni pomiar
unsigned long lastOTACheck = 0;       // Ostatnie sprawdzenie OTA
unsigned long lastDoseCheck = 0;      // Ostatnie sprawdzenie dozowania
unsigned long lastLedUpdate = 0;      // Ostatnia aktualizacja LED
unsigned long lastHaUpdate = 0;       // Ostatnia aktualizacja HA
unsigned long lastLogTime = 0;        // Ostatni zapis logu
unsigned long restartTime = 0;        // Czas restartu
unsigned long lastStatusPrint = 0;    // Ostatni wydruk statusu

/***************************************
 * DEKLARACJE FUNKCJI
 ***************************************/
 
// Deklaracje funkcji LED - dodaj na początku pliku
void setLEDColor(uint8_t index, uint32_t color, bool immediate = false);
uint32_t interpolateColor(uint32_t color1, uint32_t color2, float ratio);
void colorToRGB(uint32_t color, uint8_t &r, uint8_t &g, uint8_t &b);

// --- Funkcje inicjalizacyjne
// Inicjalizacja pamięci
void initStorage() {
    systemConfig.soundEnabled = true;  // wartość domyślna
}

// Inicjalizacja LED
void initializeLEDs() {
    strip.begin();
    strip.show();
    
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        ledStates[i] = LEDState();  // Używamy konstruktora domyślnego
    }
}

// Inicjalizacja pomp
bool initializePumps() {
    if (!pcf8574.begin()) {
        Serial.println("Błąd inicjalizacji PCF8574!");
        return false;
    }

    // Ustaw wszystkie piny jako wyjścia i wyłącz pompy
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        pcf8574.digitalWrite(i, LOW);  // Nie potrzeba pinMode
    }

    pumpInitialized = true;
    return true;
}

// Konfiguracja MQTT z Home Assistant
void setupHA() {
    // Konfiguracja urządzenia dla Home Assistant
    device.setName("AquaDoser");  
    device.setModel("AD ESP8266");  
    device.setManufacturer("PMW");  
    device.setSoftwareVersion("1.0.0");

    // Konfiguracja pomp w HA
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        // Switch do włączania/wyłączania pompy
        pumpSwitches[i] = new HASwitch(String("pump_" + String(i + 1)).c_str());
        pumpSwitches[i]->setName(String("Pompa " + String(i + 1)).c_str());
        pumpSwitches[i]->setIcon("mdi:water-pump");
        pumpSwitches[i]->onCommand(onPumpSwitch);

        // Sensor stanu pompy
        pumpStates[i].sensor = new HASensor(String("pump_state_" + String(i + 1)).c_str());
        pumpStates[i].sensor->setName(String("Stan pompy " + String(i + 1)).c_str());
        pumpStates[i].sensor->setIcon("mdi:water-pump-off");

        // Kalibracja pompy
        pumpCalibrations[i] = new HANumber(String("pump_calibration_" + String(i + 1)).c_str());
        pumpCalibrations[i]->setName(String("Kalibracja pompy " + String(i + 1)).c_str());
        pumpCalibrations[i]->setIcon("mdi:ruler");
        pumpCalibrations[i]->setMin(0);
        pumpCalibrations[i]->setMax(100);
        pumpCalibrations[i]->setStep(0.1);
        pumpCalibrations[i]->onCommand(onPumpCalibration);
    }

    // Przełącznik trybu serwisowego
    serviceModeSwitch = new HASwitch("service_mode");
    serviceModeSwitch->setName("Tryb serwisowy");
    serviceModeSwitch->setIcon("mdi:wrench");
    serviceModeSwitch->onCommand(onServiceModeSwitch);
}

// --- Funkcje konfiguracyjne
// Wczytanie konfiguracji
void loadConfiguration() {
    loadMQTTConfig();
    loadPumpsConfig();
    loadNetworkConfig();
    EEPROM.get(SYSTEM_CONFIG_ADDR, systemConfig);
    
    if (!validateConfigValues()) {
        resetFactorySettings();
    }
}

// Zapisanie konfiguracji
void saveConfiguration() {
    saveMQTTConfig();
    savePumpsConfig();
    saveNetworkConfig();
    EEPROM.put(SYSTEM_CONFIG_ADDR, systemConfig);
    EEPROM.commit();
}

// Wczytanie konfiguracji pomp
bool loadPumpsConfig() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        EEPROM.get(PUMPS_CONFIG_ADDR + (i * sizeof(PumpConfig)), pumps[i]);
    }
    return true;
}

// Zapisanie konfiguracji pomp
bool savePumpsConfig() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        EEPROM.put(PUMPS_CONFIG_ADDR + (i * sizeof(PumpConfig)), pumps[i]);
    }
    return EEPROM.commit();
}

// Wczytanie konfiguracji MQTT
void loadMQTTConfig() {
    EEPROM.get(MQTT_CONFIG_ADDR, mqttConfig);
    
    Serial.println("Wczytano konfigurację MQTT:");
    Serial.print("Broker: "); Serial.println(mqttConfig.broker);
    Serial.print("Port: "); Serial.println(mqttConfig.port);
    
    // Sprawdź czy port jest prawidłowy, jeśli nie - ustaw domyślny
    if (mqttConfig.port <= 0 || mqttConfig.port > 65535) {
        mqttConfig.port = 1883;
    }
}

// Zapisanie konfiguracji MQTT
void saveMQTTConfig() {
    EEPROM.put(MQTT_CONFIG_ADDR, mqttConfig);
    if (EEPROM.commit()) {
        Serial.println("Zapisano konfigurację MQTT:");
        Serial.print("Broker: "); Serial.println(mqttConfig.broker);
        Serial.print("Port: "); Serial.println(mqttConfig.port);
    } else {
        Serial.println("Błąd zapisu konfiguracji MQTT!");
    }
}

// Wczytanie konfiguracji sieci
bool loadNetworkConfig() {
    EEPROM.get(NETWORK_CONFIG_ADDR, networkConfig);
    return true;
}

// Zapisanie konfiguracji sieci
bool saveNetworkConfig() {
    EEPROM.put(NETWORK_CONFIG_ADDR, networkConfig);
    return EEPROM.commit();
}

// Reset do ustawień fabrycznych
void resetFactorySettings() {
    // Wyczyść EEPROM
    for (int i = 0; i < 1024; i++) {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();
    
    // Ustaw wartości domyślne
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        pumps[i].enabled = false;
        pumps[i].calibration = 1.0;
        pumps[i].dose = 0.0;
        pumps[i].schedule_hour = 0;
        pumps[i].schedule_days = 0;
        pumps[i].minute = 0;
        pumps[i].lastDosing = 0;
        pumps[i].isRunning = false;
        snprintf(pumps[i].name, sizeof(pumps[i].name), "Pump %d", i+1);
    }
    
    // Domyślna konfiguracja MQTT
    mqttConfig.port = 1883;
    //mqttConfig.enabled = false;
    mqttConfig.broker[0] = '\0';
    mqttConfig.username[0] = '\0';
    mqttConfig.password[0] = '\0';
    
    // Domyślna konfiguracja sieci
    networkConfig.dhcp = true;
    networkConfig.mqtt_port = 1883;
    networkConfig.mqtt_server[0] = '\0';
    networkConfig.mqtt_user[0] = '\0';
    networkConfig.mqtt_password[0] = '\0';
    
    // Zapisz wszystkie konfiguracje
    saveConfiguration();
}

// --- Funkcje obsługi pomp
// Uruchomienie pompy
void startPump(uint8_t pumpIndex) {
    if (!pumpInitialized || pumpIndex >= NUMBER_OF_PUMPS || pumpRunning[pumpIndex]) {
        return;
    }

    // Oblicz czas dozowania na podstawie kalibracji i dawki
    float dosingTime = (pumps[pumpIndex].dose / pumps[pumpIndex].calibration) * 1000; // konwersja na ms
    
    // Sprawdź czy czas dozowania mieści się w limitach
    if (dosingTime < MIN_DOSE_TIME || dosingTime > MAX_DOSE_TIME) {
        Serial.printf("Błąd: Nieprawidłowy czas dozowania dla pompy %d: %.1f ms\n", pumpIndex + 1, dosingTime);
        return;
    }

    // Włącz pompę
    pcf8574.digitalWrite(pumpIndex, HIGH);
    pumpRunning[pumpIndex] = true;
    doseStartTime[pumpIndex] = millis();
    
    // Aktualizuj stan LED
    strip.setPixelColor(pumpIndex, COLOR_WORKING);
    strip.show();
    
    Serial.printf("Pompa %d rozpoczęła dozowanie. Zaplanowany czas: %.1f ms\n", 
                 pumpIndex + 1, dosingTime);
}

// Zatrzymanie pompy
void stopPump(uint8_t pumpIndex) {
    if (!pumpInitialized || pumpIndex >= NUMBER_OF_PUMPS || !pumpRunning[pumpIndex]) {
        return;
    }

    // Wyłącz pompę
    pcf8574.digitalWrite(pumpIndex, LOW);
    pumpRunning[pumpIndex] = false;
    
    // Oblicz faktyczny czas dozowania
    unsigned long actualDoseTime = millis() - doseStartTime[pumpIndex];
    
    // Aktualizuj stan LED
    strip.setPixelColor(pumpIndex, pumps[pumpIndex].enabled ? COLOR_ON : COLOR_OFF);
    strip.show();
    
    Serial.printf("Pompa %d zakończyła dozowanie. Rzeczywisty czas: %lu ms\n", 
                 pumpIndex + 1, actualDoseTime);
}

// Zatrzymanie wszystkich pomp
// void stopAllPumps() {
//     for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
//         if (pumpRunning[i]) {
//             stopPump(i);
//         }
//     }
// }

void stopAllPumps() {
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        pumpRunning[i] = false;
        pcf8574.digitalWrite(i, HIGH);
    }
}

// Obsługa pomp
void handlePumps() {
    if (!pumpInitialized || serviceMode) {
        return;
    }

    unsigned long currentMillis = millis();
    
    // Sprawdzaj stan pomp co DOSE_CHECK_INTERVAL
    if (currentMillis - lastDoseCheck >= DOSE_CHECK_INTERVAL) {
        lastDoseCheck = currentMillis;
        
        for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
            if (pumpRunning[i]) {
                // Sprawdź czy czas dozowania minął
                float dosingTime = (pumps[i].dose / pumps[i].calibration) * 1000;
                if (currentMillis - doseStartTime[i] >= dosingTime) {
                    stopPump(i);
                }
            } else if (shouldStartDosing(i)) {
                startPump(i);
            }
        }
    }
}

// Sprawdzenie warunku dozowania
bool shouldStartDosing(uint8_t pumpIndex) {
    if (!pumps[pumpIndex].enabled) {
        return false;
    }

    DateTime now = rtc.now();
    uint8_t currentHour = now.hour();
    uint8_t currentDay = now.dayOfTheWeek(); // 0 = Sunday, 6 = Saturday

    // Sprawdź czy jest odpowiednia godzina
    if (currentHour != pumps[pumpIndex].schedule_hour) {
        return false;
    }

    // Sprawdź czy jest odpowiedni dzień
    if (!(pumps[pumpIndex].schedule_days & (1 << currentDay))) {
        return false;
    }

    // Sprawdź czy już nie dozowano dzisiaj
    unsigned long currentTime = now.unixtime();
    if (currentTime - pumps[pumpIndex].lastDosing < 24*60*60) {
        return false;
    }

    return true;
}

// Obsługa trybu serwisowego
void servicePump(uint8_t pumpIndex, bool state) {
    if (!pumpInitialized || pumpIndex >= NUMBER_OF_PUMPS || !serviceMode) {
        return;
    }

    // Ustaw stan pompy
    pcf8574.digitalWrite(pumpIndex, state ? HIGH : LOW);
    
    // Aktualizuj wyświetlanie
    strip.show();
}

// Przełączenie trybu serwisowego
void toggleServiceMode() {
    serviceMode = !serviceMode;
    if (serviceMode) {
        // Zatrzymaj wszystkie pompy w trybie serwisowym
        for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
            stopPump(i);
        }
    }
}

// --- Funkcje obsługi LED
// Aktualizacja wszystkich LED
void updateLEDs() {
    unsigned long currentMillis = millis();
    
    // Aktualizuj tylko co LED_UPDATE_INTERVAL
    if (currentMillis - lastLedUpdate < LED_UPDATE_INTERVAL) {
        return;
    }
    lastLedUpdate = currentMillis;
    
    // Aktualizacja każdego LED
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        LEDState &state = ledStates[i];
        
        // Obsługa pulsowania
        if (state.pulsing) {
            state.brightness += state.pulseDirection * 5;
            
            if (state.brightness >= PULSE_MAX_BRIGHTNESS) {
                state.brightness = PULSE_MAX_BRIGHTNESS;
                state.pulseDirection = -1;
            } else if (state.brightness <= PULSE_MIN_BRIGHTNESS) {
                state.brightness = PULSE_MIN_BRIGHTNESS;
                state.pulseDirection = 1;
            }
        } else {
            state.brightness = 255;
        }
        
        // Płynne przejście do docelowego koloru
        if (state.currentColor != state.targetColor) {
            state.currentColor = interpolateColor(
                state.currentColor, 
                state.targetColor, 
                0.1 // współczynnik płynności przejścia
            );
        }
        
        // Zastosowanie jasności do koloru
        uint8_t r, g, b;
        colorToRGB(state.currentColor, r, g, b);
        float brightnessRatio = state.brightness / 255.0;
        
        strip.setPixelColor(i, 
            (uint8_t)(r * brightnessRatio),
            (uint8_t)(g * brightnessRatio),
            (uint8_t)(b * brightnessRatio)
        );
    }
    
    strip.show();
}

// Aktualizacja LED w trybie serwisowym
void updateServiceModeLEDs() {
    uint32_t color = serviceMode ? COLOR_SERVICE : COLOR_OFF;
    
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (!pumpRunning[i]) {
            setLEDColor(i, color, serviceMode);
        }
    }
}

// Aktualizacja LED pojedynczej pompy
void updatePumpLED(uint8_t pumpIndex) {
    if (pumpIndex >= NUMBER_OF_PUMPS) return;
    
    if (serviceMode) {
        setLEDColor(pumpIndex, COLOR_SERVICE, true);
    } else if (pumpRunning[pumpIndex]) {
        setLEDColor(pumpIndex, COLOR_WORKING, true);
    } else {
        setLEDColor(pumpIndex, pumps[pumpIndex].enabled ? COLOR_ON : COLOR_OFF, false);
    }
}

// Aktualizacja LED wszystkich pomp
void updateAllPumpLEDs() {
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        updatePumpLED(i);
    }
}

// --- Funkcje sieciowe
// Konfiguracja WiFi
void setupWiFi() {
    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(180); // timeout po 3 minutach
    
    String apName = "AquaDoser-" + String(ESP.getChipId(), HEX);
    if (!wifiManager.autoConnect(apName.c_str())) {
        Serial.println("Nie udało się połączyć i timeout został osiągnięty");
        ESP.restart();
        delay(1000);
    }
    
    Serial.println("WiFi połączone!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
}

// Pierwsza aktualizacja stanu w Home Assistant
void firstUpdateHA() {
    // Aktualizacja stanu wszystkich pomp
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        pumpSwitches[i]->setState(pumps[i].enabled);
        pumpCalibrations[i]->setState(pumps[i].calibration);
        if (pumpStates[i].sensor) {
            pumpStates[i].sensor->setValue(pumpStates[i].isActive ? "active" : "inactive");
        }
    }
    
    // Aktualizacja trybu serwisowego
    serviceModeSwitch->setState(false);
}

// Konfiguracja MQTT
void setupMQTT() {
    // Ustawienie unikalnego ID
    byte mac[6];
    WiFi.macAddress(mac);
    device.setUniqueId(mac, sizeof(mac));
    device.setName("AquaDoser");
    
    // Rozpoczęcie połączenia MQTT
    bool result = mqtt.begin(mqttConfig.broker, mqttConfig.username, mqttConfig.password);
    
    if (result) {
        Serial.println(F("MQTT connected"));
    } else {
        Serial.println(F("MQTT connection failed"));
    }
}

// Konfiguracja serwera WWW
void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    //server.on("/save", HTTP_POST, handleSave);
    server.on("/save", HTTP_POST, handleConfigSave);
    server.on("/update", HTTP_POST, handleUpdateResult, handleDoUpdate);
    // server.on("/update", HTTP_GET, []() {
    //     server.sendHeader("Connection", "close");
    //     server.send(200, "text/html", getUpdatePage());
    // });
    
    httpUpdateServer.setup(&server);
    server.begin();
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
}

// Sprawdzenie konfiguracji MQTT
void checkMQTTConfig() {
    Serial.print(F("MQTT Broker: "));
    Serial.println(networkConfig.mqtt_server);
    Serial.print(F("MQTT Port: "));
    Serial.println(networkConfig.mqtt_port);
}

// Aktualizacja Home Assistant
void updateHomeAssistant() {
    DateTime now = rtc.now();
    char currentTimeStr[32];
    
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (!pumpStates[i].isActive && pumps[i].enabled) {
            char statusBuffer[128];
            
            if (pumpStates[i].isActive) {
                // Format: "Active (Started: 2024-11-27 20:43)"
                snprintf(statusBuffer, sizeof(statusBuffer), 
                    "Active (Started: %04d-%02d-%02d %02d:%02d)", 
                    now.year(), now.month(), now.day(), 
                    now.hour(), now.minute());
            } else {
                if (pumpStates[i].lastDoseTime > 0) {
                    // Konwertujemy unix timestamp na czytelną datę
                    time_t rawTime = pumpStates[i].lastDoseTime;
                    struct tm* timeInfo = localtime(&rawTime);
                    
                    // Format: "Inactive (Last: 2024-11-27 20:43)"
                    snprintf(statusBuffer, sizeof(statusBuffer), 
                        "Inactive (Last: %04d-%02d-%02d %02d:%02d)",
                        timeInfo->tm_year + 1900, timeInfo->tm_mon + 1, timeInfo->tm_mday,
                        timeInfo->tm_hour, timeInfo->tm_min);
                } else {
                    strcpy(statusBuffer, "Inactive (No previous runs)");
                }
            }
            
            // Dodaj informację o następnym zaplanowanym dozowaniu
            if (!pumpStates[i].isActive && pumps[i].enabled) {
                char nextRunBuffer[64];
                DateTime nextRun = calculateNextDosing(i);
                snprintf(nextRunBuffer, sizeof(nextRunBuffer), 
                    " - Next: %02d:%02d",
                    nextRun.hour(), nextRun.minute());
                strcat(statusBuffer, nextRunBuffer);
            }
            
            pumpStates[i].sensor->setValue(statusBuffer);

            // Debug info
            Serial.print("Pump ");
            Serial.print(i + 1);
            Serial.print(": ");
            Serial.println(statusBuffer);
        }
    }
}

// --- Funkcje obsługi zdarzeń
// Obsługa przycisku
void handleButton() {
    bool currentButtonState = digitalRead(BUTTON_PIN);
    
    // Debouncing
    if (currentButtonState != lastButtonState) {
        if (millis() - lastButtonPress >= DEBOUNCE_TIME) {
            lastButtonPress = millis();
            
            // Reaguj tylko na naciśnięcie (zmiana z HIGH na LOW)
            if (currentButtonState == LOW) {
                toggleServiceMode();
            }
        }
    }
    
    lastButtonState = currentButtonState;
}

// Obsługa strony głównej
void handleRoot() {
    server.send(200, "text/html", getConfigPage());
}

// Obsługa zapisu konfiguracji
void handleSave() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    if (server.hasArg("plain")) {
        String json = server.arg("plain");
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, json);
        
        if (!error) {
            bool configChanged = false;
            
            // Obsługa konfiguracji MQTT
            if (doc.containsKey("mqtt")) {
                JsonObject mqtt = doc["mqtt"];
                if (mqtt.containsKey("broker")) {
                    strlcpy(mqttConfig.broker, mqtt["broker"] | "", sizeof(mqttConfig.broker));
                    configChanged = true;
                }
                if (mqtt.containsKey("port")) {
                    mqttConfig.port = mqtt["port"] | 1883;
                    configChanged = true;
                }
                if (mqtt.containsKey("username")) {
                    strlcpy(mqttConfig.username, mqtt["username"] | "", sizeof(mqttConfig.username));
                    configChanged = true;
                }
                if (mqtt.containsKey("password")) {
                    strlcpy(mqttConfig.password, mqtt["password"] | "", sizeof(mqttConfig.password));
                    configChanged = true;
                }
            }

            // Obsługa konfiguracji pomp
            if (doc.containsKey("pumps")) {
                JsonArray pumpsArray = doc["pumps"];
                for (uint8_t i = 0; i < NUMBER_OF_PUMPS && i < pumpsArray.size(); i++) {
                    JsonObject pump = pumpsArray[i];
                    
                    if (pump.containsKey("name")) {
                        strlcpy(pumps[i].name, pump["name"] | "", sizeof(pumps[i].name));
                        configChanged = true;
                    }
                    if (pump.containsKey("enabled")) {
                        pumps[i].enabled = pump["enabled"] | false;
                        configChanged = true;
                    }
                    if (pump.containsKey("calibration")) {
                        pumps[i].calibration = pump["calibration"] | 1.0f;
                        configChanged = true;
                    }
                    if (pump.containsKey("dose")) {
                        pumps[i].dose = pump["dose"] | 0.0f;
                        configChanged = true;
                    }
                    if (pump.containsKey("schedule_hour")) {
                        pumps[i].schedule_hour = pump["schedule_hour"] | 0;
                        configChanged = true;
                    }
                    if (pump.containsKey("minute")) {
                        pumps[i].minute = pump["minute"] | 0;
                        configChanged = true;
                    }
                    if (pump.containsKey("schedule_days")) {
                        pumps[i].schedule_days = pump["schedule_days"] | 0;
                        configChanged = true;
                    }
                }
            }

            if (configChanged) {
                saveConfig();
                
                DynamicJsonDocument response(256);
                response["status"] = "success";
                response["message"] = "Configuration saved";
                String responseJson;
                serializeJson(response, responseJson);
                server.send(200, "application/json", responseJson);
            }
        } else {
            server.send(400, "text/plain", "Invalid JSON");
        }
    } else {
        server.send(400, "text/plain", "No data");
    }
}

// Obsługa zapisu konfiguracji
void handleConfigSave() {
    if (server.hasArg("mqtt_broker")) {
        String broker = server.arg("mqtt_broker");
        broker.trim();
        
        if (broker.length() > 0) {
            strncpy(mqttConfig.broker, broker.c_str(), sizeof(mqttConfig.broker) - 1);
            mqttConfig.broker[sizeof(mqttConfig.broker) - 1] = '\0';
        }
    }
    
    if (server.hasArg("mqtt_port")) {
        String portStr = server.arg("mqtt_port");
        int port = portStr.toInt();
        if (port > 0 && port <= 65535) {
            mqttConfig.port = port;
        }
    }
    
    // Zapisz konfigurację
    saveMQTTConfig();
    
    // Przygotuj odpowiedź w formacie JSON
    String jsonResponse = "{\"status\":\"success\",\"message\":\"";
    jsonResponse += "Zapisano konfigurację MQTT:\\nBroker: ";
    jsonResponse += mqttConfig.broker;
    jsonResponse += "\\nPort: ";
    jsonResponse += String(mqttConfig.port);
    jsonResponse += "\"}";
    
    Serial.println(jsonResponse);
    server.send(200, "application/json", jsonResponse);
    
    // Zrestartuj połączenie MQTT
    if (mqtt.isConnected()) {
        mqtt.disconnect();
    }
    setupMQTT();
}

// Obsługa zdarzeń WebSocket
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    // Na razie obsługujemy tylko połączenie - tak jak w HydroSense
    switch(type) {
        case WStype_DISCONNECTED:
            break;
        case WStype_CONNECTED:
            break;
    }
}

// Obsługa aktualizacji firmware
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

// Obsługa wyniku aktualizacji
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

// --- Funkcje pomocnicze
// Synchronizacja czasu
void syncRTC() {
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();
    
    if (epochTime > 1600000000) { // sprawdź czy czas jest sensowny (po 2020 roku)
        DateTime newTime(epochTime);
        rtc.adjust(newTime);
        
        Serial.println("RTC zsynchronizowany z NTP:");
        DateTime now = rtc.now();
        Serial.printf("%04d-%02d-%02d %02d:%02d:%02d\n",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute(), now.second());
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

// Melodia powitalna
void welcomeMelody() {
    if (!systemConfig.soundEnabled) return;
    tone(BUZZER_PIN, 1000, 100);
    delay(150);
    tone(BUZZER_PIN, 1500, 100);
    delay(150);
    tone(BUZZER_PIN, 2000, 100);
}

// Krótki sygnał ostrzegawczy
void playShortWarningSound() {
    if (!systemConfig.soundEnabled) return;
    tone(BUZZER_PIN, 2000, 100);
}

// Sygnał potwierdzenia
void playConfirmationSound() {
    if (!systemConfig.soundEnabled) return;
    tone(BUZZER_PIN, 1000, 50);
    delay(100);
    tone(BUZZER_PIN, 2000, 50);
}

// Walidacja konfiguracji
bool validateConfigValues() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumps[i].calibration <= 0 || pumps[i].calibration > 10)
            return false;
        if (pumps[i].dose < 0 || pumps[i].dose > 1000)
            return false;
    }
    return true;
}

// Walidacja konfiguracji MQTT
bool validateMQTTConfig() {
    if (strlen(networkConfig.mqtt_server) == 0) {
        return false;
    }
    if (networkConfig.mqtt_port <= 0 || networkConfig.mqtt_port > 65535) {
        return false;
    }
    return true;
}

// Aktualizacja statusu systemu
void updateSystemStatus() {
    systemStatus.uptime = millis() / 1000;
    systemStatus.wifi_connected = WiFi.status() == WL_CONNECTED;
    systemStatus.mqtt_connected = mqtt.isConnected();
}

// Obsługa przepełnienia millis()
void handleMillisOverflow() {
    static unsigned long lastMillis = 0;
    unsigned long currentMillis = millis();
    
    if (currentMillis < lastMillis) {
        // Przepełnienie - zresetuj timery
        lastMQTTLoop = 0;
        lastMeasurement = 0;
        lastOTACheck = 0;
    }
    
    lastMillis = currentMillis;
}

// Formatowanie czasu
String formatDateTime(const DateTime& dt) {
    char buffer[20];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
             dt.year(), dt.month(), dt.day(),
             dt.hour(), dt.minute(), dt.second());
    return String(buffer);
}

// Pobranie statusu w formacie JSON
String getSystemStatusJSON() {
    StaticJsonDocument<512> doc;
    
    doc["mqtt_connected"] = mqtt.isConnected(); // Użyj faktycznego stanu połączenia
    doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
    doc["uptime"] = millis() / 1000; // Czas w sekundach
    
    String output;
    serializeJson(doc, output);
    return output;
}

// Pobranie strony aktualizacji
String getUpdatePage() {
    String page = F("<!DOCTYPE html><html><head><title>AquaDoser Update</title></head><body>");
    page += F("<h1>AquaDoser - Aktualizacja</h1>");
    page += F("<form method='POST' action='/update' enctype='multipart/form-data'>");
    page += F("<input type='file' name='update'>");
    page += F("<input type='submit' value='Aktualizuj'>");
    page += F("</form>");
    page += F("</body></html>");
    return page;
}

// Pobranie strony konfiguracji
String getConfigPage() {
    String page = F("<!DOCTYPE html><html lang='pl'><head>");
    page += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    page += F("<title>AquaDoser</title><style>");
    page += getStyles();
    page += F("</style>");
    
    // JavaScript with WebSocket and functions
    page += F("<script>");
    // WebSocket setup
    page += F("var ws = new WebSocket('ws://' + window.location.hostname + ':81/');");
    page += F("ws.onmessage = function(evt) {");
    page += F("  var msg = evt.data;");
    page += F("  if(msg.startsWith('update:')) {");
    page += F("    var status = msg.substring(7);");
    page += F("    if(status === 'error') {");
    page += F("      document.getElementById('prg').style.display = 'none';");
    page += F("      alert('Błąd aktualizacji!');");
    page += F("    } else if(!isNaN(status)) {");
    page += F("      var progress = parseInt(status);");
    page += F("      document.getElementById('prg').style.display = 'block';");
    page += F("      document.getElementById('progress').style.width = progress + '%';");
    page += F("      document.getElementById('progress-text').innerHTML = progress + '%';");
    page += F("      if(progress === 100) {");
    page += F("        alert('Aktualizacja zakończona. Urządzenie zostanie zrestartowane.');");
    page += F("      }");
    page += F("    }");
    page += F("  }");
    page += F("};");
    
    // Obsługa formularza aktualizacji
    page += F("document.getElementById('upload_form').onsubmit = function(e) {");
    page += F("  e.preventDefault();");
    page += F("  var file = document.getElementById('file').files[0];");
    page += F("  if(!file) {");
    page += F("    alert('Wybierz plik!');");
    page += F("    return false;");
    page += F("  }");
    page += F("  document.getElementById('prg').style.display = 'block';");
    page += F("  var xhr = new XMLHttpRequest();");
    page += F("  xhr.open('POST', '/update', true);");
    page += F("  xhr.upload.onprogress = function(e) {");
    page += F("    if(e.lengthComputable) {");
    page += F("      var progress = (e.loaded * 100) / e.total;");
    page += F("      document.getElementById('progress').style.width = progress + '%';");
    page += F("      document.getElementById('progress-text').innerHTML = progress + '%';");
    page += F("    }");
    page += F("  };");
    page += F("  xhr.onload = function() {");
    page += F("    if(xhr.status === 200) {");
    page += F("      alert('Aktualizacja zakończona pomyślnie!');");
    page += F("    } else {");
    page += F("      alert('Błąd podczas aktualizacji.');");
    page += F("    }");
    page += F("  };");
    page += F("  var formData = new FormData();");
    page += F("  formData.append('update', file);");
    page += F("  xhr.send(formData);");
    page += F("  return false;");
    page += F("};");
    
    // Status update function
    page += F("function updateStatus(status) {");
    page += F("    document.getElementById('mqtt-status').className = 'status ' + (status.mqtt_connected ? 'success' : 'error');");
    page += F("    document.getElementById('mqtt-status').innerText = status.mqtt_connected ? 'Połączony' : 'Rozłączony';");
    page += F("    document.getElementById('wifi-status').className = 'status ' + (status.wifi_connected ? 'success' : 'error');");
    page += F("    document.getElementById('wifi-status').innerText = status.wifi_connected ? 'Połączony' : 'Rozłączony';");
    page += F("    document.getElementById('current-time').innerText = status.current_time;");
    page += F("}");
    
    // Uptime formatting function
    page += F("function formatUptime(seconds) {");
    page += F("    let days = Math.floor(seconds / 86400);");
    page += F("    let hours = Math.floor((seconds % 86400) / 3600);");
    page += F("    let minutes = Math.floor((seconds % 3600) / 60);");
    page += F("    let parts = [];");
    page += F("    if(days > 0) parts.push(days + 'd');");
    page += F("    if(hours > 0) parts.push(hours + 'h');");
    page += F("    parts.push(minutes + 'm');");
    page += F("    return parts.join(' ');");
    page += F("}");

    // Configuration save function
    page += F("function saveConfiguration() {");
    page += F("    const form = document.getElementById('configForm');");
    page += F("    if (!form.checkValidity()) {");
    page += F("        alert('Proszę wypełnić wszystkie wymagane pola poprawnie.');");
    page += F("        return false;");
    page += F("    }");
    
    page += F("    let pumpsData = [];");
    page += F("    for(let i = 0; i < ");
    page += String(NUMBER_OF_PUMPS);
    page += F("; i++) {");
    page += F("        let pumpData = {");
    page += F("            name: document.querySelector(`input[name='pump_name_${i}']`).value.trim() || `Pompa ${i + 1}`,");
    page += F("            enabled: document.querySelector(`input[name='pump_enabled_${i}']`).checked,");
    page += F("            calibration: Math.max(0.01, parseFloat(document.querySelector(`input[name='pump_calibration_${i}']`).value)),");
    page += F("            dose: Math.max(0, parseFloat(document.querySelector(`input[name='pump_dose_${i}']`).value)),");
    page += F("            schedule_hour: Math.min(23, Math.max(0, parseInt(document.querySelector(`input[name='pump_schedule_hour_${i}']`).value))),");
    page += F("            minute: Math.min(59, Math.max(0, parseInt(document.querySelector(`input[name='pump_minute_${i}']`).value))),");
    page += F("            schedule_days: 0");
    page += F("        };");
    page += F("        for(let day = 0; day < 7; day++) {");
    page += F("            if(document.querySelector(`input[name='pump_day_${i}_${day}']`).checked) {");
    page += F("                pumpData.schedule_days |= (1 << day);");
    page += F("            }");
    page += F("        }");
    page += F("        pumpsData.push(pumpData);");
    page += F("    }");

    page += F("    let configData = {");
    page += F("        pumps: pumpsData,");
    page += F("        mqtt: {");
    page += F("            broker: document.querySelector('input[name=\"mqtt_broker\"]').value.trim(),");
    page += F("            port: parseInt(document.querySelector('input[name=\"mqtt_port\"]').value),");
    page += F("            username: document.querySelector('input[name=\"mqtt_user\"]').value.trim(),");
    page += F("            password: document.querySelector('input[name=\"mqtt_pass\"]').value");
    page += F("        }");
    page += F("    };");

    page += F("    fetch('/save', {");
    page += F("        method: 'POST',");
    page += F("        headers: { 'Content-Type': 'application/json' },");
    page += F("        body: JSON.stringify(configData)");
    page += F("    })");
    page += F("    .then(response => response.json())");
    page += F("    .then(data => {");
    page += F("        if(data.status === 'success') {");
    page += F("            alert('Konfiguracja została zapisana pomyślnie');");
    page += F("            window.location.reload();");
    page += F("        } else {");
    page += F("            throw new Error(data.message || 'Nieznany błąd');");
    page += F("        }");
    page += F("    })");
    page += F("    .catch(error => {");
    page += F("        alert('Błąd podczas zapisywania konfiguracji: ' + error.message);");
    page += F("        console.error('Error:', error);");
    page += F("    });");
    page += F("    return false;");
    page += F("}");
    page += F("</script></head><body>");
    
    page += F("<div class='container'>");
    page += F("<h1>AquaDoser</h1>");

    // Status systemu
    page += F("<div class='section'>");
    page += F("<h2>Status systemu</h2>");
    page += F("<table class='config-table'>");
    
    // Status MQTT
    page += F("<tr><td>Status MQTT</td><td><span id='mqtt-status' class='status ");
    page += (systemStatus.mqtt_connected ? F("success'>Połączony") : F("error'>Rozłączony"));
    page += F("</span></td></tr>");
    
    // Status WiFi
    page += F("<tr><td>Status WiFi</td><td><span id='wifi-status' class='status ");
    page += (WiFi.status() == WL_CONNECTED ? F("success'>Połączony") : F("error'>Rozłączony"));
    page += F("</span></td></tr>");
    
    // Aktualna data i czas
    DateTime now = rtc.now();
    page += F("<tr><td>Data i czas</td><td><span id='current-time'>");
    page += (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
            (now.minute() < 10 ? "0" : "") + String(now.minute()) + " " +
            (now.day() < 10 ? "0" : "") + String(now.day()) + "/" +
            (now.month() < 10 ? "0" : "") + String(now.month()) + "/" +
            String(now.year());
    page += F("</span></td></tr>");
    page += F("</table></div>");
// Przyciski systemowe po sekcji statusu
    page += F("<div class='section'>");
    page += F("<div class='buttons-container'>");
    page += F("<button type='button' onclick='if(confirm(\"Czy na pewno chcesz zrestartować urządzenie?\")) { fetch(\"/reboot\"); }' class='btn btn-blue'>Restart urządzenia</button>");
    page += F("<button type='button' onclick='if(confirm(\"Czy na pewno chcesz przywrócić ustawienia fabryczne? Wszystkie ustawienia zostaną usunięte!\")) { fetch(\"/factory-reset\"); }' class='btn btn-red'>Ustawienia fabryczne</button>");
    page += F("</div>");
    page += F("</div>");

    // Formularz konfiguracji
    page += F("<form id='configForm' onsubmit='return saveConfiguration()'>");
    
    // Konfiguracja MQTT
    page += F("<div class='section'>");
    page += F("<h2>Konfiguracja MQTT</h2>");
    page += F("<table class='config-table'>");
    page += F("<tr><td>Broker MQTT</td><td><input type='text' name='mqtt_broker' value='");
    page += mqttConfig.broker;
    page += F("' required></td></tr>");
    page += F("<tr><td>Port MQTT</td><td><input type='number' name='mqtt_port' value='");
    page += String(mqttConfig.port);
    page += F("' required min='1' max='65535'></td></tr>");
    page += F("<tr><td>Użytkownik MQTT</td><td><input type='text' name='mqtt_user' value='");
    page += mqttConfig.username;
    page += F("' required></td></tr>");
    page += F("<tr><td>Hasło MQTT</td><td><input type='password' name='mqtt_pass' value='");
    page += mqttConfig.password;
    page += F("' required></td></tr>");
    page += F("</table></div>");

    // Konfiguracja pomp
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        page += F("<div class='section'>");
        page += F("<h2>Pompa "); 
        page += String(i + 1);
        page += F("</h2>");
        page += F("<table class='config-table'>");
        
        // Nazwa pompy
        page += F("<tr><td>Nazwa</td><td><input type='text' name='pump_name_");
        page += String(i);
        page += F("' value='");
        page += pumps[i].name[0] ? pumps[i].name : ("Pompa " + String(i + 1));
        page += F("' maxlength='19' required></td></tr>");

        // Status aktywności
        page += F("<tr><td>Aktywna</td><td><input type='checkbox' name='pump_enabled_");
        page += String(i);
        page += F("' ");
        page += (pumps[i].enabled ? F("checked") : F(""));
        page += F("></td></tr>");

        // Kalibracja
        page += F("<tr><td>Kalibracja (ml/s)</td><td><input type='number' step='0.01' name='pump_calibration_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].calibration > 0 ? pumps[i].calibration : 1.0);
        page += F("' min='0.01' required></td></tr>");

        // Dawka
        page += F("<tr><td>Dawka (ml)</td><td><input type='number' step='0.1' name='pump_dose_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].dose >= 0 ? pumps[i].dose : 0);
        page += F("' min='0' required></td></tr>");

        // Godzina dozowania
        page += F("<tr><td>Godzina dozowania</td><td><input type='number' name='pump_schedule_hour_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].schedule_hour < 24 ? pumps[i].schedule_hour : 0);
        page += F("' min='0' max='23' required></td></tr>");

        // Minuta dozowania
        page += F("<tr><td>Minuta dozowania</td><td><input type='number' name='pump_minute_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].minute < 60 ? pumps[i].minute : 0);
        page += F("' min='0' max='59' required></td></tr>");
        
        // Dni tygodnia - kontynuacja konfiguracji pomp
        //page += F("<tr><td>Dni dozowania</td><td class='days-container'>");
        page += F("</table>"); // zamknięcie głównej tabeli
        page += F("<div class='dosing-days'><h3>Dni dozowania</h3><div class='days-container'>");
        const char* dayNames[] = {"Pon", "Wt", "Śr", "Czw", "Pt", "Sob", "Nd"};
        for(int day = 0; day < 7; day++) {
            page += F("<label class='day-checkbox'><input type='checkbox' name='pump_day_");
            page += String(i);
            page += F("_");
            page += String(day);
            page += F("' ");
            page += ((pumps[i].schedule_days & (1 << day)) ? F("checked") : F(""));
            page += F("> ");
            page += dayNames[day];
            page += F("</label>");
        }
        page += F("</td></tr>");
        page += F("</table></div>");
        page += F("</div></div>");
        page += F("</div>"); // zamknięcie section
    }

    // Przycisk zapisz - teraz w kolorze zielonym
    page += F("<div class='section'>");
    page += F("<div class='buttons-container'>");
    page += F("<input type='submit' value='Zapisz ustawienia' class='btn btn-green'>");
    page += F("</div></div>");
    
    page += F("</form>");

    // Sekcja aktualizacji firmware
    page += F("<div class='update-section'>");
    page += F("<h2>Aktualizacja firmware</h2>");
    page += F("<form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>");
    page += F("<input type='file' name='update' id='file' accept='.bin'>");
    page += F("<input type='submit' value='Aktualizuj firmware' class='update-button'>");
    page += F("</form>");
    page += F("<div id='prg' style='display: none'>");
    page += F("<div class='progress-bar'><div class='progress' id='progress'></div></div>");
    page += F("<div class='progress-text' id='progress-text'>0%</div>");
    page += F("</div>");
    page += F("</div>");
    
    // Stopka z wersją i linkiem do GitHub
    page += F("<div class='footer'>");
    page += F("<a href='https://github.com/pimowo/AquaDoser' target='_blank'>GitHub</a>");
    page += F(" | Wersja: ");
    page += VERSION;
    page += F("</div>");
    
    page += F("</div></body></html>");
    return page;
}

// Funkcja getStyles() - style CSS
String getStyles() {
    String styles = F("");
    
    // Podstawowe style
    styles += F("* { box-sizing: border-box; margin: 0; padding: 0; }");
    styles += F("body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #1a1a1a; color: #ffffff; }");
    
    // Kontenery
    styles += F(".container { max-width: 800px; margin: 0 auto; padding: 0 15px; }");
    styles += F(".buttons-container { display: flex; justify-content: space-between; margin: -5px; }");
    styles += F(".section { background-color: #2a2a2a; padding: 20px; margin-bottom: 20px; border-radius: 8px; width: 100%; box-sizing: border-box; }");
    
    // Nagłówki
    styles += F("h1 { color: #ffffff; text-align: center; margin-bottom: 30px; font-size: 2.5em; background-color: #2d2d2d; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.2); }");
    styles += F("h2 { color: #2196F3; margin-top: 0; font-size: 1.5em; }");
    
    // Tabela konfiguracji
    styles += F(".config-table { width: 100%; border-collapse: collapse; table-layout: fixed; }");
    styles += F(".config-table td { padding: 8px; border-bottom: 1px solid #3d3d3d; }");
    styles += F(".config-table td:first-child { width: 65%; }");
    styles += F(".config-table td:last-child { width: 35%; }");
    
    // Formularze
    styles += F("input[type='text'], input[type='password'], input[type='number'] { width: 100%; padding: 8px; border: 1px solid #3d3d3d; border-radius: 4px; background-color: #1a1a1a; color: #ffffff; box-sizing: border-box; text-align: left; }");
    styles += F("input[type='checkbox'] { width: 20px; height: 20px; margin: 0; vertical-align: middle; }");
    
    // Przyciski
    styles += F(".btn { padding: 12px 24px; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; width: calc(50% - 10px); display: inline-block; margin: 5px; text-align: center; transition: background-color 0.3s; }");
    styles += F(".btn-blue { background-color: #2196F3; color: white; }");
    styles += F(".btn-blue:hover { background-color: #1976D2; }");
    styles += F(".btn-red { background-color: #DC3545; color: white; }");
    styles += F(".btn-red:hover { background-color: #C82333; }");
    styles += F(".btn-green { background-color: #28A745; color: white; width: 100% !important; }");
    styles += F(".btn-green:hover { background-color: #218838; }");
    
    // Statusy i komunikaty
    styles += F(".status { padding: 4px 8px; border-radius: 4px; display: inline-block; }");
    styles += F(".success { color: #4CAF50; }");
    styles += F(".error { color: #F44336; }");
    styles += F(".message { position: fixed; top: 20px; left: 50%; transform: translateX(-50%); padding: 15px 30px; border-radius: 5px; color: white; opacity: 0; transition: opacity 0.3s ease-in-out; z-index: 1000; }");
    
    // Sekcja dni dozowania
    styles += F(".dosing-days { margin-top: 20px; }");
    styles += F(".dosing-days h3 { margin: 0 0 10px 0; font-size: 1em; font-weight: normal; }");
    styles += F(".days-container { display: flex; flex-wrap: wrap; gap: 10px; justify-content: flex-start; }");
    
    // Sekcja aktualizacji
    styles += F(".update-section { margin-top: 30px; }");
    styles += F(".progress-bar { width: 100%; height: 24px; border: 1px solid #2196F3; border-radius: 4px; overflow: hidden; }");
    styles += F(".progress { background: #2196F3; height: 100%; width: 0%; }");
    styles += F(".progress-text { text-align: center; margin-top: 8px; }");
    styles += F(".update-button { background: #2196F3; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; margin-top: 10px; }");
    styles += F("#file { margin: 10px 0; }");
    
    // Sekcja kalibracji
    styles += F(".calibration-section { margin: 10px 0; padding: 10px; border: 1px solid #3d3d3d; border-radius: 4px; }");
    styles += F(".calib-step { margin: 5px 0; }");
    styles += F(".calib-step input { width: 60px; margin: 0 5px; }");
    styles += F(".calib-status { margin-top: 5px; font-size: 0.9em; color: #666; }");
    styles += F(".btn-small { padding: 2px 8px; margin-left: 5px; }");
    
    // Stopka
    styles += F(".footer { text-align: center; padding: 20px 0; }");
    styles += F(".footer a { display: inline-block; padding: 10px 20px; background-color: #333; color: #fff; text-decoration: none; border-radius: 4px; }");
    styles += F(".footer a:hover { background-color: #444; }");
    
    // Media Queries
    styles += F("@media (max-width: 600px) { body { padding: 10px; } .container { padding: 0; } .section { padding: 15px; margin-bottom: 15px; } .config-table td:first-child { width: 50%; } .config-table td:last-child { width: 50%; } .btn { width: 100%; margin: 5px 0; } .buttons-container { flex-direction: column; } }");
    
    return styles;
}

String getPumpInputField(uint8_t index, const char* name, const char* label, const String& value, const char* type, const char* attributes = "") {
    String html = F("<div class='field'><label for='");
    html += name;
    html += index;
    html += F("'>");
    html += label;
    html += F("</label><input type='");
    html += type;
    html += F("' id='");
    html += name;
    html += index;
    html += F("' name='pump[");
    html += index;
    html += F("][");
    html += name;
    html += F("]' value='");
    html += value;
    if (attributes && strlen(attributes) > 0) {
        html += F("' ");
        html += attributes;
    }
    html += F("'></div>");
    return html;
}

String getPumpCheckbox(uint8_t index, const char* name, const char* label, bool checked) {
    String html = F("<div class='field'><label><input type='checkbox' id='");
    html += name;
    html += index;
    html += F("' name='pump[");
    html += index;
    html += F("][");
    html += name;
    html += F("]' value='1'");
    if (checked) html += F(" checked");
    html += F("> ");
    html += label;
    html += F("</label></div>");
    return html;
}

String getScheduleDaysField(uint8_t index) {
    String html = F("<div class='field'><label>Dni dozowania:</label><div class='days'>");
    const char* days[] = {"Pn", "Wt", "Śr", "Cz", "Pt", "So", "Nd"};
    for (int i = 0; i < 7; i++) {
        html += F("<label><input type='checkbox' name='pump[");
        html += index;
        html += F("][days][");
        html += i;
        html += F("]' value='1'");
        if (pumps[index].schedule_days & (1 << i)) html += F(" checked");
        html += F("> ");
        html += days[i];
        html += F("</label>");
    }
    html += F("</div></div>");
    return html;
}

String getCalibrationSection() {
    String html = F("<div class='section'>");
    html += F("<h2>Kalibracja pomp</h2>");
    
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        html += F("<div class='pump-calibration'>");
        html += F("<h3>Pompa ") + String(i + 1) + F("</h3>");
        html += F("<p>Aktualna kalibracja: <span id='calibration") + String(i) + F("'>") + 
                String(pumps[i].calibration, 2) + F("</span> ml/min</p>");
        
        // Krok 1: Uruchomienie pompy
        html += F("<div class='step-1'>");
        html += F("<h4>Krok 1: Uruchom pompę</h4>");
        html += F("<div class='input-group'>");
        html += F("<label>Czas pracy (sek):");
        html += F("<input type='number' step='1' id='time") + String(i) + F("' value='15'></label>");
        html += F("<button onclick='startCalibration(") + String(i) + F(")'>Uruchom pompę</button>");
        html += F("</div></div>");
        
        // Krok 2: Wprowadzenie zmierzonej objętości
        html += F("<div class='step-2' id='step2_") + String(i) + F("' style='display:none;'>");
        html += F("<h4>Krok 2: Wprowadź zmierzoną objętość</h4>");
        html += F("<div class='input-group'>");
        html += F("<label>Zmierzona objętość (ml):");
        html += F("<input type='number' step='0.1' id='volume") + String(i) + F("'></label>");
        html += F("<button onclick='saveCalibration(") + String(i) + F(")'>Zapisz kalibrację</button>");
        html += F("</div></div>");
        
        html += F("<div id='result") + String(i) + F("' class='result'></div>");
        html += F("</div>");
    }
    
    html += F("</div>");
    
    // Style CSS
    html += F("<style>");
    html += F(".pump-calibration { margin-bottom: 20px; padding: 15px; border: 1px solid #ccc; border-radius: 5px; }");
    html += F(".input-group { margin-bottom: 10px; }");
    html += F(".input-group label { display: block; margin-bottom: 5px; }");
    html += F(".result { margin-top: 10px; font-weight: bold; color: #008000; }");
    html += F(".step-1, .step-2 { margin-top: 15px; }");
    html += F("button { padding: 5px 10px; margin-top: 5px; }");
    html += F("</style>");
    
    // JavaScript
    html += F("<script>");
    html += F("function startCalibration(pumpIndex) {");
    html += F("    const time = document.getElementById('time' + pumpIndex).value;");
    html += F("    if (!time || time <= 0) {");
    html += F("        alert('Wprowadź prawidłowy czas');");
    html += F("        return;");
    html += F("    }");
    html += F("    if (!confirm('Rozpocząć kalibrację pompy ' + (pumpIndex + 1) + '?\\nPompa będzie pracować przez ' + time + ' sekund.')) {");
    html += F("        return;");
    html += F("    }");
    html += F("    document.getElementById('result' + pumpIndex).innerHTML = 'Uruchamiam pompę...';");
    html += F("    fetch('/calibrate?start=1&pump=' + pumpIndex + '&time=' + time)");
    html += F("        .then(response => {");
    html += F("            if (response.ok) {");
    html += F("                document.getElementById('step2_' + pumpIndex).style.display = 'block';");
    html += F("                document.getElementById('result' + pumpIndex).innerHTML = ");
    html += F("                    'Pompa zatrzymana. Zmierz objętość płynu i wprowadź wynik.';");
    html += F("            } else {");
    html += F("                throw new Error('Błąd kalibracji');");
    html += F("            }");
    html += F("        })");
    html += F("        .catch(error => {");
    html += F("            document.getElementById('result' + pumpIndex).innerHTML = ");
    html += F("                'Błąd: ' + error.message;");
    html += F("        });");
    html += F("}");
    
    html += F("function saveCalibration(pumpIndex) {");
    html += F("    const volume = document.getElementById('volume' + pumpIndex).value;");
    html += F("    if (!volume || volume <= 0) {");
    html += F("        alert('Wprowadź zmierzoną objętość');");
    html += F("        return;");
    html += F("    }");
    html += F("    fetch('/calibrate?save=1&pump=' + pumpIndex + '&volume=' + volume)");
    html += F("        .then(response => response.json())");
    html += F("        .then(data => {");
    html += F("            document.getElementById('calibration' + pumpIndex).textContent = data.flowRate;");
    html += F("            document.getElementById('result' + pumpIndex).innerHTML = ");
    html += F("                'Kalibracja zapisana! Nowy przepływ: ' + data.flowRate + ' ml/min';");
    html += F("            document.getElementById('step2_' + pumpIndex).style.display = 'none';");
    html += F("        })");
    html += F("        .catch(error => {");
    html += F("            document.getElementById('result' + pumpIndex).innerHTML = ");
    html += F("                'Błąd zapisu kalibracji: ' + error;");
    html += F("        });");
    html += F("}");
    html += F("</script>");
    
    return html;
}

String getPumpConfigSection(uint8_t index) {
    String html = F("<div class='section pump-config'>");
    
    // Nagłówek sekcji pompy
    html += F("<h3>Pompa "); 
    html += (index + 1); 
    html += F("</h3>");
    
    // Podstawowa konfiguracja
    html += F("<div class='config-table'>");
    html += getPumpInputField(index, "name", "Nazwa", pumps[index].name, "text");
    html += getPumpCheckbox(index, "enabled", "Aktywna", pumps[index].enabled);
    html += F("</div>");
    
    // Sekcja kalibracji
    html += F("<div class='calibration-section'>");
    html += F("<h4>Kalibracja</h4>");
    html += F("<div class='current-calibration'>");
    html += F("<span>Aktualna wartość: </span>");
    html += F("<span id='calibration"); 
    html += index;
    html += F("'>");
    html += String(pumps[index].calibration, 1);
    html += F("</span>");
    html += F("<span> ml/s</span>");
    html += F("</div>");
    
    // Krok 1 kalibracji
    html += F("<div class='calib-step' id='step1_");
    html += index;
    html += F("'>");
    html += F("<label>Czas kalibracji:");
    html += F("<input type='number' id='calibTime");
    html += index;
    html += F("' min='1' max='60' value='10' class='calib-input'> sekund</label>");
    html += F("<button onclick='startCalib(");
    html += index;
    html += F(")' class='btn btn-blue btn-small'>Start</button>");
    html += F("</div>");
    
    // Krok 2 kalibracji (ukryty początkowo)
    html += F("<div class='calib-step' id='step2_");
    html += index;
    html += F("' style='display:none'>");
    html += F("<label>Zmierzona objętość:");
    html += F("<input type='number' id='measuredVol");
    html += index;
    html += F("' min='0' step='0.1' value='0.0' class='calib-input'> ml</label>");
    html += F("<button onclick='finishCalib(");
    html += index;
    html += F(")' class='btn btn-green btn-small'>Zapisz</button>");
    html += F("</div>");
    
    // Status kalibracji
    html += F("<div id='result");
    html += index;
    html += F("' class='calib-status'></div>");
    html += F("</div>");
    
    // Ustawienia dozowania
    html += F("<div class='dosing-settings'>");
    html += F("<h4>Ustawienia dozowania</h4>");
    html += getPumpInputField(index, "dose", "Dawka", String(pumps[index].dose), "number", "step='0.1' min='0'");
    html += getPumpInputField(index, "hour", "Godzina", String(pumps[index].hour), "number", "min='0' max='23'");
    html += getPumpInputField(index, "minute", "Minuta", String(pumps[index].minute), "number", "min='0' max='59'");
    html += F("</div>");
    
    // Harmonogram dni
    html += F("<div class='schedule-section'>");
    html += F("<h4>Harmonogram</h4>");
    html += getScheduleDaysField(index);
    html += F("</div>");
    
    html += F("</div>"); // Zamknięcie pump-config
    return html;
}

String getScripts() {
    String js = F("");
    
    // Podstawowe funkcje
    js += F("function updatePumpStatus(index, status) {");
    js += F("    document.getElementById('pump-status-' + index).innerHTML = status;");
    js += F("}");
    
    // Funkcje kalibracji
    js += F("function startCalibration(index) {");
    js += F("    fetch('/calibrate?pump=' + index)");
    js += F("        .then(response => response.text())");
    js += F("        .then(status => {");
    js += F("            document.getElementById('calib-status-' + index).innerHTML = status;");
    js += F("        });");
    js += F("}");

    // Dodaj funkcje obsługi formularza MQTT
    js += F("function saveMQTTConfig() {");
    js += F("    const broker = document.getElementById('mqtt_broker').value;");
    js += F("    const port = document.getElementById('mqtt_port').value;");
    js += F("    const data = new FormData();");
    js += F("    data.append('mqtt_broker', broker);");
    js += F("    data.append('mqtt_port', port);");
    js += F("    fetch('/save_config', {");
    js += F("        method: 'POST',");
    js += F("        body: data");
    js += F("    })");
    js += F("    .then(response => response.json())");
    js += F("    .then(data => {");
    js += F("        if (data.status === 'success') {");
    js += F("            alert(data.message);");
    js += F("        } else {");
    js += F("            alert('Błąd podczas zapisywania konfiguracji: ' + data.message);");
    js += F("        }");
    js += F("    })");
    js += F("    .catch(error => {");
    js += F("        alert('Błąd podczas zapisywania konfiguracji: ' + error);");
    js += F("    });");
    js += F("    return false;");
    js += F("}");

    return js;
}

// Obsługa żądań HTTP dla kalibracji
void handleCalibration() {
    if (!server.hasArg("pump")) {
        server.send(400, F("application/json"), F("{\"error\":\"Missing pump index\"}"));
        return;
    }
    
    int pumpIndex = server.arg("pump").toInt();
    if (pumpIndex < 0 || pumpIndex >= NUMBER_OF_PUMPS) {
        server.send(400, F("application/json"), F("{\"error\":\"Invalid pump index\"}"));
        return;
    }
    
    if (server.hasArg("start")) {
        // Rozpoczęcie kalibracji
        int seconds = server.arg("time").toInt();
        if (seconds < 1 || seconds > 60) {
            server.send(400, F("application/json"), F("{\"error\":\"Invalid time\"}"));
            return;
        }
        startCalibration(pumpIndex, seconds);
        server.send(200, F("application/json"), F("{\"status\":\"started\"}"));
    }
    else if (server.hasArg("finish")) {
        // Zakończenie kalibracji
        if (!pumpStates[pumpIndex].calibrationCompleted) {
            server.send(400, F("application/json"), F("{\"error\":\"Calibration not completed\"}"));
            return;
        }
        
        float volume = server.arg("volume").toFloat();
        if (volume <= 0) {
            server.send(400, F("application/json"), F("{\"error\":\"Invalid volume\"}"));
            return;
        }
        
        float flowRate = finishCalibration(pumpIndex, volume);
        String response = "{\"flowRate\":" + String(flowRate, 1) + "}";
        server.send(200, F("application/json"), response);
    }
}

// Rozpoczęcie procesu kalibracji
void startCalibration(uint8_t pumpIndex, int seconds) {
    // Zatrzymaj wszystkie pompy
    stopAllPumps();
    
    // Ustaw stan kalibracji
    pumpStates[pumpIndex].isCalibrating = true;
    pumpStates[pumpIndex].calibrationDuration = seconds * 1000UL; // konwersja na ms
    pumpStates[pumpIndex].calibrationStartTime = millis();
    pumpStates[pumpIndex].calibrationCompleted = false;
    
    // Uruchom pompę
    pumpRunning[pumpIndex] = true;
    pcf8574.digitalWrite(pumpIndex, LOW);
    
    // Ustaw fioletowy kolor LED
    setLEDColor(pumpIndex, COLOR_RAINBOW_6);
}

// Zakończenie kalibracji i obliczenie wydajności
float finishCalibration(uint8_t pumpIndex, float measuredVolume) {
    // Oblicz wydajność (ml/s)
    float seconds = pumpStates[pumpIndex].calibrationDuration / 1000.0;
    float flowRate = measuredVolume / seconds;
    
    // Zapisz nową kalibrację
    pumps[pumpIndex].calibration = flowRate;
    savePumpsConfig();
    
    // Zresetuj stan kalibracji
    pumpStates[pumpIndex].isCalibrating = false;
    pumpStates[pumpIndex].calibrationCompleted = false;
    
    // Przywróć normalny kolor LED
    updatePumpLED(pumpIndex);
    
    return flowRate;
}

// Sprawdzanie stanu kalibracji (dodaj do loop())
void checkCalibration() {
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumpStates[i].isCalibrating && !pumpStates[i].calibrationCompleted) {
            unsigned long currentTime = millis();
            unsigned long elapsedTime = currentTime - pumpStates[i].calibrationStartTime;
            
            // Sprawdź czy minął zadany czas
            if (elapsedTime >= pumpStates[i].calibrationDuration) {
                // Zatrzymaj pompę
                pumpRunning[i] = false;
                pcf8574.digitalWrite(i, HIGH);
                pumpStates[i].calibrationCompleted = true;
                
                // Ustaw LED na pulsujący fioletowy
                ledStates[i].pulsing = true;
                ledStates[i].currentColor = COLOR_RAINBOW_6;
            }
        }
    }
}

// --- Funkcje callback Home Assistant
void onPumpSwitch(bool state, HASwitch* sender) {
    String name = sender->getName();
    int pumpIndex = name.substring(name.length() - 1).toInt() - 1;
    
    if (pumpIndex >= 0 && pumpIndex < NUMBER_OF_PUMPS) {
        pumps[pumpIndex].enabled = state;
        savePumpsConfig();
        updatePumpLED(pumpIndex);
    }
}

void onPumpCalibration(HANumeric value, HANumber* sender) {
    String name = sender->getName();
    int pumpIndex = name.substring(name.length() - 1).toInt() - 1;
    
    if (pumpIndex >= 0 && pumpIndex < NUMBER_OF_PUMPS) {
        pumps[pumpIndex].calibration = value.toFloat();
        savePumpsConfig();
    }
}

void onServiceModeSwitch(bool state, HASwitch* sender) {
    if (state) {
        stopAllPumps();
    }
    toggleServiceMode();
    updateServiceModeLEDs();
}

// ========== FUNKCJE LED ==========
uint32_t interpolateColor(uint32_t color1, uint32_t color2, float ratio) {
    uint8_t r1 = (color1 >> 16) & 0xFF;
    uint8_t g1 = (color1 >> 8) & 0xFF;
    uint8_t b1 = color1 & 0xFF;
    
    uint8_t r2 = (color2 >> 16) & 0xFF;
    uint8_t g2 = (color2 >> 8) & 0xFF;
    uint8_t b2 = color2 & 0xFF;
    
    uint8_t r = r1 + ((r2 - r1) * ratio);
    uint8_t g = g1 + ((g2 - g1) * ratio);
    uint8_t b = b1 + ((b2 - b1) * ratio);
    
    return (r << 16) | (g << 8) | b;
}

void colorToRGB(uint32_t color, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

void setLEDColor(uint8_t index, uint32_t color, bool immediate) {
    if (index >= NUMBER_OF_PUMPS) return;
    
    LEDState &state = ledStates[index];
    if (immediate) {
        state.currentColor = color;
        state.targetColor = color;
    } else {
        state.targetColor = color;
    }
}

// ========== FUNKCJE POMOCNICZE ==========
void saveConfig() {
    saveNetworkConfig();
    saveMQTTConfig();
    savePumpsConfig();
    saveConfiguration();
}

DateTime calculateNextDosing(uint8_t pumpIndex) {
    if (pumpIndex >= NUMBER_OF_PUMPS) return DateTime();
    
    DateTime now = rtc.now();
    DateTime nextRun = DateTime(
        now.year(),
        now.month(),
        now.day(),
        pumps[pumpIndex].hour,    // Zmienione z dosingHour
        pumps[pumpIndex].minute,  // Zmienione z dosingMinute
        0
    );
    
    if (nextRun < now) {
        nextRun = nextRun + TimeSpan(1, 0, 0, 0);
    }
    
    return nextRun;
}

void setupPumpEntities(uint8_t index) {
    char name[32];
    char uniqueId[32];
    
    sprintf(name, "Pompa %d", index + 1);
    sprintf(uniqueId, "pump_%d", index + 1);
    
    HASwitch* pumpSwitch = new HASwitch(uniqueId);  // Usunięto drugi parametr
    pumpSwitch->setName(name);
    pumpSwitch->onCommand(onPumpSwitch);
    pumpSwitch->setIcon("mdi:water-pump");
}

void setupServiceModeSwitch() {
    HASwitch* serviceSwitch = new HASwitch("service_mode");  // Usunięto drugi parametr
    serviceSwitch->setName("Tryb serwisowy");
    serviceSwitch->onCommand(onServiceModeSwitch);
    serviceSwitch->setIcon("mdi:tools");
}

void printLogHeader() {
    Serial.println(F("Status pomp:"));
    Serial.println(F("Idx\tStan\tKalibracja\tOstatnie dozowanie"));
}

void printPumpStatus(uint8_t index) {
    Serial.print(index);
    Serial.print(F("\t"));
    Serial.print(pumps[index].enabled ? F("Wł") : F("Wył"));
    Serial.print(F("\t"));
    Serial.print(pumps[index].calibration);
    Serial.print(F(" ml/min\t"));
    
    if (pumps[index].lastDosing > 0) {
        DateTime lastDose(pumps[index].lastDosing);
        char dateStr[20];
        sprintf(dateStr, "%02d:%02d %02d/%02d", 
                lastDose.hour(), lastDose.minute(),
                lastDose.day(), lastDose.month());
        Serial.print(dateStr);
    } else {
        Serial.print(F("Nigdy"));
    }
    Serial.println();
}

void initHomeAssistant() {
    device.setName("AquaDoser");
    device.setModel("AD ESP8266");
    device.setManufacturer("PMW");
    device.setSoftwareVersion("1.0.0");
    
    // Inicjalizacja encji HA
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        setupPumpEntities(i);
    }
    
    setupServiceModeSwitch();
}

/***************************************
 * IMPLEMENTACJE FUNKCJI - INICJALIZACJA
 ***************************************/

void setup() {
    // 1. Inicjalizacja komunikacji
    Serial.begin(115200);
    Serial.println("\nAquaDoser Start");
    Wire.begin();
    
    // 2. Inicjalizacja pamięci i konfiguracji
    EEPROM.begin(512);
    //loadMQTTConfig(); 
    initStorage();
    loadConfiguration();
    
    // 3. Inicjalizacja sprzętu
    // Piny
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    
    // RTC
    if (!rtc.begin()) {
        Serial.println("Nie znaleziono RTC!");
    }
    
    // PCF8574
    if (!pcf8574.begin()) {
        Serial.println("Nie znaleziono PCF8574!");
    }
    
    // LED
    strip.begin();
    strip.show();
    initializeLEDs();
    
    // 4. Inicjalizacja sieci i komunikacji
    setupWiFi();
    checkMQTTConfig();
    initHomeAssistant();
    setupMQTT();

    // Inicjalizacja WebSocket
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    
    // 5. Konfiguracja serwera Web
    setupWebServer();
    server.on("/save", HTTP_POST, handleSave);
    server.on("/restart", HTTP_POST, []() {
        server.send(200, "text/plain", "Restarting...");
        delay(1000);
        ESP.restart();
    });
    server.on("/factory-reset", HTTP_POST, []() {
        server.send(200, "text/plain", "Resetting to factory defaults...");
        resetFactorySettings();
        delay(1000);
        ESP.restart();
    });
    server.on("/calibrate", handleCalibration);
    
    // 6. Synchronizacja czasu
    syncRTC();
    
    // 7. Efekty startowe
    playWelcomeEffect();
    welcomeMelody();
    
    Serial.println("Inicjalizacja zakończona");
}

void loop() {
    unsigned long currentMillis = millis();
    
    // Obsługa podstawowych usług systemowych
    server.handleClient();          // Obsługa żądań HTTP
    webSocket.loop();              // Obsługa WebSocket
    mqtt.loop();                   // Obsługa MQTT
    checkCalibration();
    
    // Obsługa interfejsu użytkownika
    handleButton();                // Obsługa przycisku
    updateLEDs();                  // Aktualizacja diod LED
    
    // Aktualizacja statusu przez WebSocket (co 1 sekundę)
    static unsigned long lastWebSocketUpdate = 0;
    if (currentMillis - lastWebSocketUpdate >= WEBSOCKET_UPDATE_INTERVAL) {
        String status = getSystemStatusJSON();
        webSocket.broadcastTXT(status);
        lastWebSocketUpdate = currentMillis;
    }
    
    // Logowanie stanu systemu (co LOG_INTERVAL lub przy pierwszym uruchomieniu)
    if (firstRun || (currentMillis - lastLogTime >= LOG_INTERVAL)) {
        printLogHeader();
        
        bool hasActivePumps = false;
        for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
            if (pumps[i].enabled) {
                printPumpStatus(i);
                hasActivePumps = true;
            }
        }
        
        if (!hasActivePumps) {
            Serial.println(F("Brak skonfigurowanych pomp"));
        }
        
        Serial.println(F("========================================\n"));
        lastLogTime = currentMillis;
        firstRun = false;
    }

    // Sprawdzanie stanu systemu (co SYSTEM_CHECK_INTERVAL)
    static unsigned long lastSystemCheck = 0;
    if (currentMillis - lastSystemCheck >= SYSTEM_CHECK_INTERVAL) {
        // Sprawdzanie połączenia WiFi
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println(F("Utracono połączenie WiFi - próba ponownego połączenia..."));
            WiFi.reconnect();
        }
        
        // Aktualizacja integracji z Home Assistant
        updateHomeAssistant();
        
        // Kontrola pracy pomp
        handlePumps();
        
        lastSystemCheck = currentMillis;
    }
    
    // Synchronizacja zegara RTC (co RTC_SYNC_INTERVAL lub przy starcie)
    static unsigned long lastRtcSync = 0;
    if (currentMillis - lastRtcSync >= RTC_SYNC_INTERVAL || lastRtcSync == 0) {
        syncRTC();
        lastRtcSync = currentMillis;
    }
    
    // Obsługa przepełnienia licznika millis()
    if (currentMillis < lastLogTime || 
        currentMillis < lastSystemCheck || 
        currentMillis < lastRtcSync || 
        currentMillis < lastWebSocketUpdate) {
        // Reset wszystkich liczników czasowych
        lastLogTime = currentMillis;
        lastSystemCheck = currentMillis;
        lastRtcSync = currentMillis;
        lastWebSocketUpdate = currentMillis;
    }
    
    yield();    // Obsługa zadań systemowych ESP8266
}
