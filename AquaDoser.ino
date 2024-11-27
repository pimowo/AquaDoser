// --- Biblioteki

#include <WiFiManager.h>          // Zarządzanie WiFi
#include <ESP8266WiFi.h>          // Obsługa WiFi
#include <ArduinoHA.h>            // Integracja z Home Assistant
#include <ArduinoJson.h>          // Obsługa JSON
#include <LittleFS.h>             // System plików
#include <Wire.h>                 // Komunikacja I2C
#include <DS3231.h>               // Zegar RTC
#include <PCF8574.h>      // Ekspander I/O
#include <Adafruit_NeoPixel.h>    // Diody WS2812
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <AsyncJson.h>

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
    float dose;
    uint8_t schedule_days;  // Dodane pole
    uint8_t schedule_hour;  // Dodane pole
    float calibration;      // Dodane pole
    unsigned long lastDosing;
    bool isRunning;
    unsigned long startTime;
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

AsyncWebServer server(80);

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
DS3231 rtc;
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
    File file = LittleFS.open(PUMPS_FILE, "r");
    if (!file) {
        Serial.println("Nie znaleziono pliku konfiguracji pomp");
        return false;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.println("Błąd parsowania JSON konfiguracji pomp");
        return false;
    }

    JsonArray pumpsArray = doc["pumps"];
    for (unsigned int i = 0; i < min((size_t)NUM_PUMPS, pumpsArray.size()); i++) {
        pumps[i].enabled = pumpsArray[i]["enabled"] | false;
        pumps[i].calibration = pumpsArray[i]["calibration"] | 1.0f;
        pumps[i].dose = pumpsArray[i]["dose"] | 0.0f;
        pumps[i].schedule_days = pumpsArray[i]["schedule"]["days"] | 0;
        pumps[i].schedule_hour = pumpsArray[i]["schedule"]["hour"] | 0;
    }
    return true;
}

// --- Zapisywanie konfiguracji pomp
bool savePumpsConfig() {
    StaticJsonDocument<1024> doc;
    JsonArray pumpsArray = doc.createNestedArray("pumps");

    for (int i = 0; i < NUM_PUMPS; i++) {
        JsonObject pumpObj = pumpsArray.createNestedObject();
        pumpObj["enabled"] = pumps[i].enabled;
        pumpObj["calibration"] = pumps[i].calibration;
        pumpObj["dose"] = pumps[i].dose;
        
        JsonObject schedule = pumpObj.createNestedObject("schedule");
        schedule["days"] = pumps[i].schedule_days;
        schedule["hour"] = pumps[i].schedule_hour;
    }

    File file = LittleFS.open(PUMPS_FILE, "w");
    if (!file) {
        Serial.println("Błąd otwarcia pliku konfiguracji pomp do zapisu");
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("Błąd zapisu konfiguracji pomp");
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
        pcf8574.write(i, LOW);  // Nie potrzeba pinMode
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
    if (pcf8574.write(pumpIndex, HIGH)) {
        pumpRunning[pumpIndex] = true;
        doseStartTime[pumpIndex] = millis();
        
        // Aktualizuj stan LED
        strip.setPixelColor(pumpIndex, COLOR_WORKING);
        strip.show();
        
        Serial.printf("Pompa %d rozpoczęła dozowanie. Zaplanowany czas: %.1f ms\n", 
                     pumpIndex + 1, dosingTime);
    } else {
        Serial.printf("Błąd: Nie można włączyć pompy %d\n", pumpIndex + 1);
    }
}

// --- Zatrzymanie dozowania dla pompy
void stopPump(uint8_t pumpIndex) {
    if (!pumpInitialized || pumpIndex >= NUM_PUMPS || !pumpRunning[pumpIndex]) {
        return;
    }

    // Wyłącz pompę
    if (pcf8574.write(pumpIndex, LOW)) {
        pumpRunning[pumpIndex] = false;
        
        // Oblicz faktyczny czas dozowania
        unsigned long actualDoseTime = millis() - doseStartTime[pumpIndex];
        
        // Aktualizuj stan LED
        strip.setPixelColor(pumpIndex, pumps[pumpIndex].enabled ? COLOR_ON : COLOR_OFF);
        strip.show();
        
        Serial.printf("Pompa %d zakończyła dozowanie. Rzeczywisty czas: %lu ms\n", 
                     pumpIndex + 1, actualDoseTime);
    } else {
        Serial.printf("Błąd: Nie można wyłączyć pompy %d\n", pumpIndex + 1);
    }
}

// --- Sprawdzenie czy pompa powinna rozpocząć dozowanie
bool shouldStartDosing(uint8_t pumpIndex) {
    if (!pumps[pumpIndex].enabled || serviceMode) {
        return false;
    }

    // Pobierz aktualny czas z RTC
    bool h12, PM;
    uint8_t currentHour = rtc.getHour(h12, PM);
    uint8_t currentDay = rtc.getDoW(); // 0 = Niedziela, 6 = Sobota
    
    // Sprawdź czy jest odpowiedni dzień (bit w masce dni)
    bool isDoseDay = (pumps[pumpIndex].schedule_days & (1 << currentDay)) != 0;
    
    // Sprawdź czy jest odpowiednia godzina
    bool isDoseHour = (currentHour == pumps[pumpIndex].schedule_hour);
    
    return isDoseDay && isDoseHour;
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

if (state) {
    pcf8574.write(pumpIndex, HIGH);
} else {
    pcf8574.write(pumpIndex, LOW);
}
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
    //serviceModeSwitch = new HASwitch("service_mode", false);
    HASwitch switchService("service_mode");
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
        pumpCalibrations[i] = new HANumber(entityId, HANumber::PrecisionP1);
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
    if (WiFi.status() != WL_CONNECTED) return;
    
    static WiFiUDP ntpUDP;
    static NTPClient timeClient(ntpUDP, "pool.ntp.org");
    
    timeClient.begin();
    if (timeClient.update()) {
        time_t epochTime = timeClient.getEpochTime();
        struct tm *ptm = gmtime(&epochTime);
        
        rtc.setClockMode(false); // 24h mode
        rtc.setYear(ptm->tm_year - 100);
        rtc.setMonth(ptm->tm_mon + 1);
        rtc.setDate(ptm->tm_mday);
        rtc.setDoW(ptm->tm_wday);
        rtc.setHour(ptm->tm_hour);
        rtc.setMinute(ptm->tm_min);
        rtc.setSecond(ptm->tm_sec);
        
        Serial.println("RTC zsynchronizowany z NTP");
    }
    timeClient.end();
}

// --- Inicjalizacja WiFi
void setupWiFi() {
    WiFiManager wifiManager;
    
    if (!wifiManager.autoConnect("AquaDoser")) {
        Serial.println("Nie udało się połączyć i timeout");
        ESP.restart();
        delay(1000);
    }
    
    Serial.println("Połączono z WiFi");
}

void setupWebServer() {
    // Obsługa plików statycznych
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    
    // API dla pomp
    server.on("/api/pumps", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        DynamicJsonDocument doc(1024);
        JsonArray pumpsArray = doc.createNestedArray("pumps");
        
        for (int i = 0; i < NUM_PUMPS; i++) {
            JsonObject pump = pumpsArray.createNestedObject();
            pump["id"] = i;
            pump["enabled"] = pumps[i].enabled;
            pump["calibration"] = pumps[i].calibration;
            pump["dose"] = pumps[i].dose;
            pump["schedule_days"] = pumps[i].schedule_days;
            pump["schedule_hour"] = pumps[i].schedule_hour;
        }
        
        serializeJson(doc, *response);
        request->send(response);
    });

    // Aktualizacja konfiguracji pomp
    AsyncCallbackJsonWebHandler* pumpHandler = new AsyncCallbackJsonWebHandler("/api/pumps", [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        if (jsonObj.containsKey("pumps")) {
            JsonArray pumpsArray = jsonObj["pumps"];
            for (JsonVariant v : pumpsArray) {
                JsonObject pump = v.as<JsonObject>();
                int i = pump["id"];
                if (i >= 0 && i < NUM_PUMPS) {
                    pumps[i].enabled = pump["enabled"] | false;
                    pumps[i].calibration = pump["calibration"] | 1.0f;
                    pumps[i].dose = pump["dose"] | 0.0f;
                    pumps[i].schedule_days = pump["schedule_days"] | 0;
                    pumps[i].schedule_hour = pump["schedule_hour"] | 0;
                }
            }
            savePumpsConfig();
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"Invalid data\"}");
        }
    });
    server.addHandler(pumpHandler);

    // API dla MQTT
    server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        DynamicJsonDocument doc(256);
        
        doc["server"] = networkConfig.mqtt_server;
        doc["port"] = networkConfig.mqtt_port;
        doc["user"] = networkConfig.mqtt_user;
        doc["password"] = networkConfig.mqtt_password;
        
        serializeJson(doc, *response);
        request->send(response);
    });

    // Aktualizacja konfiguracji MQTT
    AsyncCallbackJsonWebHandler* mqttHandler = new AsyncCallbackJsonWebHandler("/api/mqtt", [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        
        strlcpy(networkConfig.mqtt_server, jsonObj["server"] | "", sizeof(networkConfig.mqtt_server));
        networkConfig.mqtt_port = jsonObj["port"] | 1883;
        strlcpy(networkConfig.mqtt_user, jsonObj["user"] | "", sizeof(networkConfig.mqtt_user));
        strlcpy(networkConfig.mqtt_password, jsonObj["password"] | "", sizeof(networkConfig.mqtt_password));
        
        saveNetworkConfig();
        setupMQTT(); // Ponowne połączenie z MQTT
        
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
    server.addHandler(mqttHandler);

    // API dla informacji systemowych
    server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        DynamicJsonDocument doc(128);
        
        doc["uptime"] = millis() / 1000; // czas w sekundach
        doc["mqtt_connected"] = mqtt.isConnected();
        
        serializeJson(doc, *response);
        request->send(response);
    });

    // Obsługa nieznanych ścieżek
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.println("Serwer HTTP uruchomiony");
}

// --- Setup
void setup() {
    Serial.begin(115200);
    Serial.println("\nAquaDoser Start");
    
    // Inicjalizacja pinów
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Inicjalizacja systemu plików i wczytanie konfiguracji
    if (!initFileSystem()) {
        Serial.println("Błąd inicjalizacji systemu plików!");
    }
    loadConfiguration();
    
    // Inicjalizacja sprzętu
    Wire.begin();
    rtc.begin();  // usuń sprawdzanie if (!rtc.begin())
    
    Wire.begin();
    if (pcf8574.begin()) {
        Serial.println("PCF8574 initialized");
    } else {
        Serial.println("PCF8574 init failed");
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
    
    Serial.println("Inicjalizacja zakończona");
}

// --- Loop
void loop() {
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
