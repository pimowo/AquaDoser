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

/***************************************
 * DEFINICJE STAŁYCH
 ***************************************/

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
    // Dane konfiguracyjne pompy
};

// --- Stan pompy
struct PumpState {
    // Aktualny stan pompy
};

// --- Konfiguracja MQTT
struct MQTTConfig {
    // Ustawienia MQTT
};

// --- Konfiguracja sieci
struct NetworkConfig {
    // Ustawienia sieciowe
};

// --- Konfiguracja systemu
struct SystemConfig {
    // Ustawienia systemowe
};

// --- Stan systemu
struct SystemStatus {
    // Status systemu
};

// --- Informacje systemowe
struct SystemInfo {
    // Informacje o systemie
};

// --- Stan LED
struct LEDState {
    // Stan diod LED
};

/***************************************
 * ZMIENNE GLOBALNE
 ***************************************/

// --- Obiekty sprzętowe
RTC_DS3231 rtc;                // Zegar RTC
PCF8574 pcf(PCF8574_ADDRESS);  // Ekspander I/O
Adafruit_NeoPixel strip(NUMBER_OF_PUMPS, LED_PIN, NEO_GRB + NEO_KHZ800); // LED

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

// --- Funkcje inicjalizacyjne
void initStorage();              // Inicjalizacja pamięci
void initializeLEDs();          // Inicjalizacja LED
bool initializePumps();         // Inicjalizacja pomp
void initHomeAssistant();       // Inicjalizacja Home Assistant

// --- Funkcje konfiguracyjne
void loadConfiguration();        // Wczytanie konfiguracji
void saveConfiguration();        // Zapisanie konfiguracji
bool loadPumpsConfig();         // Wczytanie konfiguracji pomp
bool savePumpsConfig();         // Zapisanie konfiguracji pomp
void loadMQTTConfig();          // Wczytanie konfiguracji MQTT
void saveMQTTConfig();          // Zapisanie konfiguracji MQTT
bool loadNetworkConfig();        // Wczytanie konfiguracji sieci
bool saveNetworkConfig();        // Zapisanie konfiguracji sieci
void resetFactorySettings();     // Reset do ustawień fabrycznych

// --- Funkcje obsługi pomp
void startPump(uint8_t pumpIndex);    // Uruchomienie pompy
void stopPump(uint8_t pumpIndex);     // Zatrzymanie pompy
void stopAllPumps();                  // Zatrzymanie wszystkich pomp
void handlePumps();                   // Obsługa pomp
bool shouldStartDosing(uint8_t pumpIndex); // Sprawdzenie warunku dozowania
void servicePump(uint8_t pumpIndex, bool state); // Obsługa trybu serwisowego
void toggleServiceMode();             // Przełączenie trybu serwisowego

// --- Funkcje obsługi LED
void updateLEDs();              // Aktualizacja wszystkich LED
void updateServiceModeLEDs();   // Aktualizacja LED w trybie serwisowym
void updatePumpLED(uint8_t pumpIndex); // Aktualizacja LED pojedynczej pompy
void updateAllPumpLEDs();       // Aktualizacja LED wszystkich pomp
void setLEDColor(uint8_t index, uint32_t color, bool withPulsing = false);

// --- Funkcje sieciowe
void setupWiFi();               // Konfiguracja WiFi
void setupMQTT();              // Konfiguracja MQTT
void setupWebServer();          // Konfiguracja serwera WWW
void checkMQTTConfig();        // Sprawdzenie konfiguracji MQTT
void updateHomeAssistant();     // Aktualizacja Home Assistant

// --- Funkcje obsługi zdarzeń
void handleButton();            // Obsługa przycisku
void handleRoot();             // Obsługa strony głównej
void handleSave();             // Obsługa zapisu konfiguracji
void handleConfigSave();       // Obsługa zapisu konfiguracji
void handleWebSocketMessage(uint8_t num, uint8_t * payload, size_t length);
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);

// --- Funkcje pomocnicze
void syncRTC();                // Synchronizacja czasu
void playWelcomeEffect();      // Efekt powitalny
void welcomeMelody();          // Melodia powitalna
void playShortWarningSound();  // Krótki sygnał ostrzegawczy
void playConfirmationSound(); // Sygnał potwierdzenia
bool validateConfigValues();   // Walidacja konfiguracji
bool validateMQTTConfig();    // Walidacja konfiguracji MQTT
void updateSystemStatus();     // Aktualizacja statusu systemu
void handleMillisOverflow();  // Obsługa przepełnienia millis()
String formatDateTime(const DateTime& dt); // Formatowanie czasu
String getSystemStatusJSON();  // Pobranie statusu w formacie JSON
String getUpdatePage();        // Pobranie strony aktualizacji
String getConfigPage();       // Pobranie strony konfiguracji
String getStyles();           // Pobranie stylów CSS

// --- Funkcje callback Home Assistant
void onPumpSwitch(bool state, HASwitch* sender);
void onPumpCalibration(HANumeric value, HANumber* sender);
void onServiceModeSwitch(bool state, HASwitch* sender);

/***************************************
 * IMPLEMENTACJE FUNKCJI - INICJALIZACJA
 ***************************************/

void setup() {
}

void loop() {
}
