// --- Biblioteki

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <PCF8574.h>
#include <ArduinoHA.h>
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
struct PumpConfig {
    bool enabled;
    float calibration;     // ml/min - prędkość pompy
    float dose;           // ml - dawka do podania
    uint8_t schedule_days; // bitowa mapa dni tygodnia
    uint8_t schedule_hour; // godzina dozowania
    unsigned long lastDosing;
    bool isRunning;
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

ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
ESP8266HTTPUpdateServer httpUpdateServer;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
// Struktura dla informacji systemowych
struct SystemInfo {
    unsigned long uptime;
    bool mqtt_connected;
};

SystemInfo sysInfo = {0, false};

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
unsigned long lastRtcSync = 0;

// --- Deklaracje encji Home Assistant
HASwitch* pumpSwitches[NUM_PUMPS];
HASensor* pumpStates[NUM_PUMPS];
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

// --- Stałe dla systemu plików
const char* CONFIG_DIR = "/config";
const char* PUMPS_FILE = "/config/pumps.json";
const char* NETWORK_FILE = "/config/network.json";
const char* SYSTEM_FILE = "/config/system.json";

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

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Rozłączono!\n", num);
            break;
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                Serial.printf("[%u] Połączono z %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
            }
            break;
        case WStype_TEXT:
            {
                String text = String((char*)payload);
                DynamicJsonDocument doc(512);
                DeserializationError error = deserializeJson(doc, text);
                
                if (!error) {
                    // Tutaj możesz dodać obsługę komend WebSocket
                    if (doc.containsKey("command")) {
                        String command = doc["command"];
                        // Obsługa komend...
                    }
                }
            }
            break;
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
    for (int i = 0; i < NUM_PUMPS; i++) {
        char entityId[20];
        char name[20];
        
        // Przełącznik pompy
        sprintf(entityId, "pump_%d", i + 1);
        sprintf(name, "Pompa %d", i + 1);
        pumpSwitches[i] = new HASwitch(entityId);
        pumpSwitches[i]->setName(name);
        pumpSwitches[i]->onCommand(onPumpSwitch);
        pumpSwitches[i]->setIcon("mdi:water-pump");
        
        // Status pompy
        sprintf(entityId, "pump_%d_state", i + 1);
        sprintf(name, "Stan pompy %d", i + 1);
        pumpStates[i] = new HASensor(entityId);
        pumpStates[i]->setName(name);
        pumpStates[i]->setIcon("mdi:state-machine");
        
        // Kalibracja pompy
        sprintf(entityId, "pump_%d_calibration", i + 1);
        sprintf(name, "Kalibracja pompy %d", i + 1);
        pumpCalibrations[i] = new HANumber(entityId);
        pumpCalibrations[i]->setName(name);
        pumpCalibrations[i]->setIcon("mdi:ruler");
        pumpCalibrations[i]->setMin(0.1);
        pumpCalibrations[i]->setMax(10.0);
        pumpCalibrations[i]->setStep(0.1);
        pumpCalibrations[i]->onCommand(onPumpCalibration);
    }
}

// --- Aktualizacja stanów w Home Assistant
void updateHomeAssistant() {
    unsigned long currentMillis = millis();
    
    // Aktualizuj tylko co HA_UPDATE_INTERVAL
    if (currentMillis - lastHaUpdate < HA_UPDATE_INTERVAL) {
        return;
    }
    lastHaUpdate = currentMillis;
    
    // Aktualizacja trybu serwisowego
    serviceModeSwitch->setState(serviceMode);
    
    // Aktualizacja stanów pomp
    for (int i = 0; i < NUM_PUMPS; i++) {
        // Stan włączenia
        pumpSwitches[i]->setState(pumps[i].enabled);
        
        // Status tekstowy
        const char* state;
        if (serviceMode) {
            state = "Tryb serwisowy";
        } else if (pumpRunning[i]) {
            state = "Dozowanie";
        } else if (pumps[i].enabled) {
            state = "Gotowa";
        } else {
            state = "Wyłączona";
        }
        pumpStates[i]->setValue(state);
        
        // Kalibracja
        //pumpCalibrations[i]->setValue(pumps[i].calibration);
    }
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

String getConfigPage() {
    String page = F("<!DOCTYPE html><html><head>");
    page += F("<meta charset='utf-8'>");
    page += F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
    page += F("<title>AquaDoser</title>");
    page += F("<style>");
    page += F("body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f5f5f5; }");
    page += F(".container { max-width: 960px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }");
    page += F("h1, h2 { color: #2196F3; }");
    page += F("h1 { text-align: center; margin-bottom: 30px; }");
    page += F(".section { background: #fff; border: 1px solid #ddd; padding: 20px; margin-bottom: 20px; border-radius: 4px; }");
    page += F(".form-group { margin-bottom: 15px; }");
    page += F("label { display: inline-block; margin-bottom: 5px; color: #666; }");
    page += F("input[type='text'], input[type='number'] { width: 200px; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }");
    page += F("input[type='checkbox'] { margin-right: 5px; }");
    page += F(".days-group { margin: 10px 0; }");
    page += F(".days-group label { margin-right: 10px; }");
    page += F(".button-group { text-align: center; margin: 20px 0; }");
    page += F(".button-primary { background: #2196F3; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; margin: 5px; }");
    page += F(".button-warning { background: #f44336; color: white; }");
    page += F(".button-primary:hover { background: #1976D2; }");
    page += F(".button-warning:hover { background: #d32f2f; }");
    page += F(".status-item { display: inline-block; margin-right: 20px; }");
    page += F("</style>");
    page += F("</head><body>");
    page += F("<div class='container'>");
    page += F("<h1>AquaDoser</h1>");

    // Status systemu
    page += F("<div class='section'>");
    page += F("<h2>Status systemu</h2>");
    page += F("<div class='status-item'><strong>Data i czas:</strong> <span id='datetime'></span></div>");
    page += F("<div class='status-item'><strong>WiFi:</strong> ");
    page += WiFi.SSID();
    page += F(" (");
    page += WiFi.RSSI();
    page += F(" dBm)</div>");
    page += F("<div class='status-item'><strong>IP:</strong> ");
    page += WiFi.localIP().toString();
    page += F("</div>");
    page += F("<div class='status-item'><strong>Uptime:</strong> <span id='uptime'></span></div>");
    page += F("</div>");

    // Konfiguracja MQTT
    page += F("<div class='section'>");
    page += F("<h2>Konfiguracja MQTT</h2>");
    page += F("<div class='form-group'>");
    page += F("<label>MQTT Server: <input type='text' id='mqtt_server' value='");
    page += mqttServer;
    page += F("'></label>");
    page += F("</div>");
    page += F("<div class='form-group'>");
    page += F("<label>MQTT Port: <input type='number' id='mqtt_port' value='");
    page += String(mqttPort);
    page += F("'></label>");
    page += F("</div>");
    page += F("<div class='form-group'>");
    page += F("<label>MQTT User: <input type='text' id='mqtt_user' value='");
    page += mqttUser;
    page += F("'></label>");
    page += F("</div>");
    page += F("<div class='form-group'>");
    page += F("<label>MQTT Password: <input type='password' id='mqtt_password' value='");
    page += mqttPassword;
    page += F("'></label>");
    page += F("</div>");
    page += F("<div class='form-group'>");
    page += F("<label><input type='checkbox' id='mqtt_enabled'");
    if (mqttEnabled) page += F(" checked");
    page += F("> Enable MQTT</label>");
    page += F("</div>");
    page += F("</div>");

    // Konfiguracja pomp
    for (uint8_t i = 0; i < NUM_PUMPS; i++) {
        page += F("<div class='section'>");
        page += "<h2>Pump " + String(i + 1) + "</h2>";
        // [reszta konfiguracji pompy bez zmian]
        page += F("</div>");
    }

    // Przyciski
    page += F("<div class='button-group'>");
    page += F("<button class='button-primary' onclick='saveConfig()'>Save Configuration</button>");
    page += F("<button class='button-primary' onclick='location.href=\"/update\"'>Update Firmware</button>");
    page += F("<button class='button-primary' onclick='if(confirm(\"Restart device?\")) restartDevice()'>Restart Device</button>");
    page += F("<button class='button-warning' onclick='if(confirm(\"Reset to factory defaults?\")) resetFactory()'>Factory Reset</button>");
    page += F("</div>");

    // JavaScript
    page += F("<script>");
    // Aktualizacja czasu
    page += F("function updateDateTime() {");
    page += F("    const now = new Date();");
    page += F("    document.getElementById('datetime').textContent = now.toLocaleString();");
    page += F("}");
    page += F("setInterval(updateDateTime, 1000);");
    page += F("updateDateTime();");

    // Aktualizacja uptime
    page += F("let uptime = "); 
    page += String(millis() / 1000);
    page += F(";");
    page += F("function updateUptime() {");
    page += F("    const days = Math.floor(uptime / 86400);");
    page += F("    const hours = Math.floor((uptime % 86400) / 3600);");
    page += F("    const minutes = Math.floor((uptime % 3600) / 60);");
    page += F("    const seconds = Math.floor(uptime % 60);");
    page += F("    document.getElementById('uptime').textContent = ");
    page += F("        days + 'd ' + hours + 'h ' + minutes + 'm ' + seconds + 's';");
    page += F("    uptime++;");
    page += F("}");
    page += F("setInterval(updateUptime, 1000);");
    page += F("updateUptime();");

    // Funkcje przycisków
    page += F("function restartDevice() {");
    page += F("    fetch('/restart', {method: 'POST'})");
    page += F("    .then(() => alert('Device is restarting...'))");
    page += F("    .catch(error => alert('Error: ' + error));");
    page += F("}");

    page += F("function resetFactory() {");
    page += F("    fetch('/factory-reset', {method: 'POST'})");
    page += F("    .then(() => alert('Device is resetting and will restart...'))");
    page += F("    .catch(error => alert('Error: ' + error));");
    page += F("}");

    // Funkcja zapisu konfiguracji
    page += F("function saveConfig() {");
    // [istniejący kod zapisu konfiguracji]
    page += F("}");
    page += F("</script>");
    page += F("</div></body></html>");
    
    return page;
}

function saveConfig() {
    const config = {
        pumps: [],
        mqtt: {
            enabled: document.getElementById('mqtt_enabled').checked,
            server: document.getElementById('mqtt_server').value,
            port: parseInt(document.getElementById('mqtt_port').value) || 1883,
            user: document.getElementById('mqtt_user').value,
            password: document.getElementById('mqtt_password').value
        }
    };

    // Konfiguracja pomp
    for(let i = 0; i < 4; i++) {  // NUM_PUMPS = 4
        const pump = {
            enabled: document.querySelector(`.pump-enabled[data-pump='${i}']`).checked,
            calibration: parseFloat(document.querySelector(`.pump-calibration[data-pump='${i}']`).value) || 1.0,
            dose: parseFloat(document.querySelector(`.pump-dose[data-pump='${i}']`).value) || 0.0,
            schedule: {
                hour: parseInt(document.querySelector(`.pump-hour[data-pump='${i}']`).value) || 0,
                days: Array.from(document.querySelectorAll(`.pump-day[data-pump='${i}']`))
                      .reduce((acc, cb, idx) => acc | (cb.checked ? (1 << idx) : 0), 0)
            }
        };
        config.pumps.push(pump);
    }

    // Wysyłanie konfiguracji
    fetch('/save', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(config)
    })
    .then(response => {
        if (response.ok) {
            alert('Konfiguracja zapisana');
        } else {
            response.text().then(text => {
                alert('Błąd zapisu konfiguracji: ' + text);
            });
        }
    })
    .catch(error => {
        alert('Błąd: ' + error.message);
    });
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
    
    // Inicjalizacja systemu plików i wczytanie konfiguracji
    if (!initFileSystem()) {
        Serial.println("Błąd inicjalizacji systemu plików!");
    }
    loadConfiguration();
    
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
    //MDNS.update();

    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 5000) {  // Co 5 sekund
        lastCheck = millis();
        Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("WiFi Status: %d\n", WiFi.status());
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
            
            for (int i = 0; i < NUM_PUMPS; i++) {
                DEBUG_SERIAL("Pompa " + String(i + 1) + ":");
                DEBUG_SERIAL("  Enabled: " + String(pumps[i].enabled));
                DEBUG_SERIAL("  Calibration: " + String(pumps[i].calibration));
                DEBUG_SERIAL("  Dose: " + String(pumps[i].dose));
                DEBUG_SERIAL("  Schedule Hour: " + String(pumps[i].schedule_hour));
                DEBUG_SERIAL("  Schedule Days: " + String(pumps[i].schedule_days, BIN));
                DEBUG_SERIAL("  Is Running: " + String(pumps[i].isRunning));
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
    
    // Obsługa MQTT i Home Assistant
    mqtt.loop();
    
    // Synchronizacja RTC co 24h
    static unsigned long lastRtcSync = 0;
    if (millis() - lastRtcSync >= 24*60*60*1000UL) {
        syncRTC();
        lastRtcSync = millis();
    }
    
    // Obsługa przycisku
    handleButton();
    
    // Obsługa pomp
    handlePumps();
    
    // Aktualizacja LED
    updateLEDs();
    
    // Aktualizacja Home Assistant
    updateHomeAssistant();
    
    // Pozwól ESP8266 obsłużyć inne zadania
    yield();
}
