// --- Biblioteki

#include <Arduino.h>  // Podstawowa biblioteka Arduino zawierająca funkcje rdzenia
#include <ArduinoHA.h>  // Biblioteka do integracji z Home Assistant przez protokół MQTT
#include <ArduinoOTA.h>  // Biblioteka do aktualizacji oprogramowania przez sieć WiFi
#include <ESP8266WiFi.h>  // Biblioteka WiFi dedykowana dla układu ESP8266
#include <EEPROM.h>  // Biblioteka do dostępu do pamięci nieulotnej EEPROM
#include <WiFiManager.h>  // Biblioteka do zarządzania połączeniami WiFi
#include <ESP8266WebServer.h>  // Biblioteka do obsługi serwera HTTP na ESP8266
#include <WebSocketsServer.h>  // Biblioteka do obsługi serwera WebSockets na ESP8266
#include <ESP8266HTTPUpdateServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <PCF8574.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <NTPClient.h>
#include <WiFiUDP.h>

// Definicje MQTT
#define MQTT_SERVER_LENGTH 40
#define MQTT_USER_LENGTH 20
#define MQTT_PASSWORD_LENGTH 20

// Zmienne MQTT
bool mqttEnabled = false;
char mqttServer[MQTT_SERVER_LENGTH] = "";
uint16_t mqttPort = 1883;
char mqttUser[MQTT_USER_LENGTH] = "";
char mqttPassword[MQTT_PASSWORD_LENGTH] = "";
bool shouldRestart = false;
unsigned long restartTime = 0;
unsigned long lastStatusPrint = 0;
const unsigned long STATUS_PRINT_INTERVAL = 60000; // Wyświetlaj status co 1 minutę
const unsigned long LOG_INTERVAL = 60000; // 60 sekund między wyświetlaniem statusu
unsigned long lastLogTime = 0;
bool firstRun = true;

#define TEST_MODE  // Zakomentuj tę linię w wersji produkcyjnej

#ifdef TEST_MODE
  #define DEBUG_SERIAL(x) Serial.println(x)
  // Emulacja RTC
  class RTCEmulator {
    private:
      unsigned long startupTime;
      DateTime currentTime;
      
    public:
      RTCEmulator() {
        startupTime = millis();
        currentTime = DateTime(2024, 1, 1, 0, 0, 0);
      }
      
      bool begin() {
        return true;
      }
      
      void adjust(const DateTime& dt) {
        currentTime = dt;
        startupTime = millis();
      }
      
      DateTime now() {
        // Symuluj czas od ostatniego ustawienia
        unsigned long currentMillis = millis();
        unsigned long secondsElapsed = (currentMillis - startupTime) / 1000;
        
        uint32_t totalSeconds = currentTime.unixtime() + secondsElapsed;
        return DateTime(totalSeconds);
      }
  };

  // Emulacja PCF8574
  class PCF8574Emulator {
    public:
      uint8_t currentState;
      PCF8574Emulator() : currentState(0xFF) {}
      bool begin() { return true; }
      uint8_t read8() { return currentState; }
      void write(uint8_t pin, uint8_t value) {
        if (value == HIGH) {
          currentState |= (1 << pin);
        } else {
          currentState &= ~(1 << pin);
        }
        DEBUG_SERIAL("PCF Pin " + String(pin) + " set to " + String(value));
      }
  };

  RTCEmulator rtc;
  PCF8574Emulator pcf;

#else
  #define DEBUG_SERIAL(x)
  RTC_DS3231 rtc;
  PCF8574 pcf(PCF8574_ADDRESS);
#endif

// --- Definicje pinów i stałych
#define NUMBER_OF_PUMPS 8  // lub inna odpowiednia liczba
#define NUM_PUMPS 8              // Liczba pomp
#define LED_PIN D1               // Pin danych dla WS2812
#define BUTTON_PIN D2            // Pin przycisku serwisowego
#define DEBOUNCE_TIME 50         // Czas debounce w ms

// --- Kolory LED
#define COLOR_OFF 0xFF0000      // Czerwony (pompa wyłączona)
#define COLOR_ON 0x00FF00       // Zielony (pompa włączona)
#define COLOR_WORKING 0x0000FF  // Niebieski (pompa pracuje)
#define COLOR_SERVICE 0xFFFF00  // Żółty (tryb serwisowy)

// --- Struktura konfiguracji pompy
// Struktura konfiguracji pompy
struct PumpConfig {
    char name[20];
    float doseAmount;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    bool enabled;
    
    PumpConfig() : 
        doseAmount(0),
        hour(0),
        minute(0),
        days(0),
        enabled(false) {
        name[0] = '\0';
    }
};

// --- Struktura konfiguracji sieciowej
struct NetworkConfig {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_server[40];
    char mqtt_user[32];
    char mqtt_password[64];
    uint16_t mqtt_port;
};

struct SystemStatus {
    unsigned long uptime;
    bool mqtt_connected;
    bool wifi_connected;
    bool rtc_synced;
    String lastError;
    
    SystemStatus() : 
        uptime(0), 
        mqtt_connected(false),
        wifi_connected(false),
        rtc_synced(false),
        lastError("") {}
};

struct MQTTConfig {
    char broker[40];
    int port;
    char username[20];
    char password[20];
    
    MQTTConfig() : port(1883) {
        broker[0] = '\0';
        username[0] = '\0';
        password[0] = '\0';
    }
};

MQTTConfig mqttConfig;

ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
ESP8266HTTPUpdateServer httpUpdateServer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

const char* dayNames[] = {"Pn", "Wt", "Sr", "Cz", "Pt", "So", "Nd"};


// Struktura dla informacji systemowych
struct SystemInfo {
    unsigned long uptime;
    bool mqtt_connected;
};

SystemInfo sysInfo = {0, false};

struct Config {
    PumpConfig pumps[NUM_PUMPS];
    MQTTConfig mqtt;  // Dodaj to pole
    bool soundEnabled;
    uint32_t checksum;
    
    Config() : soundEnabled(true), checksum(0) {}
};

// Globalna instancja konfiguracji
Config config;

struct PumpState {
    bool isActive;
    unsigned long lastDoseTime;
    HASensor* sensor;      // Dodajemy wskaźnik do HASensor
    
    PumpState() : isActive(false), lastDoseTime(0), sensor(nullptr) {}
};

// Tablica stanów pomp - tylko jedna deklaracja!
PumpState pumpStates[NUMBER_OF_PUMPS];

// --- Globalne obiekty
WiFiClient wifiClient;
HADevice haDevice("AquaDoser");
HAMqtt mqtt(wifiClient, haDevice);
//RTC_DS3231 rtc;
PCF8574 pcf8574(0x20);
Adafruit_NeoPixel strip(NUM_PUMPS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Globalne zmienne stanu
bool serviceMode = false;
PumpConfig pumps[NUM_PUMPS];
NetworkConfig networkConfig;
bool pumpRunning[NUM_PUMPS] = {false};
unsigned long doseStartTime[NUM_PUMPS] = {0};
//unsigned long lastRtcSync = 0;

// Dodaj stałe dla timeoutów i interwałów jak w HydroSense
const unsigned long MQTT_LOOP_INTERVAL = 100;      // Obsługa MQTT co 100ms
const unsigned long OTA_CHECK_INTERVAL = 1000;     // Sprawdzanie OTA co 1s
const unsigned long MQTT_RETRY_INTERVAL = 10000;   // Próba połączenia MQTT co 10s
const unsigned long MILLIS_OVERFLOW_THRESHOLD = 4294967295U - 60000; // ~49.7 dni

unsigned long lastMQTTLoop = 0;
unsigned long lastMeasurement = 0;
unsigned long lastOTACheck = 0;

// --- Deklaracje encji Home Assistant
HASwitch* pumpSwitches[NUM_PUMPS];
//HASensor* pumpStates[NUM_PUMPS];
HANumber* pumpCalibrations[NUM_PUMPS];
HASwitch* serviceModeSwitch;

// Deklaracje funkcji - będą zaimplementowane w kolejnych częściach
void loadConfiguration();
void saveConfiguration();
void setupWiFi();
void setupMQTT();
void setupRTC();
void setupLED();
void handlePumps();
void updateHomeAssistant();
bool validateConfigValues();

const int BUZZER_PIN = D2;

bool hasIntervalPassed(unsigned long current, unsigned long previous, unsigned long interval) {
    return (current - previous >= interval);
}

// Stałe dla systemu logowania
//const unsigned long LOG_INTERVAL = 60000; // 60 sekund między wyświetlaniem statusu
//unsigned long lastLogTime = 0;
//bool firstRun = true;

// Funkcja formatująca czas
String formatDateTime(const DateTime& dt) {
    char buffer[20];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
             dt.year(), dt.month(), dt.day(),
             dt.hour(), dt.minute(), dt.second());
    return String(buffer);
}

// Funkcja wyświetlająca nagłówek logu
void printLogHeader() {
    Serial.println(F("\n========================================"));
    Serial.print(F("Status update at: "));
    Serial.println(formatDateTime(rtc.now()));
    Serial.println(F("----------------------------------------"));
}

// Funkcja wyświetlająca status pojedynczej pompy
void printPumpStatus(uint8_t pumpIndex) {
    if (!config.pumps[pumpIndex].enabled) {
        return; // Nie wyświetlaj statusu wyłączonych pomp
    }

    Serial.print(F("Pump "));
    Serial.print(pumpIndex + 1);
    Serial.print(F(": "));

    if (config.pumps[pumpIndex].isRunning) {
        Serial.println(F("ACTIVE - Currently dosing"));
    } else {
        Serial.print(F("Standby - Next dose: "));
        DateTime nextDose = calculateNextDosing(pumpIndex);
        Serial.print(formatDateTime(nextDose));
        
        if (config.pumps[pumpIndex].lastDosing > 0) {
            Serial.print(F(" (Last: "));
            time_t lastDose = config.pumps[pumpIndex].lastDosing;
            DateTime lastDoseTime(lastDose);
            Serial.print(formatDateTime(lastDoseTime));
            Serial.print(F(")"));
        }
        Serial.println();
    }
}

void printPumpStatus(int pumpIndex) {
    if (!config.pumps[pumpIndex].enabled) {
        Serial.print(F("Pump "));
        Serial.print(pumpIndex + 1);
        Serial.println(F(": Disabled"));
        return;
    }

    String status = "Pump " + String(pumpIndex + 1) + ": ";
    
    if (config.pumps[pumpIndex].isRunning) {
        status += "Active (Dosing)";
    } else {
        if (config.pumps[pumpIndex].lastDosing > 0) {
            time_t lastDose = config.pumps[pumpIndex].lastDosing;
            struct tm* timeinfo = localtime(&lastDose);
            char timeStr[20];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", timeinfo);
            status += "Inactive (Last: " + String(timeStr) + ")";
        } else {
            status += "Inactive (No previous runs)";
        }

        // Dodaj informację o następnym zaplanowanym dozowaniu
        if (config.pumps[pumpIndex].enabled) {
            DateTime nextDose = calculateNextDosing(pumpIndex);
            status += " - Next: ";
            status += String(nextDose.year()) + "-";
            status += (nextDose.month() < 10 ? "0" : "") + String(nextDose.month()) + "-";
            status += (nextDose.day() < 10 ? "0" : "") + String(nextDose.day()) + " ";
            status += (nextDose.hour() < 10 ? "0" : "") + String(nextDose.hour()) + ":";
            status += (nextDose.minute() < 10 ? "0" : "") + String(nextDose.minute());
        }
    }

    Serial.println(status);
}

// --- Stałe dla systemu plików
const char* CONFIG_DIR = "/config";
const char* PUMPS_FILE = "/config/pumps.json";
const char* NETWORK_FILE = "/config/network.json";
const char* SYSTEM_FILE = "/config/system.json";

//Config config;                   // Główna konfiguracja
SystemStatus systemStatus;       // Status systemu

// Dodaj obsługę dźwięku jak w HydroSense
void playShortWarningSound() {
    if (!config.soundEnabled) return;
    tone(BUZZER_PIN, 2000, 100);
}

void playConfirmationSound() {
    if (!config.soundEnabled) return;
    tone(BUZZER_PIN, 1000, 50);
    delay(100);
    tone(BUZZER_PIN, 2000, 50);
}

void welcomeMelody() {
    if (!config.soundEnabled) return;
    tone(BUZZER_PIN, 1000, 100);
    delay(150);
    tone(BUZZER_PIN, 1500, 100);
    delay(150);
    tone(BUZZER_PIN, 2000, 100);
}

// Dodaj walidację konfiguracji
bool validateConfigValues() {
    // Sprawdź poprawność wartości dla każdej pompy
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (config.pumps[i].calibration <= 0 || config.pumps[i].calibration > 10) 
            return false;
        if (config.pumps[i].dose < 0 || config.pumps[i].dose > 1000) 
            return false;
    }
    return true;
}

void handleWebSocketMessage(uint8_t num, uint8_t * payload, size_t length) {
    String message = String((char*)payload);
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) {
        Serial.println("Failed to parse WebSocket message");
        return;
    }
    
    const char* type = doc["type"];
    if (strcmp(type, "getPumpStatus") == 0) {
        String status = getSystemStatusJSON();
        webSocket.sendTXT(num, status);
    }
}

// Pojedyncza definicja webSocketEvent
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WebSocket] Client #%u Disconnected\n", num);
            break;
        case WStype_CONNECTED:
            {
                Serial.printf("[WebSocket] Client #%u Connected\n", num);
                String json = getSystemStatusJSON();
                webSocket.sendTXT(num, json);
            }
            break;
        case WStype_TEXT:
            handleWebSocketMessage(num, payload, length);
            break;
    }
}

String getSystemStatusJSON() {
    StaticJsonDocument<512> doc;
    doc["uptime"] = systemStatus.uptime;
    doc["mqtt"] = systemStatus.mqtt_connected;
    
    JsonArray pumps = doc.createNestedArray("pumps");
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        JsonObject pump = pumps.createNestedObject();
        pump["id"] = i;
        pump["active"] = pumpStates[i].isActive;
        pump["lastDose"] = pumpStates[i].lastDoseTime;
    }
    
    String json;
    serializeJson(doc, json);
    return json;
}

void updateSystemStatus() {
    systemStatus.uptime = millis() / 1000;
    systemStatus.wifi_connected = WiFi.status() == WL_CONNECTED;
    systemStatus.mqtt_connected = mqtt.isConnected();
}

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

// Zapisywanie konfiguracji MQTT
void saveMQTTConfig() {
    StaticJsonDocument<512> doc;
    
    doc["enabled"] = mqttEnabled;
    doc["server"] = mqttServer;
    doc["port"] = mqttPort;
    doc["user"] = mqttUser;
    doc["password"] = mqttPassword;

    File configFile = LittleFS.open("/mqtt.json", "w");
    if (!configFile) {
        Serial.println("Failed to open mqtt config file for writing");
        return;
    }

    serializeJson(doc, configFile);
    configFile.close();
}

// Wczytywanie konfiguracji MQTT
void loadMQTTConfig() {
    File configFile = LittleFS.open("/mqtt.json", "r");
    if (!configFile) {
        Serial.println("No mqtt config file, using defaults");
        return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error) {
        Serial.println("Failed to parse mqtt config file");
        return;
    }

    mqttEnabled = doc["enabled"] | false;
    strlcpy(mqttServer, doc["server"] | "", sizeof(mqttServer));
    mqttPort = doc["port"] | 1883;
    strlcpy(mqttUser, doc["user"] | "", sizeof(mqttUser));
    strlcpy(mqttPassword, doc["password"] | "", sizeof(mqttPassword));
}



// --- Inicjalizacja systemu plików
bool initFileSystem() {
    if (!LittleFS.begin()) {
        Serial.println("Błąd inicjalizacji LittleFS!");
        return false;
    }

    // Sprawdź czy istnieje katalog konfiguracji
    if (!LittleFS.exists(CONFIG_DIR)) {
        LittleFS.mkdir(CONFIG_DIR);
    }
    return true;
}

// --- Ładowanie konfiguracji pomp
bool loadPumpsConfig() {
    File file = LittleFS.open("/pumps.json", "r");
    if (!file) {
        Serial.println(F("Failed to open pumps config file"));
        return false;
    }

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.println(F("Failed to parse pumps config file"));
        return false;
    }

    JsonArray pumpsArray = doc["pumps"].as<JsonArray>();
    for (uint8_t i = 0; i < NUM_PUMPS && i < pumpsArray.size(); i++) {
        pumps[i].enabled = pumpsArray[i]["enabled"] | false;
        pumps[i].calibration = pumpsArray[i]["calibration"] | 1.0f;
        pumps[i].dose = pumpsArray[i]["dose"] | 0.0f;
        pumps[i].schedule_days = pumpsArray[i]["schedule"]["days"] | 0;
        pumps[i].schedule_hour = pumpsArray[i]["schedule"]["hour"] | 0;
        pumps[i].lastDosing = 0;
        pumps[i].isRunning = false;
    }

    return true;
}

// --- Zapisywanie konfiguracji pomp
bool savePumpsConfig() {
    DynamicJsonDocument doc(1024);
    JsonArray pumpsArray = doc.createNestedArray("pumps");

    for (uint8_t i = 0; i < NUM_PUMPS; i++) {
        JsonObject pumpObj = pumpsArray.createNestedObject();
        pumpObj["enabled"] = pumps[i].enabled;
        pumpObj["calibration"] = pumps[i].calibration;
        pumpObj["dose"] = pumps[i].dose;
        JsonObject schedule = pumpObj.createNestedObject("schedule");
        schedule["days"] = pumps[i].schedule_days;
        schedule["hour"] = pumps[i].schedule_hour;
    }

    File file = LittleFS.open("/pumps.json", "w");
    if (!file) {
        Serial.println(F("Failed to open pumps config file for writing"));
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println(F("Failed to write pumps config"));
        file.close();
        return false;
    }

    file.close();
    return true;
}

// --- Ładowanie konfiguracji sieciowej
bool loadNetworkConfig() {
    File file = LittleFS.open(NETWORK_FILE, "r");
    if (!file) {
        Serial.println("Nie znaleziono pliku konfiguracji sieciowej");
        return false;
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.println("Błąd parsowania JSON konfiguracji sieciowej");
        return false;
    }

    strlcpy(networkConfig.wifi_ssid, doc["wifi_ssid"] | "", sizeof(networkConfig.wifi_ssid));
    strlcpy(networkConfig.wifi_password, doc["wifi_password"] | "", sizeof(networkConfig.wifi_password));
    strlcpy(networkConfig.mqtt_server, doc["mqtt_server"] | "", sizeof(networkConfig.mqtt_server));
    strlcpy(networkConfig.mqtt_user, doc["mqtt_user"] | "", sizeof(networkConfig.mqtt_user));
    strlcpy(networkConfig.mqtt_password, doc["mqtt_password"] | "", sizeof(networkConfig.mqtt_password));
    networkConfig.mqtt_port = doc["mqtt_port"] | 1883;

    return true;
}

// --- Zapisywanie konfiguracji sieciowej
bool saveNetworkConfig() {
    StaticJsonDocument<512> doc;
    
    doc["wifi_ssid"] = networkConfig.wifi_ssid;
    doc["wifi_password"] = networkConfig.wifi_password;
    doc["mqtt_server"] = networkConfig.mqtt_server;
    doc["mqtt_user"] = networkConfig.mqtt_user;
    doc["mqtt_password"] = networkConfig.mqtt_password;
    doc["mqtt_port"] = networkConfig.mqtt_port;

    File file = LittleFS.open(NETWORK_FILE, "w");
    if (!file) {
        Serial.println("Błąd otwarcia pliku konfiguracji sieciowej do zapisu");
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("Błąd zapisu konfiguracji sieciowej");
        file.close();
        return false;
    }

    file.close();
    return true;
}

// --- Ogólna funkcja ładowania konfiguracji
void loadConfiguration() {
    if (!initFileSystem()) {
        Serial.println("Używam domyślnych ustawień");
        return;
    }

    if (!loadPumpsConfig()) {
        Serial.println("Używam domyślnych ustawień pomp");
        // Tutaj możemy zainicjować domyślne wartości dla pomp
        for (int i = 0; i < NUM_PUMPS; i++) {
            pumps[i].enabled = false;
            pumps[i].calibration = 1.0;
            pumps[i].dose = 0.0;
            pumps[i].schedule_days = 0;
            pumps[i].schedule_hour = 0;
        }
    }

    if (!loadNetworkConfig()) {
        Serial.println("Używam domyślnych ustawień sieciowych");
        // WiFiManager zajmie się konfiguracją sieci
    }
}

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

void handleRoot() {
    server.send(200, "text/html", getConfigPage());
}

void handleSave() {
    if (server.hasArg("plain")) {
        String json = server.arg("plain");
        DynamicJsonDocument doc(2048);  // Zwiększony rozmiar dla dodatkowych danych
        DeserializationError error = deserializeJson(doc, json);
        
        if (!error) {
            // Obsługa konfiguracji pomp
            if (doc.containsKey("pumps")) {
                JsonArray pumpsArray = doc["pumps"].as<JsonArray>();
                for (uint8_t i = 0; i < NUM_PUMPS && i < pumpsArray.size(); i++) {
                    pumps[i].enabled = pumpsArray[i]["enabled"] | false;
                    pumps[i].calibration = pumpsArray[i]["calibration"] | 1.0f;
                    pumps[i].dose = pumpsArray[i]["dose"] | 0.0f;
                    pumps[i].schedule_hour = pumpsArray[i]["schedule"]["hour"] | 0;
                    pumps[i].schedule_days = pumpsArray[i]["schedule"]["days"] | 0;
                }
                savePumpsConfig();
            }

            // Obsługa konfiguracji MQTT
            if (doc.containsKey("mqtt")) {
                JsonObject mqtt = doc["mqtt"];
                mqttEnabled = mqtt["enabled"] | false;
                strlcpy(mqttServer, mqtt["server"] | "", sizeof(mqttServer));
                mqttPort = mqtt["port"] | 1883;
                strlcpy(mqttUser, mqtt["user"] | "", sizeof(mqttUser));
                strlcpy(mqttPassword, mqtt["password"] | "", sizeof(mqttPassword));
                saveMQTTConfig();
            }

            server.send(200, "text/plain", "Configuration saved");
        } else {
            server.send(400, "text/plain", "Invalid JSON");
        }
    } else {
        server.send(400, "text/plain", "No data");
    }
}

// --- Ogólna funkcja zapisywania konfiguracji
void saveConfiguration() {
    if (!savePumpsConfig()) {
        Serial.println("Błąd zapisu konfiguracji pomp!");
    }
    if (!saveNetworkConfig()) {
        Serial.println("Błąd zapisu konfiguracji sieciowej!");
    }
}

// --- Stałe czasowe
#define DOSE_CHECK_INTERVAL 1000    // Sprawdzanie dozowania co 1 sekundę
#define MIN_DOSE_TIME 100          // Minimalny czas dozowania (ms)
#define MAX_DOSE_TIME 60000        // Maksymalny czas dozowania (60 sekund)

// --- Zmienne dla obsługi pomp
unsigned long lastDoseCheck = 0;
bool pumpInitialized = false;

// --- Inicjalizacja PCF8574 i pomp
bool initializePumps() {
    if (!pcf8574.begin()) {
        Serial.println("Błąd inicjalizacji PCF8574!");
        return false;
    }

    // Ustaw wszystkie piny jako wyjścia i wyłącz pompy
    for (int i = 0; i < NUM_PUMPS; i++) {
        pcf8574.digitalWrite(i, LOW);  // Nie potrzeba pinMode
    }

    pumpInitialized = true;
    return true;
}

// --- Rozpoczęcie dozowania dla pompy
void startPump(uint8_t pumpIndex) {
    if (!pumpInitialized || pumpIndex >= NUM_PUMPS || pumpRunning[pumpIndex]) {
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

// --- Zatrzymanie dozowania dla pompy
void stopPump(uint8_t pumpIndex) {
    if (!pumpInitialized || pumpIndex >= NUM_PUMPS || !pumpRunning[pumpIndex]) {
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

// --- Sprawdzenie czy pompa powinna rozpocząć dozowanie
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

// --- Główna funkcja obsługi pomp
void handlePumps() {
    if (!pumpInitialized || serviceMode) {
        return;
    }

    unsigned long currentMillis = millis();
    
    // Sprawdzaj stan pomp co DOSE_CHECK_INTERVAL
    if (currentMillis - lastDoseCheck >= DOSE_CHECK_INTERVAL) {
        lastDoseCheck = currentMillis;
        
        for (uint8_t i = 0; i < NUM_PUMPS; i++) {
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

// --- Obsługa trybu serwisowego dla pomp
void servicePump(uint8_t pumpIndex, bool state) {
    if (!pumpInitialized || pumpIndex >= NUM_PUMPS || !serviceMode) {
        return;
    }

    // Ustaw stan pompy
    pcf8574.digitalWrite(pumpIndex, state ? HIGH : LOW);
    
    // Aktualizuj wyświetlanie
    strip.show();
}

void toggleServiceMode() {
    serviceMode = !serviceMode;
    if (serviceMode) {
        // Zatrzymaj wszystkie pompy w trybie serwisowym
        for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
            stopPump(i);
        }
    }
}

// --- Zatrzymanie wszystkich pomp
void stopAllPumps() {
    for (uint8_t i = 0; i < NUM_PUMPS; i++) {
        if (pumpRunning[i]) {
            stopPump(i);
        }
    }
}

// --- Stałe dla animacji LED
#define LED_UPDATE_INTERVAL 50    // Aktualizacja LED co 50ms
#define FADE_STEPS 20            // Liczba kroków w animacji fade
#define PULSE_MIN_BRIGHTNESS 20  // Minimalna jasność podczas pulsowania (0-255)
#define PULSE_MAX_BRIGHTNESS 255 // Maksymalna jasność podczas pulsowania

// --- Struktury i zmienne dla LED
struct LEDState {
    uint32_t targetColor;     // Docelowy kolor
    uint32_t currentColor;    // Aktualny kolor
    uint8_t brightness;       // Aktualna jasność
    bool pulsing;            // Czy LED pulsuje
    int pulseDirection;      // Kierunek pulsowania (1 lub -1)
};

LEDState ledStates[NUM_PUMPS];
unsigned long lastLedUpdate = 0;

// --- Inicjalizacja LED
void initializeLEDs() {
    strip.begin();
    
    // Inicjalizacja stanów LED
    for (int i = 0; i < NUM_PUMPS; i++) {
        ledStates[i] = {
            COLOR_OFF,    // targetColor
            COLOR_OFF,    // currentColor
            255,         // brightness
            false,       // pulsing
            1           // pulseDirection
        };
    }
    
    updateLEDs();
}

// --- Konwersja koloru na komponenty RGB
void colorToRGB(uint32_t color, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

// --- Konwersja komponentów RGB na kolor
uint32_t RGBToColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// --- Interpolacja między dwoma kolorami
uint32_t interpolateColor(uint32_t color1, uint32_t color2, float ratio) {
    uint8_t r1, g1, b1, r2, g2, b2;
    colorToRGB(color1, r1, g1, b1);
    colorToRGB(color2, r2, g2, b2);
    
    uint8_t r = r1 + (r2 - r1) * ratio;
    uint8_t g = g1 + (g2 - g1) * ratio;
    uint8_t b = b1 + (b2 - b1) * ratio;
    
    return RGBToColor(r, g, b);
}

// --- Ustawienie koloru LED z animacją
void setLEDColor(uint8_t index, uint32_t color, bool withPulsing = false) {
    if (index >= NUM_PUMPS) return;
    
    ledStates[index].targetColor = color;
    ledStates[index].pulsing = withPulsing;
}

// --- Aktualizacja wszystkich LED
void updateLEDs() {
    unsigned long currentMillis = millis();
    
    // Aktualizuj tylko co LED_UPDATE_INTERVAL
    if (currentMillis - lastLedUpdate < LED_UPDATE_INTERVAL) {
        return;
    }
    lastLedUpdate = currentMillis;
    
    // Aktualizacja każdego LED
    for (int i = 0; i < NUM_PUMPS; i++) {
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

// --- Aktualizacja LED dla trybu serwisowego
void updateServiceModeLEDs() {
    uint32_t color = serviceMode ? COLOR_SERVICE : COLOR_OFF;
    
    for (int i = 0; i < NUM_PUMPS; i++) {
        if (!pumpRunning[i]) {
            setLEDColor(i, color, serviceMode);
        }
    }
}

// --- Aktualizacja LED dla pompy
void updatePumpLED(uint8_t pumpIndex) {
    if (pumpIndex >= NUM_PUMPS) return;
    
    if (serviceMode) {
        setLEDColor(pumpIndex, COLOR_SERVICE, true);
    } else if (pumpRunning[pumpIndex]) {
        setLEDColor(pumpIndex, COLOR_WORKING, true);
    } else {
        setLEDColor(pumpIndex, pumps[pumpIndex].enabled ? COLOR_ON : COLOR_OFF, false);
    }
}

// --- Aktualizacja wszystkich LED dla pomp
void updateAllPumpLEDs() {
    for (uint8_t i = 0; i < NUM_PUMPS; i++) {
        updatePumpLED(i);
    }
}

// --- Stałe dla Home Assistant
#define HA_UPDATE_INTERVAL 30000  // Aktualizacja HA co 30 sekund
unsigned long lastHaUpdate = 0;

// --- Funkcje callback dla Home Assistant
void onPumpSwitch(bool state, HASwitch* sender) {
    // Znajdź indeks pompy na podstawie wskaźnika do przełącznika
    int pumpIndex = -1;
    for (int i = 0; i < NUM_PUMPS; i++) {
        if (sender == pumpSwitches[i]) {
            pumpIndex = i;
            break;
        }
    }
    
    if (pumpIndex >= 0) {
        pumps[pumpIndex].enabled = state;
        updatePumpLED(pumpIndex);
        saveConfiguration();
    }
}

void onPumpCalibration(HANumeric value, HANumber* sender) {
    // Znajdź indeks pompy na podstawie wskaźnika do kalibracji
    int pumpIndex = -1;
    for (int i = 0; i < NUM_PUMPS; i++) {
        if (sender == pumpCalibrations[i]) {
            pumpIndex = i;
            break;
        }
    }
    
    if (pumpIndex >= 0) {
        pumps[pumpIndex].calibration = value.toFloat();
        saveConfiguration();
    }
}

void onServiceModeSwitch(bool state, HASwitch* sender) {
    serviceMode = state;
    if (serviceMode) {
        stopAllPumps();
    }
    updateServiceModeLEDs();
}

// --- Inicjalizacja encji Home Assistant
void initHomeAssistant() {
    // Konfiguracja urządzenia
    haDevice.setName("AquaDoser");
    haDevice.setSoftwareVersion("1.0.0");
    haDevice.setManufacturer("DIY");
    haDevice.setModel("AquaDoser 8-channel");
    
    // Przełącznik trybu serwisowego
    serviceModeSwitch = new HASwitch("service_mode");
    serviceModeSwitch->setName("Tryb serwisowy");
    serviceModeSwitch->onCommand(onServiceModeSwitch);
    serviceModeSwitch->setIcon("mdi:wrench");
    
    // Tworzenie encji dla każdej pompy
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        char entityId[32];
        snprintf(entityId, sizeof(entityId), "pump_%d_state", i + 1);
        
        char name[32];
        snprintf(name, sizeof(name), "Pump %d State", i + 1);
        
        // Tworzymy nowy sensor i przypisujemy go do pola sensor w PumpState
        pumpStates[i].sensor = new HASensor(entityId);
        pumpStates[i].sensor->setName(name);
        pumpStates[i].sensor->setIcon("mdi:state-machine");
    }
}

// --- Aktualizacja stanów w Home Assistant
void updateHomeAssistant() {
    DateTime now = rtc.now();
    char currentTimeStr[32];
    
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumpStates[i].sensor != nullptr) {
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
            if (!pumpStates[i].isActive && config.pumps[i].enabled) {
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

// Pomocnicza funkcja do obliczania następnego dozowania
DateTime calculateNextDosing(uint8_t pumpIndex) {
    DateTime now = rtc.now();
    uint8_t currentHour = now.hour();
    uint8_t currentMinute = now.minute();
    uint8_t currentDay = now.dayOfTheWeek(); // 0 = Sunday, 6 = Saturday
    
    // Pobierz zaplanowaną godzinę dozowania z konfiguracji
    uint8_t schedHour = config.pumps[pumpIndex].schedule_hour;
    uint8_t schedMinute = config.pumps[pumpIndex].minute;
    uint8_t scheduleDays = config.pumps[pumpIndex].schedule_days;
    
    DateTime nextRun = now;
    int daysToAdd = 0;
    
    // Najpierw sprawdź, czy dzisiaj jest jeszcze możliwe dozowanie
    if (currentHour > schedHour || 
        (currentHour == schedHour && currentMinute >= schedMinute)) {
        // Jeśli czas dozowania na dziś już minął, zacznij sprawdzać od jutra
        daysToAdd = 1;
        currentDay = (currentDay + 1) % 7;
    }
    
    // Znajdź następny zaplanowany dzień
    int daysChecked = 0;
    while (daysChecked < 7) {
        if (scheduleDays & (1 << currentDay)) {
            // Znaleziono następny zaplanowany dzień
            break;
        }
        daysToAdd++;
        currentDay = (currentDay + 1) % 7;
        daysChecked++;
    }
    
    if (daysToAdd > 0) {
        nextRun = now + TimeSpan(daysToAdd, 0, 0, 0);
    }
    
    return DateTime(nextRun.year(), nextRun.month(), nextRun.day(), 
                   schedHour, schedMinute, 0);
}

void handleConfigSave() {
    String page = "";
    bool needRestart = false;

    if (server.hasArg("mqtt_broker")) {
        // Zapisz dane MQTT
        String mqtt_broker = server.arg("mqtt_broker");
        String mqtt_port = server.arg("mqtt_port");
        String mqtt_user = server.arg("mqtt_user");
        String mqtt_password = server.arg("mqtt_password");
        
        // Sprawdź, czy dane się zmieniły
        if (mqtt_broker != config.mqtt.broker || 
            mqtt_port.toInt() != config.mqtt.port ||
            mqtt_user != config.mqtt.username ||
            mqtt_password != config.mqtt.password) {
                
            // Kopiuj dane do konfiguracji
            strlcpy(config.mqtt.broker, mqtt_broker.c_str(), sizeof(config.mqtt.broker));
            config.mqtt.port = mqtt_port.toInt();
            strlcpy(config.mqtt.username, mqtt_user.c_str(), sizeof(config.mqtt.username));
            strlcpy(config.mqtt.password, mqtt_password.c_str(), sizeof(config.mqtt.password));
            
            needRestart = true;
        }
        
        // Zapisz konfigurację do EEPROM
        saveConfiguration();
        
        page = "Configuration saved.";
        if (needRestart) {
            page += " Device will restart in 3 seconds...";
            shouldRestart = true;
            restartTime = millis() + 3000;
        }
    }
    
    server.send(200, "text/html", page);
}

bool validateMQTTConfig() {
    // Sprawdź czy broker nie jest pusty
    if (strlen(config.mqtt.broker) == 0) {
        Serial.println("Brak konfiguracji MQTT - broker jest pusty");
        return false;
    }
    
    // Sprawdź czy port jest poprawny
    if (config.mqtt.port <= 0 || config.mqtt.port > 65535) {
        Serial.println("Brak konfiguracji MQTT - niepoprawny port");
        return false;
    }
    
    return true;
}

// --- Konfiguracja MQTT dla Home Assistant
void setupMQTT() {
    if (strlen(networkConfig.mqtt_server) == 0) {
        Serial.println("Brak konfiguracji MQTT");
        return;
    }
    
    mqtt.begin(
        networkConfig.mqtt_server,
        networkConfig.mqtt_port,
        networkConfig.mqtt_user,
        networkConfig.mqtt_password
    );
}

// --- Zmienne dla obsługi przycisku
unsigned long lastButtonPress = 0;
bool lastButtonState = HIGH;

// --- Obsługa przycisku
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

// --- Synchronizacja RTC z NTP
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

// --- Inicjalizacja WiFi

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

void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/update", HTTP_GET, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/html", getUpdatePage());
    });
    
    httpUpdateServer.setup(&server);
    server.begin();
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
}

// String getConfigPage() {
//     String page = F("<!DOCTYPE html><html><head>");
//     page += F("<meta charset='UTF-8'>");
//     page += F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
//     page += F("<title>AquaDoser Configuration</title>");
//     page += getStyles();
//     page += F("</head><body>");
//     page += F("<div class='container'>");
//     page += F("<h1>MQTT Configuration</h1>");
//     page += F("<form method='POST' action='/config'>");
    
//     // MQTT configuration
//     page += F("<div class='card'>");
//     page += F("<h2>MQTT Settings</h2>");
//     page += F("<div class='form-group'>");
//     page += F("<label>Broker:</label>");
//     page += F("<input type='text' name='mqtt_broker' value='");
//     page += config.mqtt.broker;
//     page += F("'></div>");
    
//     page += F("<div class='form-group'>");
//     page += F("<label>Port:</label>");
//     page += F("<input type='number' name='mqtt_port' value='");
//     page += String(config.mqtt.port);
//     page += F("'></div>");
    
//     page += F("<div class='form-group'>");
//     page += F("<label>Username:</label>");
//     page += F("<input type='text' name='mqtt_user' value='");
//     page += config.mqtt.username;
//     page += F("'></div>");
    
//     page += F("<div class='form-group'>");
//     page += F("<label>Password:</label>");
//     page += F("<input type='password' name='mqtt_password' value='");
//     page += config.mqtt.password;
//     page += F("'></div>");
//     page += F("</div>");
    
//     page += F("<button type='submit' class='button'>Save Configuration</button>");
//     page += F("</form>");
//     page += F("</div></body></html>");
    
//     return page;
// }

void checkMQTTConfig() {
    if (validateMQTTConfig()) {
        Serial.println("Konfiguracja MQTT znaleziona:");
        Serial.print("Broker: ");
        Serial.println(config.mqtt.broker);
        Serial.print("Port: ");
        Serial.println(config.mqtt.port);
        Serial.print("Username: ");
        Serial.println(config.mqtt.username);
    } else {
        Serial.println("Brak poprawnej konfiguracji MQTT");
    }
}

String getStyles() {
    return F("body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f5f5f5; }"
            ".container { max-width: 960px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
            "h1, h2 { color: #2196F3; }"
            "h1 { text-align: center; margin-bottom: 30px; }"
            ".section { background: #fff; border: 1px solid #ddd; padding: 20px; margin-bottom: 20px; border-radius: 4px; }"
            ".form-group { margin-bottom: 15px; }"
            "label { display: inline-block; margin-bottom: 5px; color: #666; }"
            "input[type='text'], input[type='number'], input[type='password'] { width: 200px; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }"
            "input[type='checkbox'] { margin-right: 5px; }"
            ".days-group { margin: 10px 0; }"
            ".days-group label { margin-right: 10px; }"
            ".button-group { text-align: center; margin: 20px 0; }"
            ".button-primary { background: #2196F3; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; margin: 5px; }"
            ".button-warning { background: #f44336; color: white; }"
            ".button-primary:hover { background: #1976D2; }"
            ".button-warning:hover { background: #d32f2f; }"
            ".status-item { display: inline-block; margin-right: 20px; }");
}

String getConfigPage() {
    String page;
    page += F("<!DOCTYPE html>");
    page += F("<html lang='en'>");
    page += F("<head>");
    page += F("<meta charset='UTF-8'>");
    page += F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
    page += F("<title>AquaDoser Configuration</title>");
    page += F("<style>");
    page += getStyles();  // Dodanie stylów CSS
    page += F("</style>");
    page += F("</head>");
    page += F("<body>");
    page += F("<div class='container'>");

    // Sekcja statusu systemu
    page += F("<div class='section'>");
    page += F("<h2>System Status</h2>");
    page += F("<div id='systemStatus'></div>");
    page += F("</div>");

    // Sekcja konfiguracji MQTT
    page += F("<div class='section'>");
    page += F("<h2>MQTT Configuration</h2>");
    page += F("<div class='form-group'>");
    page += F("<label>Server: </label>");
    page += F("<input type='text' id='mqtt_server' value='");
                    ffddczsccsd     page += String(mqttConfig.server);
    page += F("'></div>");
    page += F("<div class='form-group'>");
    page += F("<label>Port: </label>");
    page += F("<input type='number' id='mqtt_port' value='");
    page += String(mqttConfig.port);
    page += F("'></div>");
    page += F("<div class='form-group'>");
    page += F("<label>Username: </label>");
    page += F("<input type='text' id='mqtt_user' value='");
    page += String(mqttConfig.username);
    page += F("'></div>");
    page += F("<div class='form-group'>");
    page += F("<label>Password: </label>");
    page += F("<input type='password' id='mqtt_pass' value='");
    page += String(mqttConfig.password);
    page += F("'></div>");
    page += F("</div>");

    // Sekcje konfiguracji pomp
    for(int i = 0; i < NUM_PUMPS; i++) {
        page += F("<div class='section'>");
        page += F("<h2>Pump ");
        page += String(i + 1);
        page += F(" Configuration</h2>");
        page += F("<div class='form-group'>");
        page += F("<label>Name: </label>");
        page += F("<input type='text' id='pump_name_");
        page += String(i);
        page += F("' value='");
        page += String(config.pumps[i].name);
        page += F("'></div>");
        page += F("<div class='form-group'>");
        page += F("<label>Dose (ml): </label>");
        page += F("<input type='number' id='pump_dose_");
        page += String(i);
        page += F("' value='");
        page += String(config.pumps[i].doseAmount);
        page += F("' step='0.1'></div>");
        page += F("<div class='form-group'>");
        page += F("<label>Hour: </label>");
        page += F("<input type='number' id='pump_hour_");
        page += String(i);
        page += F("' value='");
        page += String(config.pumps[i].hour);
        page += F("' min='0' max='23'></div>");
        page += F("<div class='form-group'>");
        page += F("<label>Minute: </label>");
        page += F("<input type='number' id='pump_minute_");
        page += String(i);
        page += F("' value='");
        page += String(config.pumps[i].minute);
        page += F("' min='0' max='59'></div>");
        page += F("<div class='days-group'>");
        page += F("<label>Days: </label>");

        for(int j = 0; j < 7; j++) {
            page += F("<label><input type='checkbox' id='pump_day_");
            page += String(i);
            page += F("_");
            page += String(j);
            page += F("'");
            page += (config.pumps[i].days & (1 << j) ? F(" checked") : F(""));
            page += F(">");
            page += dayNames[j];
            page += F("</label>");
        }

        page += F("</div>");
        page += F("<div class='form-group'>");
        page += F("<label>Enabled: </label>");
        page += F("<input type='checkbox' id='pump_enabled_");
        page += String(i);
        page += (config.pumps[i].enabled ? F("' checked>") : F("'>"));
        page += F("</div></div>");
    }

    // Przyciski
    page += F("<div class='button-group'>");
    page += F("<button class='button-primary' onclick='saveConfig()'>Save Configuration</button>");
    page += F("<button class='button-primary button-warning' onclick='resetESP()'>Reset ESP</button>");
    page += F("</div>");

    // JavaScript
    page += F("<script>");
    page += F("let ws = new WebSocket('ws://' + window.location.hostname + ':81/');");
    page += F("ws.onmessage = function(event) {");
    page += F("    let data = JSON.parse(event.data);");
    page += F("    updateSystemStatus(data);");
    page += F("};");
    
    page += F("function updateSystemStatus(data) {");
    page += F("    let statusHtml = '';");
    page += F("    statusHtml += '<div class=\"status-item\">Uptime: ' + data.uptime + '</div>';");
    page += F("    statusHtml += '<div class=\"status-item\">MQTT: ' + (data.mqtt_connected ? 'Connected' : 'Disconnected') + '</div>';");
    page += F("    document.getElementById('systemStatus').innerHTML = statusHtml;");
    page += F("}");
    
    page += F("function saveConfig() {");
    page += F("    let config = {");
    page += F("        mqtt: {");
    page += F("            server: document.getElementById('mqtt_server').value,");
    page += F("            port: parseInt(document.getElementById('mqtt_port').value),");
    page += F("            username: document.getElementById('mqtt_user').value,");
    page += F("            password: document.getElementById('mqtt_pass').value");
    page += F("        },");
    page += F("        pumps: []");
    page += F("    };");
    
    page += F("    for(let i = 0; i < ");
    page += String(NUM_PUMPS);
    page += F("; i++) {");
    page += F("        let pump = {");
    page += F("            name: document.getElementById('pump_name_' + i).value,");
    page += F("            doseAmount: parseFloat(document.getElementById('pump_dose_' + i).value),");
    page += F("            hour: parseInt(document.getElementById('pump_hour_' + i).value),");
    page += F("            minute: parseInt(document.getElementById('pump_minute_' + i).value),");
    page += F("            days: 0,");
    page += F("            enabled: document.getElementById('pump_enabled_' + i).checked");
    page += F("        };");
    page += F("        for(let j = 0; j < 7; j++) {");
    page += F("            if(document.getElementById('pump_day_' + i + '_' + j).checked) {");
    page += F("                pump.days |= (1 << j);");
    page += F("            }");
    page += F("        }");
    page += F("        config.pumps.push(pump);");
    page += F("    }");
    
    page += F("    fetch('/save', {");
    page += F("        method: 'POST',");
    page += F("        headers: {'Content-Type': 'application/json'},");
    page += F("        body: JSON.stringify(config)");
    page += F("    })");
    page += F("    .then(response => response.text())");
    page += F("    .then(data => alert(data));");
    page += F("}");
    
    page += F("function resetESP() {");
    page += F("    if(confirm('Are you sure you want to reset the ESP?')) {");
    page += F("        fetch('/reset')");
    page += F("        .then(response => response.text())");
    page += F("        .then(data => alert(data));");
    page += F("    }");
    page += F("}");
    page += F("</script>");

    // Zakończenie strony
    page += F("</div></body></html>");
    
    return page;
}

void resetFactorySettings() {
    // Usuń wszystkie pliki konfiguracyjne
    LittleFS.remove("/config.json");
    LittleFS.remove("/mqtt.json");
    
    // Resetuj struktury danych do wartości domyślnych
    for (uint8_t i = 0; i < NUM_PUMPS; i++) {
        pumps[i].enabled = false;
        pumps[i].calibration = 1.0;
        pumps[i].dose = 0.0;
        pumps[i].schedule_hour = 0;
        pumps[i].schedule_days = 0;
        pumps[i].lastDosing = 0;
        pumps[i].isRunning = false;
    }
    
    // Resetuj ustawienia MQTT
    mqttEnabled = false;
    mqttServer[0] = '\0';
    mqttPort = 1883;
    mqttUser[0] = '\0';
    mqttPassword[0] = '\0';
    
    // Zapisz domyślne ustawienia
    saveConfiguration();
    saveMQTTConfig();
}

// --- Setup
void setup() {
    Serial.begin(115200);
    Serial.println("\nAquaDoser Start");
    Wire.begin();
    
    // Inicjalizacja pinów
    pinMode(BUTTON_PIN, INPUT_PULLUP);

        pinMode(BUZZER_PIN, OUTPUT);  // Wyjście - buzzer
    digitalWrite(BUZZER_PIN, LOW);  // Wyłączenie buzzera
    
    // Inicjalizacja systemu plików i wczytanie konfiguracji
    if (!initFileSystem()) {
        Serial.println("Błąd inicjalizacji systemu plików!");
    }

    loadConfiguration();  // Wczytaj konfigurację z EEPROM
    checkMQTTConfig();   // Sprawdź i wyświetl status konfiguracji MQTT

    // Inicjalizacja systemu plików
    if (!LittleFS.begin()) {
        Serial.println("Błąd inicjalizacji LittleFS!");
        return;
    }

        if (LittleFS.begin()) {
        loadConfiguration();
        loadMQTTConfig();  // Dodaj to
    }
    
    // Inicjalizacja RTC
    if (!rtc.begin()) {
        Serial.println("Nie znaleziono RTC!");
    }
    
    // Inicjalizacja PCF8574
    if (!pcf8574.begin()) {
        Serial.println("Nie znaleziono PCF8574!");
    }
    
    initializeLEDs();
    
    // Konfiguracja WiFi i MQTT
    setupWiFi();

    setupWebServer();
    
    // Inicjalizacja Home Assistant
    initHomeAssistant();
    setupMQTT();
    
    // Synchronizacja czasu
    syncRTC();
    
    server.on("/restart", HTTP_POST, []() {
        server.send(200, "text/plain", "Restarting...");
        delay(1000);
        ESP.restart();
    });

    server.on("/factory-reset", HTTP_POST, []() {
        server.send(200, "text/plain", "Resetting to factory defaults...");
        resetFactorySettings();  // Funkcja do zaimplementowania
        delay(1000);
        ESP.restart();
    });

    Serial.println("Inicjalizacja zakończona");
}

#ifdef TEST_MODE
void simulateButton(uint8_t buttonPin) {
    // Symuluj naciśnięcie przycisku
    pcf.currentState &= ~(1 << buttonPin);
    handleButton();
    delay(50);
    // Symuluj zwolnienie przycisku
    pcf.currentState |= (1 << buttonPin);
    handleButton();
}
#endif

// --- Loop
void loop() {
    server.handleClient();
    unsigned long currentMillis = millis();
    
    // Jednolity system wyświetlania statusu
    if (firstRun || (currentMillis - lastLogTime >= LOG_INTERVAL)) {
        printLogHeader();
        
        bool hasActivePumps = false;
        for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
            if (config.pumps[i].enabled) {
                printPumpStatus(i);
                hasActivePumps = true;
            }
        }
        
        if (!hasActivePumps) {
            Serial.println(F("No active pumps configured"));
        }
        
        Serial.println(F("========================================\n"));
        
        lastLogTime = currentMillis;
        firstRun = false;
    }

    // Sprawdzanie systemu co 5 sekund
    static unsigned long lastCheck = 0;
    if (currentMillis - lastCheck > 5000) {
        lastCheck = currentMillis;
        //Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
        //Serial.printf("WiFi Status: %d\n", WiFi.status());
    }

#ifdef TEST_MODE
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        if (cmd.startsWith("button")) {
            int buttonNum = cmd.substring(6).toInt();
            if (buttonNum >= 1 && buttonNum <= 4) {
                DEBUG_SERIAL("Symulacja przycisku " + String(buttonNum));
                simulateButton(buttonNum - 1);
            }
        }
        else if (cmd == "status") {
            DateTime now = rtc.now();
            DEBUG_SERIAL("Czas: " + String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second()));
            DEBUG_SERIAL("Dzień: " + String(now.dayOfTheWeek()));
            
            for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
                DEBUG_SERIAL("Pompa " + String(i + 1) + ":");
                DEBUG_SERIAL("  Enabled: " + String(config.pumps[i].enabled));
                DEBUG_SERIAL("  Calibration: " + String(config.pumps[i].calibration));
                DEBUG_SERIAL("  Dose: " + String(config.pumps[i].dose));
                DEBUG_SERIAL("  Schedule Hour: " + String(config.pumps[i].schedule_hour));
                DEBUG_SERIAL("  Schedule Days: " + String(config.pumps[i].schedule_days, BIN));
                DEBUG_SERIAL("  Is Running: " + String(config.pumps[i].isRunning));
            }
        }
        else if (cmd == "help") {
            DEBUG_SERIAL("Dostępne komendy:");
            DEBUG_SERIAL("button X - symuluj naciśnięcie przycisku X (1-4)");
            DEBUG_SERIAL("status - pokaż aktualny stan urządzenia");
            DEBUG_SERIAL("help - pokaż tę pomoc");
        }
    }
#endif

    // Obsługa połączenia WiFi
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Utracono połączenie WiFi");
        WiFi.reconnect();
        delay(5000);
        return;
    }
    
    // Obsługa systemowa
    mqtt.loop();
    handleButton();
    handlePumps();
    updateLEDs();
    updateHomeAssistant();
    
    // Synchronizacja RTC co 24h
    static unsigned long lastRtcSync = 0;
    if (currentMillis - lastRtcSync >= 24*60*60*1000UL) {
        syncRTC();
        lastRtcSync = currentMillis;
    }
    
    yield();
}
