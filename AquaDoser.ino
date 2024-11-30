/*
AquaDoser - dozowanie nawozów
HydroSense - dolewanie wody
LumaSense - oświetlenie
ClimaCore - sterowanie temperaturą/CO2
GasSense - monitoring butli CO2
*/

// --- Biblioteki

// Podstawowe biblioteki systemowe
#include <Arduino.h>     // Podstawowa biblioteka Arduino - zawiera główne funkcje i definicje dla platformy Arduino
#include <ArduinoHA.h>   // Integracja z Home Assistant poprzez MQTT - umożliwia komunikację z systemem automatyki domowej
#include <ArduinoOTA.h>  // Over The Air - umożliwia bezprzewodową aktualizację oprogramowania przez WiFi

// Biblioteki do obsługi WiFi i komunikacji sieciowej
#include <ESP8266WiFi.h>              // Obsługa WiFi dla układu ESP8266
#include <WiFiManager.h>              // Zarządzanie konfiguracją WiFi - portal konfiguracyjny do łatwego podłączenia do sieci
#include <ESP8266WebServer.h>         // Serwer HTTP dla ESP8266 - umożliwia hostowanie strony konfiguracyjnej
#include <WebSocketsServer.h>         // Obsługa WebSocket - do komunikacji w czasie rzeczywistym
#include <ESP8266HTTPUpdateServer.h>  // Serwer aktualizacji HTTP - umożliwia aktualizację firmware przez przeglądarkę

// Biblioteki do obsługi pamięci i plików
#include <EEPROM.h>    // Dostęp do pamięci EEPROM - przechowywanie konfiguracji

// Biblioteki do komunikacji z urządzeniami I2C
#include <Wire.h>     // Obsługa magistrali I2C
#include <RTClib.h>   // Obsługa zegara czasu rzeczywistego (RTC)
#include <PCF8574.h>  // Sterownik ekspandera I/O I2C PCF8574

// Biblioteki do obsługi JSON i LED
#include <ArduinoJson.h>        // Parsowanie i generowanie danych w formacie JSON
#include <Adafruit_NeoPixel.h>  // Sterowanie diodami LED WS2812B (NeoPixel)

// Biblioteki do synchronizacji czasu
#include <NTPClient.h>  // Klient NTP - synchronizacja czasu z serwerami czasu
#include <WiFiUDP.h>    // Obsługa protokołu UDP - wymagana dla NTP

// Deklaracja wyprzedzająca
class HASensor;

// Stałe
const int NUMBER_OF_PUMPS = 8;
const int BUZZER_PIN = 13;    // GPIO 13
const int LED_PIN = 12;    // GPIO 12
const int BUTTON_PIN = 14;    // GPIO 14
const int SDA_PIN = 4;        // GPIO 4
const int SCL_PIN = 5;        // GPIO 5
const int PCF8574_ADDRESS = 0x20;

#define PULSE_MAX_BRIGHTNESS 255
#define PULSE_MIN_BRIGHTNESS 50

// Definicje kolorów
#define COLOR_RAINBOW_1 strip.Color(255, 0, 0)      // Czerwony
#define COLOR_RAINBOW_2 strip.Color(255, 127, 0)    // Pomarańczowy
#define COLOR_RAINBOW_3 strip.Color(255, 255, 0)    // Żółty
#define COLOR_RAINBOW_4 strip.Color(0, 255, 0)      // Zielony
#define COLOR_RAINBOW_5 strip.Color(0, 0, 255)      // Niebieski
#define COLOR_RAINBOW_6 strip.Color(139, 0, 255)    // Fioletowy

#define MQTT_SERVER_LENGTH 40
#define MQTT_USER_LENGTH 20
#define MQTT_PASSWORD_LENGTH 20

#define LOG_INTERVAL 60000           // 1 minuta
#define SYSTEM_CHECK_INTERVAL 5000   // 5 sekund
#define WEBSOCKET_UPDATE_INTERVAL 1000 // 1 sekunda
#define RTC_SYNC_INTERVAL 86400000UL // 24 godziny

// Definicje struktur
struct PumpState {
    bool isActive;
    unsigned long lastDoseTime;
    HASensor* sensor;
    
    PumpState() : isActive(false), lastDoseTime(0), sensor(nullptr) {}
};

struct MQTTConfig {
    char broker[64];
    int port;
    char username[32];
    char password[32];
    bool enabled;
};

struct PumpConfig {
    char name[20];
    bool enabled;
    float calibration;
    float dose;
    uint8_t schedule_hour;
    uint8_t minute;
    uint8_t schedule_days;
    time_t lastDosing;
    bool isRunning;
};

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

struct SystemConfig {
    bool soundEnabled;
};

struct SystemStatus {
    bool mqtt_connected;
    bool wifi_connected;
    unsigned long uptime;
};

struct SystemInfo {
    unsigned long uptime;
    bool mqtt_connected;
};

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

// Adresy EEPROM
const int PUMPS_CONFIG_ADDR = 0;
const int MQTT_CONFIG_ADDR = PUMPS_CONFIG_ADDR + (sizeof(PumpConfig) * NUMBER_OF_PUMPS);
const int NETWORK_CONFIG_ADDR = MQTT_CONFIG_ADDR + sizeof(MQTTConfig);
const int SYSTEM_CONFIG_ADDR = NETWORK_CONFIG_ADDR + sizeof(NetworkConfig);
const int SYSTEM_STATUS_ADDR = SYSTEM_CONFIG_ADDR + sizeof(SystemConfig);

// Globalne obiekty
WiFiClient client;
HADevice device("aquadoser");
HAMqtt mqtt(client, device);
PCF8574 pcf(PCF8574_ADDRESS);
ESP8266WebServer server(80);
WebSocketsServer webSocket(81);
Adafruit_NeoPixel strip(NUMBER_OF_PUMPS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Globalne zmienne stanu
PumpConfig pumps[NUMBER_OF_PUMPS];
PumpState pumpStates[NUMBER_OF_PUMPS];
NetworkConfig networkConfig;
SystemConfig systemConfig;
SystemStatus systemStatus;
MQTTConfig mqttConfig;
LEDState ledStates[NUMBER_OF_PUMPS];

// Zmienne globalne do obsługi czasu
DateTime now;
uint8_t currentDay;
uint8_t currentHour;
uint8_t currentMinute;
bool hasActivePumps = false;

// --- Zmienne dla obsługi przycisku
unsigned long lastButtonPress = 0;
bool lastButtonState = HIGH;

// --- Interwały czasowe
const unsigned long MQTT_LOOP_INTERVAL = 100;      // Obsługa MQTT co 100ms
const unsigned long OTA_CHECK_INTERVAL = 1000;     // Sprawdzanie OTA co 1s
const unsigned long PUMP_CHECK_INTERVAL = 1000;    // Sprawdzanie pomp co 1s
const unsigned long BUTTON_DEBOUNCE_TIME = 50;     // Czas debounce przycisku (ms)
const unsigned long WIFI_RECONNECT_DELAY = 5000;   // Opóźnienie ponownego połączenia WiFi
const unsigned long MQTT_RECONNECT_DELAY = 5000;   // Opóźnienie ponownego połączenia MQTT

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
//const unsigned long LOG_INTERVAL = 60000; // 60 sekund między wyświetlaniem statusu
unsigned long lastLogTime = 0;
bool firstRun = true;
const char* dayNames[] = {"Pn", "Wt", "Śr", "Cz", "Pt", "Sb", "Nd"};

#define DEBOUNCE_TIME 50   // Czas debounce w ms

// --- Kolory LED
#define COLOR_OFF 0xFF0000      // Czerwony (pompa wyłączona)
#define COLOR_ON 0x00FF00       // Zielony (pompa włączona)
#define COLOR_WORKING 0x0000FF  // Niebieski (pompa pracuje)
#define COLOR_SERVICE 0xFFFF00  // Żółty (tryb serwisowy)

#define DEBUG_SERIAL(x)
RTC_DS3231 rtc;

ESP8266HTTPUpdateServer httpUpdateServer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// --- Globalne obiekty
WiFiClient wifiClient;
HADevice haDevice("AquaDoser");
//HAMqtt mqtt(wifiClient, haDevice);
//RTC_DS3231 rtc;
PCF8574 pcf8574(0x20);

// --- Globalne zmienne stanu
bool serviceMode = false;
bool pumpRunning[NUMBER_OF_PUMPS] = {false};
unsigned long doseStartTime[NUMBER_OF_PUMPS] = {0};

// Dodaj stałe dla timeoutów i interwałów jak w HydroSense
const unsigned long MQTT_RETRY_INTERVAL = 10000;   
const unsigned long MILLIS_OVERFLOW_THRESHOLD = 4294967295U - 60000; // ~49.7 dni

unsigned long lastMQTTLoop = 0;
unsigned long lastMeasurement = 0;
unsigned long lastOTACheck = 0;

// --- Stałe czasowe
#define DOSE_CHECK_INTERVAL 1000    // Sprawdzanie dozowania co 1 sekundę
#define MIN_DOSE_TIME 100          // Minimalny czas dozowania (ms)
#define MAX_DOSE_TIME 60000        // Maksymalny czas dozowania (60 sekund)

// --- Zmienne dla obsługi pomp
unsigned long lastDoseCheck = 0;
bool pumpInitialized = false;
// --- Stałe dla animacji LED
#define LED_UPDATE_INTERVAL 50    // Aktualizacja LED co 50ms
#define FADE_STEPS 20            // Liczba kroków w animacji fade

unsigned long lastLedUpdate = 0;
// --- Stałe dla Home Assistant
#define HA_UPDATE_INTERVAL 30000  // Aktualizacja HA co 30 sekund
unsigned long lastHaUpdate = 0;

// --- Deklaracje encji Home Assistant
HASwitch* pumpSwitches[NUMBER_OF_PUMPS];
HANumber* pumpCalibrations[NUMBER_OF_PUMPS];
HASwitch* serviceModeSwitch;

SystemInfo sysInfo = {0, false};

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
void saveConfiguration();
void loadConfiguration();

// --- Funkcje obsługi EEPROM
// void initStorage() {   
//     // Inicjalizacja struktury pomp
//     for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
//         pumps[i].enabled = false;
//         pumps[i].calibration = 1.0;
//         pumps[i].dose = 0.0;
//         pumps[i].schedule_hour = 0;
//         pumps[i].schedule_days = 0;
//         pumps[i].minute = 0;
//         pumps[i].lastDosing = 0;
//         pumps[i].isRunning = false;
//         strncpy(pumps[i].name, ("Pump " + String(i+1)).c_str(), 19);
//         pumps[i].name[19] = '\0';
//     }
    
//     // Inicjalizacja konfiguracji sieciowej
//     networkConfig.dhcp = true;
//     strncpy(networkConfig.hostname, "aquadoser", 31);
//     networkConfig.hostname[31] = '\0';
//     networkConfig.mqtt_port = 1883;
//     networkConfig.mqtt_server[0] = '\0';
//     networkConfig.mqtt_user[0] = '\0';
//     networkConfig.mqtt_password[0] = '\0';
    
//     // Inicjalizacja statusu systemu
//     systemStatus.mqtt_connected = false;
//     systemStatus.wifi_connected = false;
//     systemStatus.uptime = 0;
    
//     // Zapisz domyślną konfigurację
//     saveConfiguration();
// }

void initStorage() {
    systemConfig.soundEnabled = true;  // wartość domyślna
}

// --- Konfiguracja MQTT dla Home Assistant
void setupMQTT() {
    if (strlen(networkConfig.mqtt_server) == 0) {
        Serial.println(F("Brak konfiguracji MQTT"));
        return;
    }

    if (!mqttConfig.enabled) return;
    
    mqtt.onConnected([]() {
        systemStatus.mqtt_connected = true;
        Serial.println("MQTT Connected");
    });
    
    mqtt.onDisconnected([]() {
        systemStatus.mqtt_connected = false;
        Serial.println("MQTT Disconnected");
    });

    // Konfiguracja urządzenia HA
    byte mac[6];
    WiFi.macAddress(mac);
    device.setUniqueId(mac, sizeof(mac));
    device.setName("AquaDoser");
    device.setSoftwareVersion("1.0.0");
    
    // Konfiguracja MQTT
    mqtt.begin(
        networkConfig.mqtt_server,
        networkConfig.mqtt_port,
        networkConfig.mqtt_user,
        networkConfig.mqtt_password
    );
}

bool hasIntervalPassed(unsigned long current, unsigned long previous, unsigned long interval) {
    return (current - previous >= interval);
}

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
    if (!pumps[pumpIndex].enabled) {
        return; // Nie wyświetlaj statusu wyłączonych pomp
    }

    Serial.print(F("Pump "));
    Serial.print(pumpIndex + 1);
    Serial.print(F(": "));

    if (pumps[pumpIndex].isRunning) {
        Serial.println(F("Pompa pracuje"));
    } else {
        Serial.print(F("Standby - Next dose: "));
        DateTime nextDose = calculateNextDosing(pumpIndex);
        Serial.print(formatDateTime(nextDose));
        
        if (pumps[pumpIndex].lastDosing > 0) {
            Serial.print(F(" (Last: "));
            time_t lastDose = pumps[pumpIndex].lastDosing;
            DateTime lastDoseTime(lastDose);
            Serial.print(formatDateTime(lastDoseTime));
            Serial.print(F(")"));
        }
        Serial.println();
    }
}

void printPumpStatus(int pumpIndex) {
    if (!pumps[pumpIndex].enabled) {
        Serial.print(F("Pump "));
        Serial.print(pumpIndex + 1);
        Serial.println(F(": Disabled"));
        return;
    }

    String status = "Pump " + String(pumpIndex + 1) + ": ";
    
    if (pumps[pumpIndex].isRunning) {
        status += "Active (Dosing)";
    } else {
        if (pumps[pumpIndex].lastDosing > 0) {
            time_t lastDose = pumps[pumpIndex].lastDosing;
            struct tm* timeinfo = localtime(&lastDose);
            char timeStr[20];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", timeinfo);
            status += "Inactive (Last: " + String(timeStr) + ")";
        } else {
            status += "Inactive (No previous runs)";
        }

        // Dodaj informację o następnym zaplanowanym dozowaniu
        if (pumps[pumpIndex].enabled) {
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

// Dodaj obsługę dźwięku jak w HydroSense
void playShortWarningSound() {
    if (!systemConfig.soundEnabled) return;
    tone(BUZZER_PIN, 2000, 100);
}

void playConfirmationSound() {
    if (!systemConfig.soundEnabled) return;
    tone(BUZZER_PIN, 1000, 50);
    delay(100);
    tone(BUZZER_PIN, 2000, 50);
}

void welcomeMelody() {
    if (!systemConfig.soundEnabled) return;
    tone(BUZZER_PIN, 1000, 100);
    delay(150);
    tone(BUZZER_PIN, 1500, 100);
    delay(150);
    tone(BUZZER_PIN, 2000, 100);
}

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
    
    // Efekt 3: Pulsowanie wszystkich diod (1.5s)
    for(int j = 0; j < 3; j++) {  // Trzy pulsy
        // Rozjaśnianie
        for(int b = 0; b < 255; b += 5) {
            for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
                strip.setPixelColor(i, strip.Color(b, b, b));
            }
            strip.show();
            delay(2);
        }
        // Ściemnianie
        for(int b = 255; b >= 0; b -= 5) {
            for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
                strip.setPixelColor(i, strip.Color(b, b, b));
            }
            strip.show();
            delay(2);
        }
    }
}

// Dodaj walidację konfiguracji
bool validateConfigValues() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumps[i].calibration <= 0 || pumps[i].calibration > 10)
            return false;
        if (pumps[i].dose < 0 || pumps[i].dose > 1000)
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
    
    doc["mqtt_connected"] = mqtt.isConnected(); // Użyj faktycznego stanu połączenia
    doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
    doc["uptime"] = millis() / 1000; // Czas w sekundach
    
    String output;
    serializeJson(doc, output);
    return output;
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

// Funkcje specyficzne dla różnych typów konfiguracji
void saveMQTTConfig() {
    mqttConfig.broker[sizeof(mqttConfig.broker) - 1] = '\0';
    mqttConfig.username[sizeof(mqttConfig.username) - 1] = '\0';
    mqttConfig.password[sizeof(mqttConfig.password) - 1] = '\0';
    EEPROM.put(MQTT_CONFIG_ADDR, mqttConfig);
    EEPROM.commit();
}

// --- Ładowanie konfiguracji pomp
bool loadPumpsConfig() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        EEPROM.get(PUMPS_CONFIG_ADDR + (i * sizeof(PumpConfig)), pumps[i]);
    }
    return true;
}

// --- Zapisywanie konfiguracji pomp
bool savePumpsConfig() {
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        EEPROM.put(PUMPS_CONFIG_ADDR + (i * sizeof(PumpConfig)), pumps[i]);
    }
    return EEPROM.commit();
}

// --- Ładowanie konfiguracji sieciowej
bool loadNetworkConfig() {
    EEPROM.get(NETWORK_CONFIG_ADDR, networkConfig);
    return true;
}

// --- Zapisywanie konfiguracji sieciowej
bool saveNetworkConfig() {
    EEPROM.put(NETWORK_CONFIG_ADDR, networkConfig);
    return EEPROM.commit();
}

// --- Ogólna funkcja ładowania konfiguracji
void loadConfiguration() {
    // Wczytaj wszystkie konfiguracje
    loadMQTTConfig();
    loadPumpsConfig();
    loadNetworkConfig();
    EEPROM.get(SYSTEM_CONFIG_ADDR, systemConfig);
    
    // Sprawdź poprawność danych
    if (!validateConfigValues()) {
        resetFactorySettings();
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

// void loadConfig() {
//     EEPROM.get(PUMPS_CONFIG_ADDR, pumps);
//     EEPROM.get(MQTT_CONFIG_ADDR, mqttConfig);
//     EEPROM.get(NETWORK_CONFIG_ADDR, networkConfig);
//     EEPROM.get(SYSTEM_CONFIG_ADDR, systemConfig);
//     EEPROM.get(SYSTEM_STATUS_ADDR, systemStatus);
// }

// Funkcja zapisująca konfigurację
// void saveConfig() {
//     EEPROM.put(PUMPS_CONFIG_ADDR, pumps);
//     EEPROM.put(MQTT_CONFIG_ADDR, mqttConfig);
//     EEPROM.put(NETWORK_CONFIG_ADDR, networkConfig);
//     EEPROM.put(SYSTEM_CONFIG_ADDR, systemConfig);
//     EEPROM.put(SYSTEM_STATUS_ADDR, systemStatus);
//     EEPROM.commit();
// }

// --- Ogólna funkcja zapisywania konfiguracji
void saveConfiguration() {
    saveMQTTConfig();
    savePumpsConfig();
    saveNetworkConfig();
    EEPROM.put(SYSTEM_CONFIG_ADDR, systemConfig);
    EEPROM.commit();
}

// --- Inicjalizacja PCF8574 i pomp
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

void loadMQTTConfig() {
    EEPROM.get(MQTT_CONFIG_ADDR, mqttConfig);
    if (!validateMQTTConfig()) {
        mqttConfig.port = 1883;
        mqttConfig.enabled = false;
        mqttConfig.broker[0] = '\0';
        mqttConfig.username[0] = '\0';
        mqttConfig.password[0] = '\0';
        saveMQTTConfig();
    }
}

// --- Rozpoczęcie dozowania dla pompy
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

// --- Zatrzymanie dozowania dla pompy
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

// --- Obsługa trybu serwisowego dla pomp
void servicePump(uint8_t pumpIndex, bool state) {
    if (!pumpInitialized || pumpIndex >= NUMBER_OF_PUMPS || !serviceMode) {
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
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (pumpRunning[i]) {
            stopPump(i);
        }
    }
}

// --- Inicjalizacja LED
void initializeLEDs() {
    strip.begin();
    strip.show();
    
    for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
        ledStates[i] = LEDState();  // Używamy konstruktora domyślnego
    }
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
    if (index >= NUMBER_OF_PUMPS) return;
    
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

// --- Aktualizacja LED dla trybu serwisowego
void updateServiceModeLEDs() {
    uint32_t color = serviceMode ? COLOR_SERVICE : COLOR_OFF;
    
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (!pumpRunning[i]) {
            setLEDColor(i, color, serviceMode);
        }
    }
}

// --- Aktualizacja LED dla pompy
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

// --- Aktualizacja wszystkich LED dla pomp
void updateAllPumpLEDs() {
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        updatePumpLED(i);
    }
}

// --- Funkcje callback dla Home Assistant
void onPumpSwitch(bool state, HASwitch* sender) {
    // Znajdź indeks pompy na podstawie wskaźnika do przełącznika
    int pumpIndex = -1;
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
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
    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
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
    char entityId[32];
    for (uint8_t i = 0; i < NUMBER_OF_PUMPS; i++) {
        snprintf(entityId, sizeof(entityId), "pump_%d", i);
        pumpStates[i].sensor = new HASensor(entityId); // Teraz używamy HASensor
        pumpStates[i].sensor->setName(pumps[i].name);
        pumpStates[i].sensor->setIcon("mdi:water-pump");
    }
}

// --- Aktualizacja stanów w Home Assistant
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

// Pomocnicza funkcja do obliczania następnego dozowania
DateTime calculateNextDosing(uint8_t pumpIndex) {
    DateTime now = rtc.now();
    currentDay = now.dayOfTheWeek();
    currentHour = now.hour();
    currentMinute = now.minute();
    
    uint8_t schedHour = pumps[pumpIndex].schedule_hour;
    uint8_t schedMinute = pumps[pumpIndex].minute;
    uint8_t scheduleDays = pumps[pumpIndex].schedule_days;
    
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
    if (server.hasArg("mqtt_broker") && server.hasArg("mqtt_port")) {
        String mqtt_broker = server.arg("mqtt_broker");
        String mqtt_port = server.arg("mqtt_port");
        String mqtt_user = server.arg("mqtt_user");
        String mqtt_password = server.arg("mqtt_password");
        
        // Zapisz do konfiguracji MQTT
        strlcpy(mqttConfig.broker, mqtt_broker.c_str(), sizeof(mqttConfig.broker));
        mqttConfig.port = mqtt_port.toInt();
        strlcpy(mqttConfig.username, mqtt_user.c_str(), sizeof(mqttConfig.username));
        strlcpy(mqttConfig.password, mqtt_password.c_str(), sizeof(mqttConfig.password));
        
        // Zapisz do EEPROM
        saveMQTTConfig();
        
        server.send(200, "text/plain", "Konfiguracja zapisana");
    } else {
        server.send(400, "text/plain", "Brak wymaganych parametrów");
    }
}

bool validateMQTTConfig() {
    if (strlen(networkConfig.mqtt_server) == 0) {
        return false;
    }
    if (networkConfig.mqtt_port <= 0 || networkConfig.mqtt_port > 65535) {
        return false;
    }
    return true;
}

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

void checkMQTTConfig() {
    Serial.print(F("MQTT Broker: "));
    Serial.println(networkConfig.mqtt_server);
    Serial.print(F("MQTT Port: "));
    Serial.println(networkConfig.mqtt_port);
}

String getStyles() {
    String styles = F(
        "body { "
        "    font-family: Arial, sans-serif; "
        "    margin: 0; "
        "    padding: 20px; "
        "    background-color: #1a1a1a;"
        "    color: #ffffff;"
        "}"

        ".buttons-container {"
        "    display: flex;"
        "    justify-content: space-between;"
        "    margin: -5px;"
        "}"

        ".container { "
        "    max-width: 800px; "
        "    margin: 0 auto; "
        "    padding: 0 15px;"
        "}"

        ".section {"
        "    background-color: #2a2a2a;"
        "    padding: 20px;"
        "    margin-bottom: 20px;"
        "    border-radius: 8px;"
        "    width: 100%;"
        "    box-sizing: border-box;"
        "}"

        "h1 { "
        "    color: #ffffff; "
        "    text-align: center;"
        "    margin-bottom: 30px;"
        "    font-size: 2.5em;"
        "    background-color: #2d2d2d;"
        "    padding: 20px;"
        "    border-radius: 8px;"
        "    box-shadow: 0 2px 4px rgba(0,0,0,0.2);"
        "}"

        "h2 { "
        "    color: #2196F3;"
        "    margin-top: 0;"
        "    font-size: 1.5em;"
        "}"

        ".config-table {"
        "    width: 100%;"
        "    border-collapse: collapse;"
        "    table-layout: fixed;"
        "}"

        ".config-table td {"
        "    padding: 8px;"
        "    border-bottom: 1px solid #3d3d3d;"
        "}"

        ".config-table td:first-child {"
        "    width: 65%;"
        "}"

        ".config-table td:last-child {"
        "    width: 35%;"
        "}"

        "input[type='text'],"
        "input[type='password'],"
        "input[type='number'] {"
        "    width: 100%;"
        "    padding: 8px;"
        "    border: 1px solid #3d3d3d;"
        "    border-radius: 4px;"
        "    background-color: #1a1a1a;"
        "    color: #ffffff;"
        "    box-sizing: border-box;"
        "    text-align: left;"
        "}"

        "input[type='checkbox'] {"
        "    width: 20px;"
        "    height: 20px;"
        "    margin: 0;"
        "    vertical-align: middle;"
        "}"

        ".btn {"
        "    padding: 12px 24px;"
        "    border: none;"
        "    border-radius: 4px;"
        "    cursor: pointer;"
        "    font-size: 14px;"
        "    width: calc(50% - 10px);"
        "    display: inline-block;"
        "    margin: 5px;"
        "    text-align: center;"
        "}"

        ".btn-blue { "
        "    background-color: #2196F3;"
        "    color: white; "
        "}"

        ".btn-red { "
        "    background-color: #F44336;"
        "    color: white; "
        "}"

        ".status {"
        "    padding: 4px 8px;"
        "    border-radius: 4px;"
        "    display: inline-block;"
        "}"

        ".success { "
        "    color: #4CAF50; "
        "}"

        ".error { "
        "    color: #F44336;"
        "}"

        ".message {"
        "    position: fixed;"
        "    top: 20px;"
        "    left: 50%;"
        "    transform: translateX(-50%);"
        "    padding: 15px 30px;"
        "    border-radius: 5px;"
        "    color: white;"
        "    opacity: 0;"
        "    transition: opacity 0.3s ease-in-out;"
        "    z-index: 1000;"
        "}"

        "@media (max-width: 600px) {"
        "    body { padding: 10px; }"
        "    .container { padding: 0; }"
        "    .section { padding: 15px; margin-bottom: 15px; }"
        "    .config-table td:first-child { width: 50%; }"
        "    .config-table td:last-child { width: 50%; }"
        "    .btn { width: 100%; margin: 5px 0; }"
        "    .buttons-container { flex-direction: column; }"
        "}"
    );
    return styles;
}

// String getConfigPage() {
//     String page = F("<!DOCTYPE html><html lang='pl'><head>");
//     page += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
//     page += F("<title>AquaDoser</title><style>");
//     page += getStyles();
//     page += F("</style>");
    
//     // Dodajemy skrypt JavaScript
//     page += F("<script>");
//     page += F("function saveConfig() {");
//     page += F("    let pumpsData = [];");
//     page += F("    for(let i = 0; i < ");
//     page += String(NUMBER_OF_PUMPS);
//     page += F("; i++) {");
//     page += F("        let pumpData = {");
//     page += F("            name: document.querySelector(`input[name='pump_name_${i}']`).value,");
//     page += F("            enabled: document.querySelector(`input[name='pump_enabled_${i}']`).checked,");
//     page += F("            calibration: parseFloat(document.querySelector(`input[name='pump_calibration_${i}']`).value),");
//     page += F("            dose: parseFloat(document.querySelector(`input[name='pump_dose_${i}']`).value),");
//     page += F("            schedule_hour: parseInt(document.querySelector(`input[name='pump_schedule_hour_${i}']`).value),");
//     page += F("            minute: parseInt(document.querySelector(`input[name='pump_minute_${i}']`).value),");
//     page += F("            schedule_days: 0");
//     page += F("        };");
//     page += F("        // Oblicz dni tygodnia");
//     page += F("        for(let day = 0; day < 7; day++) {");
//     page += F("            if(document.querySelector(`input[name='pump_day_${i}_${day}']`).checked) {");
//     page += F("                pumpData.schedule_days |= (1 << day);");
//     page += F("            }");
//     page += F("        }");
//     page += F("        pumpsData.push(pumpData);");
//     page += F("    }");
//     page += F("    let mqttData = {");
//     page += F("        broker: document.querySelector('input[name=\"mqtt_broker\"]').value,");
//     page += F("        port: parseInt(document.querySelector('input[name=\"mqtt_port\"]').value),");
//     page += F("        username: document.querySelector('input[name=\"mqtt_user\"]').value,");
//     page += F("        password: document.querySelector('input[name=\"mqtt_pass\"]').value");
//     page += F("    };");
//     page += F("    let configData = {");
//     page += F("        pumps: pumpsData,");
//     page += F("        mqtt: mqttData");
//     page += F("    };");
//     page += F("    fetch('/save', {");
//     page += F("        method: 'POST',");
//     page += F("        headers: {");
//     page += F("            'Content-Type': 'application/json'");
//     page += F("        },");
//     page += F("        body: JSON.stringify(configData)");
//     page += F("    })");
//     page += F("    .then(response => response.json())");
//     page += F("    .then(data => {");
//     page += F("        if(data.status === 'success') {");
//     page += F("            alert('Konfiguracja została zapisana');");
//     page += F("            window.location.reload();");
//     page += F("        } else {");
//     page += F("            alert('Błąd: ' + data.message);");
//     page += F("        }");
//     page += F("    })");
//     page += F("    .catch(error => {");
//     page += F("        alert('Wystąpił błąd podczas zapisywania konfiguracji');");
//     page += F("        console.error('Error:', error);");
//     page += F("    });");
//     page += F("    return false;");
//     page += F("}");
//     page += F("</script></head><body>");
    
//     // Strona główna
//     page += F("<div class='container'>");
//     page += F("<h1>AquaDoser</h1>");

//     // Status systemu
//     page += F("<div class='section'>");
//     page += F("<h2>Status systemu</h2>");
//     page += F("<table class='config-table'>");
//     page += F("<tr><td>Status MQTT</td><td><span class='status ");
//     page += (systemStatus.mqtt_connected ? F("success'>Połączony") : F("error'>Rozłączony"));
//     page += F("</span></td></tr>");
//     page += F("</table></div>");

//     // Formularz konfiguracji
//     page += F("<form onsubmit='return saveConfig()'>");
    
//     // Konfiguracja MQTT
//     page += F("<div class='section'>");
//     page += F("<h2>Konfiguracja MQTT</h2>");
//     page += F("<table class='config-table'>");
//     page += F("<tr><td>Broker MQTT</td><td><input type='text' name='mqtt_broker' value='");
//     page += mqttConfig.broker;
//     page += F("'></td></tr>");
//     page += F("<tr><td>Port MQTT</td><td><input type='number' name='mqtt_port' value='");
//     page += String(mqttConfig.port);
//     page += F("'></td></tr>");
//     page += F("<tr><td>Użytkownik MQTT</td><td><input type='text' name='mqtt_user' value='");
//     page += mqttConfig.username;
//     page += F("'></td></tr>");
//     page += F("<tr><td>Hasło MQTT</td><td><input type='password' name='mqtt_pass' value='");
//     page += mqttConfig.password;
//     page += F("'></td></tr>");
//     page += F("</table></div>");

//     // Konfiguracja pomp
//     for(int i = 0; i < NUMBER_OF_PUMPS; i++) {
//         page += F("<div class='section'>");
//         page += F("<h2>Pompa "); 
//         page += String(i + 1);
//         page += F("</h2>");
//         page += F("<table class='config-table'>");
        
//         // Nazwa pompy
//         page += F("<tr><td>Nazwa</td><td><input type='text' name='pump_name_");
//         page += String(i);
//         page += F("' value='");
//         page += pumps[i].name;
//         page += F("' maxlength='19'></td></tr>");

//         // Status aktywności
//         page += F("<tr><td>Aktywna</td><td><input type='checkbox' name='pump_enabled_");
//         page += String(i);
//         page += F("' ");
//         page += (pumps[i].enabled ? F("checked") : F(""));
//         page += F("></td></tr>");

//         // Kalibracja
//         page += F("<tr><td>Kalibracja (ml/s)</td><td><input type='number' step='0.01' name='pump_calibration_");
//         page += String(i);
//         page += F("' value='");
//         page += String(pumps[i].calibration);
//         page += F("' min='0.01'></td></tr>");

//         // Dawka
//         page += F("<tr><td>Dawka (ml)</td><td><input type='number' step='0.1' name='pump_dose_");
//         page += String(i);
//         page += F("' value='");
//         page += String(pumps[i].dose);
//         page += F("' min='0'></td></tr>");

//         // Godzina dozowania
//         page += F("<tr><td>Godzina dozowania</td><td><input type='number' name='pump_schedule_hour_");
//         page += String(i);
//         page += F("' value='");
//         page += String(pumps[i].schedule_hour);
//         page += F("' min='0' max='23'></td></tr>");

//         // Minuta dozowania
//         page += F("<tr><td>Minuta dozowania</td><td><input type='number' name='pump_minute_");
//         page += String(i);
//         page += F("' value='");
//         page += String(pumps[i].minute);
//         page += F("' min='0' max='59'></td></tr>");

//         // Dni tygodnia
//         page += F("<tr><td>Dni dozowania</td><td>");
//         for(int day = 0; day < 7; day++) {
//             page += F("<label style='margin-right: 5px;'><input type='checkbox' name='pump_day_");
//             page += String(i);
//             page += F("_");
//             page += String(day);
//             page += F("' ");
//             page += ((pumps[i].schedule_days & (1 << day)) ? F("checked") : F(""));
//             page += F("> ");
//             page += dayNames[day];
//             page += F("</label> ");
//         }
//         page += F("</td></tr>");
        
//         page += F("</table></div>");
//     }

//     // Przyciski akcji
//     page += F("<div class='section'>");
//     page += F("<div class='buttons-container'>");
//     page += F("<input type='submit' value='Zapisz ustawienia' class='btn btn-blue'>");
//     page += F("<button type='button' onclick='if(confirm(\"Czy na pewno chcesz zresetować urządzenie?\")) { fetch(\"/reset\"); }' class='btn btn-red'>Reset urządzenia</button>");
//     page += F("</div></div>");
    
//     page += F("</form></div></body></html>");
//     return page;
// }

String getConfigPage() {
    String page = F("<!DOCTYPE html><html lang='pl'><head>");
    page += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    page += F("<title>AquaDoser</title><style>");
    page += getStyles();
    page += F("</style>");
    
    // Skrypt JavaScript z WebSocket i funkcjami
    page += F("<script>");
    // Dodanie WebSocket
    page += F("let ws = new WebSocket('ws://' + window.location.hostname + ':81/');");
    page += F("ws.onmessage = function(event) {");
    page += F("    let status = JSON.parse(event.data);");
    page += F("    updateStatus(status);");
    page += F("};");
    
    // Funkcja aktualizacji statusu
    page += F("function updateStatus(status) {");
    page += F("    document.getElementById('mqtt-status').className = 'status ' + (status.mqtt_connected ? 'success' : 'error');");
    page += F("    document.getElementById('mqtt-status').innerText = status.mqtt_connected ? 'Połączony' : 'Rozłączony';");
    page += F("    document.getElementById('wifi-status').className = 'status ' + (status.wifi_connected ? 'success' : 'error');");
    page += F("    document.getElementById('wifi-status').innerText = status.wifi_connected ? 'Połączony' : 'Rozłączony';");
    page += F("    document.getElementById('uptime').innerText = formatUptime(status.uptime);");
    page += F("    document.getElementById('current-time').innerText = status.current_time;");
    page += F("}");
    
    // Funkcja formatowania czasu pracy
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

    // Funkcja zapisu konfiguracji
    page += F("function saveConfig() {");
    page += F("    let pumpsData = [];");
    page += F("    for(let i = 0; i < ");
    page += String(NUMBER_OF_PUMPS);
    page += F("; i++) {");
    page += F("        let pumpData = {");
    page += F("            name: document.querySelector(`input[name='pump_name_${i}']`).value,");
    page += F("            enabled: document.querySelector(`input[name='pump_enabled_${i}']`).checked,");
    page += F("            calibration: parseFloat(document.querySelector(`input[name='pump_calibration_${i}']`).value),");
    page += F("            dose: parseFloat(document.querySelector(`input[name='pump_dose_${i}']`).value),");
    page += F("            schedule_hour: parseInt(document.querySelector(`input[name='pump_schedule_hour_${i}']`).value),");
    page += F("            minute: parseInt(document.querySelector(`input[name='pump_minute_${i}']`).value),");
    page += F("            schedule_days: 0");
    page += F("        };");
    page += F("        // Oblicz dni tygodnia");
    page += F("        for(let day = 0; day < 7; day++) {");
    page += F("            if(document.querySelector(`input[name='pump_day_${i}_${day}']`).checked) {");
    page += F("                pumpData.schedule_days |= (1 << day);");
    page += F("            }");
    page += F("        }");
    page += F("        pumpsData.push(pumpData);");
    page += F("    }");
    page += F("    let mqttData = {");
    page += F("        broker: document.querySelector('input[name=\"mqtt_broker\"]').value,");
    page += F("        port: parseInt(document.querySelector('input[name=\"mqtt_port\"]').value),");
    page += F("        username: document.querySelector('input[name=\"mqtt_user\"]').value,");
    page += F("        password: document.querySelector('input[name=\"mqtt_pass\"]').value");
    page += F("    };");
    page += F("    let configData = {");
    page += F("        pumps: pumpsData,");
    page += F("        mqtt: mqttData");
    page += F("    };");
    page += F("    fetch('/save', {");
    page += F("        method: 'POST',");
    page += F("        headers: {");
    page += F("            'Content-Type': 'application/json'");
    page += F("        },");
    page += F("        body: JSON.stringify(configData)");
    page += F("    })");
    page += F("    .then(response => response.json())");
    page += F("    .then(data => {");
    page += F("        if(data.status === 'success') {");
    page += F("            alert('Konfiguracja została zapisana');");
    page += F("            window.location.reload();");
    page += F("        } else {");
    page += F("            alert('Błąd: ' + data.message);");
    page += F("        }");
    page += F("    })");
    page += F("    .catch(error => {");
    page += F("        alert('Wystąpił błąd podczas zapisywania konfiguracji');");
    page += F("        console.error('Error:', error);");
    page += F("    });");
    page += F("    return false;");
    page += F("}");
    page += F("</script></head><body>");
    
    // Strona główna
    page += F("<div class='container'>");
    page += F("<h1>AquaDoser</h1>");

    // Rozszerzony status systemu
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
    
    // Czas pracy
    page += F("<tr><td>Czas pracy</td><td><span id='uptime'>");
    unsigned long uptime = millis() / 1000;
    page += String(uptime);
    page += F("</span></td></tr>");
    
    // Adres IP
    page += F("<tr><td>Adres IP</td><td>");
    page += WiFi.localIP().toString();
    page += F("</td></tr>");
    
    // Aktualna data i czas
    DateTime now = rtc.now();
    page += F("<tr><td>Data i czas</td><td><span id='current-time'>");
    page += String(now.year()) + "-" + 
            (now.month() < 10 ? "0" : "") + String(now.month()) + "-" + 
            (now.day() < 10 ? "0" : "") + String(now.day()) + " " +
            (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
            (now.minute() < 10 ? "0" : "") + String(now.minute());
    page += F("</span></td></tr>");
    page += F("</table></div>");

    // Formularz konfiguracji
    page += F("<form onsubmit='return saveConfig()'>");
    
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
        // Zabezpieczenie przed pustą nazwą
        page += pumps[i].name[0] ? pumps[i].name : ("Pompa " + String(i + 1));
        page += F("' maxlength='19' required></td></tr>");

        // Status aktywności
        page += F("<tr><td>Aktywna</td><td><input type='checkbox' name='pump_enabled_");
        page += String(i);
        page += F("' ");
        page += (pumps[i].enabled ? F("checked") : F(""));
        page += F("></td></tr>");

        // Kalibracja - zabezpieczenie przed wartością 0
        page += F("<tr><td>Kalibracja (ml/s)</td><td><input type='number' step='0.01' name='pump_calibration_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].calibration > 0 ? pumps[i].calibration : 1.0);
        page += F("' min='0.01' required></td></tr>");

        // Dawka - zabezpieczenie przed wartością ujemną
        page += F("<tr><td>Dawka (ml)</td><td><input type='number' step='0.1' name='pump_dose_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].dose >= 0 ? pumps[i].dose : 0);
        page += F("' min='0' required></td></tr>");

        // Godzina dozowania - walidacja zakresu
        page += F("<tr><td>Godzina dozowania</td><td><input type='number' name='pump_schedule_hour_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].schedule_hour < 24 ? pumps[i].schedule_hour : 0);
        page += F("' min='0' max='23' required></td></tr>");

        // Minuta dozowania - walidacja zakresu
        page += F("<tr><td>Minuta dozowania</td><td><input type='number' name='pump_minute_");
        page += String(i);
        page += F("' value='");
        page += String(pumps[i].minute < 60 ? pumps[i].minute : 0);
        page += F("' min='0' max='59' required></td></tr>");

        // Dni tygodnia
        page += F("<tr><td>Dni dozowania</td><td>");
        for(int day = 0; day < 7; day++) {
            page += F("<label style='margin-right: 5px;'><input type='checkbox' name='pump_day_");
            page += String(i);
            page += F("_");
            page += String(day);
            page += F("' ");
            page += ((pumps[i].schedule_days & (1 << day)) ? F("checked") : F(""));
            page += F("> ");
            page += dayNames[day];
            page += F("</label> ");
        }
        page += F("</td></tr>");
        
        page += F("</table></div>");
    }

    // Przyciski akcji
    page += F("<div class='section'>");
    page += F("<div class='buttons-container'>");
    page += F("<input type='submit' value='Zapisz ustawienia' class='btn btn-blue'>");
    page += F("<button type='button' onclick='if(confirm(\"Czy na pewno chcesz zresetować urządzenie?\")) { fetch(\"/reset\"); }' class='btn btn-red'>Reset urządzenia</button>");
    page += F("</div></div>");
    
    page += F("</form></div>");
    
    // Dodanie informacji o wersji
    page += F("<div class='footer'>");
    page += F("AquaDoser v1.0");
    page += F("</div>");
    
    page += F("</body></html>");
    return page;
}

// void resetFactorySettings() {
//     // Wyczyść EEPROM zapisując wartość inną niż znacznik walidacji
//     uint32_t invalidMark = 0;
//     EEPROM.put(0, invalidMark);
//     EEPROM.commit();
    
//     // Załaduj domyślne wartości
//     loadConfig();
    
//     // Zresetuj pozostałe ustawienia
//     for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
//         pumps[i].lastDosing = 0;
//         pumps[i].isRunning = false;
//     }
    
//     saveConfig();
// }

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
    mqttConfig.enabled = false;
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

// --- Setup
void setup() {
    // 1. Inicjalizacja komunikacji
    Serial.begin(115200);
    Serial.println("\nAquaDoser Start");
    Wire.begin();
    
    // 2. Inicjalizacja pamięci i konfiguracji
    EEPROM.begin(1024);
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
    
    // 6. Synchronizacja czasu
    syncRTC();
    
    // 7. Efekty startowe
    playWelcomeEffect();
    welcomeMelody();
    
    Serial.println("Inicjalizacja zakończona");
}

// --- Loop
void loop() {
    unsigned long currentMillis = millis();
    
    // Obsługa serwera WWW i WebSocket
    server.handleClient();
    webSocket.loop();
    
    // Obsługa MQTT
    mqtt.loop();
    
    // Obsługa przycisków i LED
    handleButton();
    updateLEDs();
    
    // Aktualizacja statusu na WebSocket co 1 sekundę
    static unsigned long lastWebSocketUpdate = 0;
    if (currentMillis - lastWebSocketUpdate >= 1000) {
        String status = getSystemStatusJSON();
        webSocket.broadcastTXT(status);
        lastWebSocketUpdate = currentMillis;
    }
    
    // Log do Serial co LOG_INTERVAL
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
            Serial.println(F("No active pumps configured"));
        }
        
        Serial.println(F("========================================\n"));
        lastLogTime = currentMillis;
        firstRun = false;
    }

    // Sprawdzanie systemu i pomp co 5 sekund
    static unsigned long lastSystemCheck = 0;
    if (currentMillis - lastSystemCheck >= 5000) {
        // Sprawdź stan WiFi
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi connection lost - reconnecting...");
            WiFi.reconnect();
        }
        
        // Aktualizacja Home Assistant
        updateHomeAssistant();
        
        // Obsługa pomp
        handlePumps();
        
        lastSystemCheck = currentMillis;
    }
    
    // Synchronizacja RTC co 24h
    static unsigned long lastRtcSync = 0;
    if (currentMillis - lastRtcSync >= 24*60*60*1000UL || lastRtcSync == 0) {
        syncRTC();
        lastRtcSync = currentMillis;
    }
    
    // Obsługa przekroczenia millis()
    if (currentMillis < lastLogTime || currentMillis < lastSystemCheck || 
        currentMillis < lastRtcSync || currentMillis < lastWebSocketUpdate) {
        // Reset wszystkich liczników czasu
        lastLogTime = currentMillis;
        lastSystemCheck = currentMillis;
        lastRtcSync = currentMillis;
        lastWebSocketUpdate = currentMillis;
    }
    
    yield(); // Pozwól ESP8266 obsłużyć zadania systemowe
}
