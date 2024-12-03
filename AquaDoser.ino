// ** BIBLIOTEKI **

#include <Arduino.h>                  // Podstawowa biblioteka Arduino zawierająca funkcje rdzenia
#include <ArduinoHA.h>                // Biblioteka do integracji z Home Assistant przez protokół MQTT
#include <ArduinoOTA.h>               // Biblioteka do aktualizacji oprogramowania przez sieć WiFi
#include <ESP8266WiFi.h>              // Biblioteka WiFi dedykowana dla układu ESP8266
#include <EEPROM.h>                   // Biblioteka do dostępu do pamięci nieulotnej EEPROM
#include <WiFiManager.h>              // Biblioteka do zarządzania połączeniami WiFi
#include <ESP8266WebServer.h>         // Biblioteka do obsługi serwera HTTP na ESP8266
#include <WebSocketsServer.h>         // Biblioteka do obsługi serwera WebSockets na ESP8266
#include <ESP8266HTTPUpdateServer.h>  // Biblioteka do aktualizacji oprogramowania przez HTTP

#include <Wire.h>
#include <PCF8574.h>

// ** DEFINICJE PINÓW **

// Przypisanie pinów do urządzeń
const int NUMBER_OF_PUMPS = 8;  // Ilość pomp
const int BUZZER_PIN = 13;      // GPIO 13
const int LED_PIN = 12;         // GPIO 12
const int PRZYCISK_PIN = 14;    // GPIO 14
const int SDA_PIN = 4;          // GPIO 4
const int SCL_PIN = 5;          // GPIO 5

PCF8574 pcf8574(0x20);

// ** USTAWIENIA CZASOWE **

// Konfiguracja timeoutów i interwałów
const unsigned long WATCHDOG_TIMEOUT = 8000;       // Timeout dla watchdoga
const unsigned long LONG_PRESS_TIME = 1000;        // Czas długiego naciśnięcia przycisku
const unsigned long MQTT_LOOP_INTERVAL = 100;      // Obsługa MQTT co 100ms
const unsigned long OTA_CHECK_INTERVAL = 1000;     // Sprawdzanie OTA co 1s
const unsigned long MQTT_RETRY_INTERVAL = 10000;   // Próba połączenia MQTT co 10s
const unsigned long MILLIS_OVERFLOW_THRESHOLD = 4294967295U - 60000; // ~49.7 dni


void updateHAState(uint8_t pumpIndex);

// ** KONFIGURACJA SYSTEMU **

// Makra debugowania
// Makra debugowania
#define DEBUG 0  // 0 wyłącza debug, 1 włącza debug

#if DEBUG
    #define AQUA_DEBUG_PRINT(x) Serial.println(x)
    #define AQUA_DEBUG_PRINTF(format, ...) Serial.printf(format, __VA_ARGS__)
#else
    #define AQUA_DEBUG_PRINT(x)
    #define AQUA_DEBUG_PRINTF(format, ...)
#endif

// Zmienna przechowująca wersję oprogramowania
const char* SOFTWARE_VERSION = "2.12.24";  // Definiowanie wersji oprogramowania

// Struktury konfiguracyjne i statusowe

// Struktura dla pojedynczej pompy
struct PumpConfig {
    bool enabled;                // czy pompa jest włączona w harmonogramie
    uint8_t dosage;          // dawka w ml
    float calibration;       // kalibracja (ml/min)
    uint8_t pcf8574_pin;    // numer pinu na PCF8574
    uint8_t hour;               // godzina dozowania
    uint8_t minute;             // minuta dozowania
};

// Konfiguracja
// Główna struktura konfiguracji
struct Config {
    char hostname[32];          // nazwa urządzenia w sieci
    char mqtt_server[64];         // adres serwera MQTT
    int mqtt_port;              // port MQTT
    char mqtt_user[32];         // nazwa użytkownika MQTT
    char mqtt_password[32];     // hasło MQTT
    bool soundEnabled;         // czy dźwięki są włączone
    PumpConfig pumps[NUMBER_OF_PUMPS];  // konfiguracja dla każdej pompy
    uint8_t configVersion;     // wersja konfiguracji (dla EEPROM)
    char checksum;             // suma kontrolna konfiguracji
};

// Struktura dla pojedynczej pompy - stan bieżący
struct Pump {
    bool isRunning;           // czy pompa aktualnie pracuje
    unsigned long lastDose;   // czas ostatniego dozowania
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
    unsigned long lastMQTTRetry;     // ostatnia próba połączenia MQTT
    unsigned long lastOTACheck;      // ostatnie sprawdzenie aktualizacji
    unsigned long lastMQTTLoop;      // ostatnia pętla MQTT
    unsigned long lastPumpCheck;     // ostatnie sprawdzenie harmonogramu
    unsigned long lastStateUpdate;   // ostatnia aktualizacja stanu do HA
    unsigned long lastButtonCheck;   // ostatnie sprawdzenie przycisku
    
    // Konstruktor inicjalizujący wszystkie timery na 0
    Timers() : 
        lastMQTTRetry(0), 
        lastOTACheck(0), 
        lastMQTTLoop(0),
        lastPumpCheck(0),
        lastStateUpdate(0),
        lastButtonCheck(0) {}
};

// Globalne instancje struktur
Config config;
Status status;
ButtonState buttonState;
Timers timers;

// ** FILTROWANIE I POMIARY **

// Parametry czujnika ultradźwiękowego i obliczeń
const int HYSTERESIS = 10;  // Histereza przy zmianach poziomu (mm)
const int SENSOR_MIN_RANGE = 20;    // Minimalny zakres czujnika (mm)
const int SENSOR_MAX_RANGE = 1020;  // Maksymalny zakres czujnika (mm)
const float EMA_ALPHA = 0.2f;       // Współczynnik wygładzania dla średniej wykładniczej (0-1)
const int SENSOR_AVG_SAMPLES = 3;   // Liczba próbek do uśrednienia pomiaru

float lastFilteredDistance = 0;     // Dla filtra EMA (Exponential Moving Average)
float lastReportedDistance = 0;     // Ostatnia zgłoszona wartość odległości
unsigned long lastMeasurement = 0;  // Ostatni czas pomiaru
float currentDistance = 0;          // Bieżąca odległość od powierzchni wody (mm)
float volume = 0;                   // Objętość wody w akwarium (l)
unsigned long pumpStartTime = 0;    // Czas rozpoczęcia pracy pompy
float waterLevelBeforePump = 0;     // Poziom wody przed uruchomieniem pompy

// ** INSTANCJE URZĄDZEŃ I USŁUG **

// Wi-Fi, MQTT i Home Assistant
WiFiClient client;              // Klient połączenia WiFi
HADevice device("AquaDoser");  // Definicja urządzenia dla Home Assistant
HAMqtt mqtt(client, device);    // Klient MQTT dla Home Assistant

// Serwer HTTP i WebSockets
ESP8266WebServer server(80);     // Tworzenie instancji serwera HTTP na porcie 80
WebSocketsServer webSocket(81);  // Tworzenie instancji serwera WebSockets na porcie 81

// Czujniki i przełączniki dla Home Assistant

// Przełączniki
HASwitch switchPumpAlarm("pump_alarm");  // Resetowania blokady pompy
HASwitch switchService("service_mode");  // Tryb serwisowy
HASwitch switchSound("sound_switch");    // Dźwięki systemu

HASwitch pumpSwitches[NUMBER_OF_PUMPS];  // Tablica przełączników pomp
HASensor sensorPump("pump_status");  // Sensor statusu pompy

// ** FUNKCJE I METODY SYSTEMOWE **

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
    if (currentMillis < lastMeasurement) lastMeasurement = 0;
    
    // Jeśli zbliża się przepełnienie, zresetuj wszystkie timery
    if (currentMillis > MILLIS_OVERFLOW_THRESHOLD) {
        status.pumpStartTime = 0;
        status.pumpDelayStartTime = 0;
        status.lastSoundAlert = 0;
        status.lastSuccessfulMeasurement = 0;
        lastMeasurement = 0;
        
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
    EEPROM.begin(sizeof(Config));
    EEPROM.get(0, config);

    // Sprawdź sumę kontrolną
    if (config.checksum != calculateChecksum(config)) {
        // Jeśli suma kontrolna się nie zgadza, ustaw domyślną konfigurację
        setDefaultConfig();
        return false;
    }
    return true;
}

// Zapis aktualnej konfiguracji do pamięci EEPROM
void saveConfig() {
    config.checksum = calculateChecksum(config);
    EEPROM.put(0, config);
    EEPROM.commit();
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
void setupPCF8574() {
    if (pcf8574.begin()) {
        for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
            pcf8574.digitalWrite(config.pumps[i].pcf8574_pin, HIGH);  // HIGH = pompa wyłączona
            status.pumps[i].isRunning = false;
            status.pumps[i].lastDose = 0;
            status.pumps[i].totalDosed = 0;
        }
    }
}

// Włączenie pompy
void turnOnPump(uint8_t pumpIndex) {
    #if DEBUG
    AQUA_DEBUG_PRINTF("Turning ON pump %d\n", pumpIndex);
    #endif
    pcf8574.digitalWrite(config.pumps[pumpIndex].pcf8574_pin, LOW);  // LOW = pompa włączona
    status.pumps[pumpIndex].isRunning = true;
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
    #if DEBUG
    AQUA_DEBUG_PRINTF("Turning OFF pump %d\n", pumpIndex);
    #endif
    pcf8574.digitalWrite(config.pumps[pumpIndex].pcf8574_pin, HIGH);  // HIGH = pompa wyłączona
    status.pumps[pumpIndex].isRunning = false;
}

// Bezpieczne wyłączenie wszystkich pomp
void stopAllPumps() {
    for(int i = 0; i < 8; i++) {
        turnOffPump(i);
    }
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

// Konfiguracja połączenia Wi-Fi
void setupWiFi() {
    WiFiManager wifiManager;

    // Reset WiFi settings if the button is pressed
    if (digitalRead(PRZYCISK_PIN) == LOW) {
        wifiManager.resetSettings();
        delay(1000);
    }

    wifiManager.autoConnect(config.hostname);

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Failed to connect to WiFi");
        ESP.restart();
    }

    Serial.println("Connected to WiFi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

// Połączenie z serwerem MQTT
bool connectMQTT() {   
    if (!mqtt.begin(config.mqtt_server, 1883, config.mqtt_user, config.mqtt_password)) {
        AQUA_DEBUG_PRINT("\nBŁĄD POŁĄCZENIA MQTT!");
        return false;
    }
    
    AQUA_DEBUG_PRINT("MQTT połączono pomyślnie!");
    return true;
}

// Konfiguracja MQTT z Home Assistant
void setupHA() {
    // Konfiguracja urządzenia dla Home Assistant
    device.setName("AquaDoser");  // Nazwa urządzenia
    device.setModel("AD ESP8266");  // Model urządzenia
    device.setManufacturer("PMW");  // Producent
    device.setSoftwareVersion(SOFTWARE_VERSION);  // Wersja oprogramowania

    // Configure Home Assistant entities
    for(uint8_t i = 0; i < NUM_PUMPS; i++) {
        String pumpName = String("pump_") + String(i + 1);
        pumpSwitches[i] = HASwitch(pumpName.c_str());  // Tworzenie przełącznika
        pumpSwitches[i].setName(pumpName.c_str());     // Ustawienie nazwy
        
        // Ustawienie callbacka z odpowiednim prototypem
        pumpSwitches[i].onCommand([i](bool state, HASwitch* sender) {
            onPumpCommand(state, sender, i);
        });
    }

    switchSound.setName("Dźwięk");
    switchSound.setIcon("mdi:volume-high");        // Ikona głośnika
    switchSound.onCommand(onSoundSwitchCommand);   // Funkcja obsługi zmiany stanu
    switchSound.setState(status.soundEnabled);      // Stan początkowy

    // Konfiguracja przełączników w HA
    switchService.setName("Serwis");
    switchService.setIcon("mdi:account-wrench-outline");            
    switchService.onCommand(onServiceSwitchCommand);  // Funkcja obsługi zmiany stanu
    switchService.setState(status.isServiceMode);  // Stan początkowy
    // Inicjalizacja stanu - domyślnie wyłączony
    status.isServiceMode = false;
    switchService.setState(false, true);  // force update przy starcie

    firstUpdateHA();
}

// ** FUNKCJE ZWIĄZANE Z PINAMI **

// Konfiguracja pinów wejścia/wyjścia
void setupPin() {   
    pinMode(PRZYCISK_PIN, INPUT_PULLUP);  // Wejście z podciąganiem - przycisk
    pinMode(BUZZER_PIN, OUTPUT);  // Wyjście - buzzer
    digitalWrite(BUZZER_PIN, LOW);  // Wyłączenie buzzera
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
        
    // Wymuś stan OFF na początku
    //sensorAlarm.setValue("OFF");
    switchSound.setState(false);  // Dodane - wymuś stan początkowy
    mqtt.loop();
    
    // Ustawienie końcowych stanów i wysyłka do HA
    switchSound.setState(status.soundEnabled);  // Dodane - ustaw aktualny stan dźwięku
    mqtt.loop();
}

// ** FUNKCJE ZWIĄZANE Z PRZYCISKIEM **

// Obsługa przycisku
void handleButton() {
    static unsigned long lastDebounceTime = 0;
    static bool lastReading = HIGH;
    const unsigned long DEBOUNCE_DELAY = 50;  // 50ms debounce

    bool reading = digitalRead(PRZYCISK_PIN);

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
                    if (status.isServiceMode && status.isPumpActive) {
                        pcf8574.digitalWrite(pumpPin, LOW);  // Wyłącz pompę
                        status.isPumpActive = false;  // Reset flagi aktywności
                        status.pumpStartTime = 0;  // Reset czasu startu
                        sensorPump.setValue("OFF");  // Aktualizacja w HA
                    }
                }
            }
        }
        
        // Obsługa długiego naciśnięcia (reset blokady pompy)
        if (reading == LOW && !buttonState.isLongPressHandled) {
            if (millis() - buttonState.pressedTime >= LONG_PRESS_TIME) {
                ESP.wdtFeed();  // Reset przy długim naciśnięciu
                status.pumpSafetyLock = false;  // Zdjęcie blokady pompy
                playConfirmationSound();  // Sygnał potwierdzenia zmiany trybu
                switchPumpAlarm.setState(false, true);  // force update w HA
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

void onPumpCommand(bool state, HASwitch* sender, int pumpIndex) {
    if (state) {
        dosePump(pumpIndex);
    } else {
        turnOffPump(pumpIndex);
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
        if (status.isPumpActive) {
            pcf8574.digitalWrite(pumpPin, LOW);  // Wyłączenie pompy
            status.isPumpActive = false;  // Reset flagi aktywności
            status.pumpStartTime = 0;  // Reset czasu startu
            sensorPump.setValue("OFF");  // Aktualizacja stanu w HA
        }
    } else {  // Wyłączanie trybu serwisowego
        // Reset stanu opóźnienia pompy aby umożliwić normalne uruchomienie
        status.isPumpDelayActive = false;
        status.pumpDelayStartTime = 0;
        // Normalny tryb pracy - pompa uruchomi się automatycznie 
        // jeśli czujnik poziomu wykryje wodę
    }
    
    AQUA_DEBUG_PRINTF("Tryb serwisowy: %s (przez HA)\n", state ? "WŁĄCZONY" : "WYŁĄCZONY");
}

// ** STRONA KONFIGURACYJNA **

// Strona konfiguracji przechowywana w pamięci programu
const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
  <html>
    <head>
        <meta charset='UTF-8'>
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>HydroSense</title>
        <style>
            body { 
                font-family: Arial, sans-serif; 
                margin: 0; 
                padding: 20px; 
                background-color: #1a1a1a;
                color: #ffffff;
            }

            .buttons-container {
                display: flex;
                justify-content: space-between;
                margin: -5px;
            }

            .container { 
                max-width: 800px; 
                margin: 0 auto; 
                padding: 0 15px;
            }

            .section {
                background-color: #2a2a2a;
                padding: 20px;
                margin-bottom: 20px;
                border-radius: 8px;
                width: 100%;
                box-sizing: border-box;
            }

            h1 { 
                color: #ffffff; 
                text-align: center;
                margin-bottom: 30px;
                font-size: 2.5em;
                background-color: #2d2d2d;
                padding: 20px;
                border-radius: 8px;
                box-shadow: 0 2px 4px rgba(0,0,0,0.2);
            }

            h2 { 
                color: #2196F3;
                margin-top: 0;
                font-size: 1.5em;
            }

            table { 
                width: 100%; 
                border-collapse: collapse;
            }

            td { 
                padding: 12px 8px;
                border-bottom: 1px solid #3d3d3d;
            }

            .config-table td {
                padding: 8px;
            }

            .config-table {
                width: 100%;
                border-collapse: collapse;
                table-layout: fixed;
            }

            .config-table td:first-child {
                width: 65%;
            }

            .config-table td:last-child {
                width: 35%;
            }
            .config-table input[type="text"],
            .config-table input[type="password"],
            .config-table input[type="number"] {
                width: 100%;
                padding: 8px;
                border: 1px solid #3d3d3d;
                border-radius: 4px;
                background-color: #1a1a1a;
                color: #ffffff;
                box-sizing: border-box;
            }

            input[type="text"],
            input[type="password"],
            input[type="number"] {
                width: 100%;
                padding: 8px;
                border: 1px solid #3d3d3d;
                border-radius: 4px;
                background-color: #1a1a1a;
                color: #ffffff;
                box-sizing: border-box;
                text-align: left;
            }

            input[type="submit"] { 
                background-color: #4CAF50; 
                color: white; 
                padding: 12px 24px; 
                border: none; 
                border-radius: 4px; 
                cursor: pointer;
                width: 100%;
                font-size: 16px;
            }

            input[type="submit"]:hover { 
                background-color: #45a049; 
            }

            input[type="file"] {
                background-color: #1a1a1a;
                color: #ffffff;
                padding: 8px;
                border: 1px solid #3d3d3d;
                border-radius: 4px;
                width: 100%;
                cursor: pointer;
            }

            input[type="file"]::-webkit-file-upload-button {
                background-color: #2196F3;
                color: white;
                padding: 8px 16px;
                border: none;
                border-radius: 4px;
                cursor: pointer;
                margin-right: 10px;
            }

            input[type="file"]::-webkit-file-upload-button:hover {
                background-color: #1976D2;
            }

            .success { 
                color: #4CAF50; 
            }

            .error { 
                color: #F44336;
            }

            .alert { 
                padding: 15px; 
                margin-bottom: 20px; 
                border-radius: 0;
                position: fixed;
                top: 0;
                left: 0;
                right: 0;
                width: 100%;
                z-index: 1000;
                text-align: center;
                animation: fadeOut 0.5s ease-in-out 5s forwards;
            }
            .alert.success { 
                background-color: #4CAF50;
                color: white;
                border: none;
                box-shadow: 0 2px 5px rgba(0,0,0,0.2);
            }

            .btn {
                padding: 12px 24px;
                border: none;
                border-radius: 4px;
                cursor: pointer;
                font-size: 14px;
                width: calc(50% - 10px);
                display: inline-block;
                margin: 5px;
                text-align: center;
            }

            .btn-blue { 
                background-color: #2196F3;
                color: white; 
            }

            .btn-red { 
                background-color: #F44336;
                color: white; 
            }

            .btn-orange {
                background-color: #FF9800 !important;
                color: white !important;
            }

            .btn-orange:hover {
                background-color: #F57C00 !important;
            }

            .console {
                background-color: #1a1a1a;
                color: #ffffff;
                padding: 15px;
                border-radius: 4px;
                font-family: monospace;
                height: 200px;
                overflow-y: auto;
                margin-top: 10px;
                border: 1px solid #3d3d3d;
            }

            .footer {
                background-color: #2d2d2d;
                padding: 20px;
                margin-top: 20px;
                border-radius: 8px;
                text-align: center;
                box-shadow: 0 2px 4px rgba(0,0,0,0.2);
            }

            .footer a {
                display: inline-block;
                background-color: #2196F3;
                color: white;
                text-decoration: underline;
                padding: 12px 24px;
                border-radius: 4px;
                font-weight: normal;
                transition: background-color 0.3s;
                width: 100%;
                box-sizing: border-box;
            }

            .footer a:hover {
                background-color: #1976D2;
            }

            .progress {
                width: 100%;
                background-color: #1a1a1a;
                border: 1px solid #3d3d3d;
                border-radius: 4px;
                padding: 3px;
                margin-top: 15px;
            }
            .progress-bar {
                width: 0%;
                height: 20px;
                background-color: #4CAF50;
                border-radius: 2px;
                transition: width 0.3s ease-in-out;
                text-align: center;
                color: white;
                line-height: 20px;
            }

            .message {
                position: fixed;
                top: 20px;
                left: 50%;
                transform: translateX(-50%);
                padding: 15px 30px;
                border-radius: 5px;
                color: white;
                opacity: 0;
                transition: opacity 0.3s ease-in-out;
                z-index: 1000;
            }

            .message.success {
                background-color: #4CAF50;
                box-shadow: 0 2px 5px rgba(0,0,0,0.2);
            }

            .message.error {
                background-color: #f44336;
                box-shadow: 0 2px 5px rgba(0,0,0,0.2);
            }

            @media (max-width: 600px) {
                body {
                    padding: 10px;
                }
                .container {
                    padding: 0;
                }
                .section {
                    padding: 15px;
                    margin-bottom: 15px;
                }
                .console {
                    height: 150px;
                }
            }
        </style>
        <script>
            function confirmReset() {
                return confirm('Czy na pewno chcesz przywrócić ustawienia fabryczne? Spowoduje to utratę wszystkich ustawień.');
            }

            function rebootDevice() {
                if(confirm('Czy na pewno chcesz zrestartować urządzenie?')) {
                    fetch('/reboot', {method: 'POST'}).then(() => {
                        showMessage('Urządzenie zostanie zrestartowane...', 'success');
                        setTimeout(() => { window.location.reload(); }, 3000);
                    });
                }
            }

            function factoryReset() {
                if(confirmReset()) {
                    fetch('/factory-reset', {method: 'POST'}).then(() => {
                        showMessage('Przywracanie ustawień fabrycznych...', 'success');
                        setTimeout(() => { window.location.reload(); }, 3000);
                    });
                }
            }
            function showMessage(text, type) {
                var oldMessages = document.querySelectorAll('.message');
                oldMessages.forEach(function(msg) {
                    msg.remove();
                });
                
                var messageBox = document.createElement('div');
                messageBox.className = 'message ' + type;
                messageBox.innerHTML = text;
                document.body.appendChild(messageBox);
                
                setTimeout(function() {
                    messageBox.style.opacity = '1';
                }, 10);
                
                setTimeout(function() {
                    messageBox.style.opacity = '0';
                    setTimeout(function() {
                        messageBox.remove();
                    }, 300);
                }, 3000);
            }

            var socket;
            window.onload = function() {
                document.querySelector('form[action="/save"]').addEventListener('submit', function(e) {
                    e.preventDefault();
                    
                    var formData = new FormData(this);
                    fetch('/save', {
                        method: 'POST',
                        body: formData
                    }).then(response => {
                        if (!response.ok) {
                            throw new Error('Network response was not ok');
                        }
                    }).catch(error => {
                        showMessage('Błąd podczas zapisywania!', 'error');
                    });
                });

                socket = new WebSocket('ws://' + window.location.hostname + ':81/');
                socket.onmessage = function(event) {
                    var message = event.data;
                    
                    if (message.startsWith('update:')) {
                        if (message.startsWith('update:error:')) {
                            document.getElementById('update-progress').style.display = 'none';
                            showMessage(message.split(':')[2], 'error');
                            return;
                        }
                        var percentage = message.split(':')[1];
                        document.getElementById('update-progress').style.display = 'block';
                        document.getElementById('progress-bar').style.width = percentage + '%';
                        document.getElementById('progress-bar').textContent = percentage + '%';
                        
                        if (percentage == '100') {
                            document.getElementById('update-progress').style.display = 'none';
                            showMessage('Aktualizacja zakończona pomyślnie! Trwa restart urządzenia...', 'success');
                            setTimeout(function() {
                                window.location.reload();
                            }, 3000);
                        }
                    } 
                    else if (message.startsWith('save:')) {
                        var parts = message.split(':');
                        var type = parts[1];
                        var text = parts[2];
                        showMessage(text, type);
                    }
                    else {
                        var console = document.getElementById('console');
                        console.innerHTML += message + '<br>';
                        console.scrollTop = console.scrollHeight;
                    }
                };
            };
        </script>
    </head>
    <body>
        <h1>HydroSense</h1>
        
        <div class='section'>
            <h2>Status systemu</h2>
            <table class='config-table'>
                <tr>
                    <td>Status MQTT</td>
                    <td><span class='status %MQTT_STATUS_CLASS%'>%MQTT_STATUS%</span></td>
                </tr>
                <tr>
                    <td>Status dźwięku</td>
                    <td><span class='status %SOUND_STATUS_CLASS%'>%SOUND_STATUS%</span></td>
                </tr>
                <tr>
                    <td>Wersja oprogramowania</td>
                    <td>%SOFTWARE_VERSION%</td>
                </tr>
            </table>
        </div>

        %BUTTONS%
        %CONFIG_FORMS%
        %UPDATE_FORM%
        %FOOTER%

    </body>
  </html>
)rawliteral";

// Formularz do aktualizacji firmware przechowywany w pamięci programu
const char UPDATE_FORM[] PROGMEM = R"rawliteral(
<div class='section'>
    <h2>Aktualizacja firmware</h2>
    <form method='POST' action='/update' enctype='multipart/form-data'>
        <table class='config-table' style='margin-bottom: 15px;'>
            <tr><td colspan='2'><input type='file' name='update' accept='.bin'></td></tr>
        </table>
        <input type='submit' value='Aktualizuj firmware' class='btn btn-orange'>
    </form>
    <div id='update-progress' style='display:none'>
        <div class='progress'>
            <div id='progress-bar' class='progress-bar' role='progressbar' style='width: 0%'>0%</div>
        </div>
    </div>
</div>
)rawliteral";

// Strona konfiguracji przechowywana w pamięci programu
const char PAGE_FOOTER[] PROGMEM = R"rawliteral(
<div class='footer'>
    <a href='https://github.com/pimowo/HydroSense' target='_blank'>Project by PMW</a>
</div>
)rawliteral";

// Zwraca zawartość strony konfiguracji jako ciąg znaków
String getConfigPage() {
    String html = FPSTR(CONFIG_PAGE);
    
    // Przygotuj wszystkie wartości przed zastąpieniem
    bool mqttConnected = client.connected();
    String mqttStatus = mqttConnected ? "Połączony" : "Rozłączony";
    String mqttStatusClass = mqttConnected ? "success" : "error";
    String soundStatus = config.soundEnabled ? "Włączony" : "Wyłączony";
    String soundStatusClass = config.soundEnabled ? "success" : "error";
    
    // Sekcja przycisków - zmieniona na String zamiast PROGMEM
    String buttons = F(
        "<div class='section'>"
        "<div class='buttons-container'>"
        "<button class='btn btn-blue' onclick='rebootDevice()'>Restart urządzenia</button>"
        "<button class='btn btn-red' onclick='factoryReset()'>Przywróć ustawienia fabryczne</button>"
        "</div>"
        "</div>"
    );

    // Zastąp wszystkie placeholdery
    html.replace("%MQTT_SERVER%", config.mqtt_server);
    html.replace("%MQTT_PORT%", String(config.mqtt_port));
    html.replace("%MQTT_USER%", config.mqtt_user);
    html.replace("%MQTT_PASSWORD%", config.mqtt_password);
    html.replace("%MQTT_STATUS%", mqttStatus);
    html.replace("%MQTT_STATUS_CLASS%", mqttStatusClass);
    html.replace("%SOUND_STATUS%", soundStatus);
    html.replace("%SOUND_STATUS_CLASS%", soundStatusClass);
    html.replace("%SOFTWARE_VERSION%", SOFTWARE_VERSION);
    html.replace("%BUTTONS%", buttons);
    html.replace("%UPDATE_FORM%", FPSTR(UPDATE_FORM));
    html.replace("%FOOTER%", FPSTR(PAGE_FOOTER));
    html.replace("%MESSAGE%", "");

    // Przygotuj formularze konfiguracyjne
    String configForms = F("<form method='POST' action='/save'>");
    
    // MQTT
    configForms += F("<div class='section'>"
                     "<h2>Konfiguracja MQTT</h2>"
                     "<table class='config-table'>"
                     "<tr><td>Serwer</td><td><input type='text' name='mqtt_server' value='");
    configForms += config.mqtt_server;
    configForms += F("'></td></tr>"
                     "<tr><td>Port</td><td><input type='number' name='mqtt_port' value='");
    configForms += String(config.mqtt_port);
    configForms += F("'></td></tr>"
                     "<tr><td>Użytkownik</td><td><input type='text' name='mqtt_user' value='");
    configForms += config.mqtt_user;
    configForms += F("'></td></tr>"
                     "<tr><td>Hasło</td><td><input type='password' name='mqtt_password' value='");
    configForms += config.mqtt_password;
    configForms += F("'></td></tr>"
                     "</table></div>");
    
    // // Zbiornik
    // configForms += F("<div class='section'>"
    //                  "<h2>Ustawienia zbiornika</h2>"
    //                  "<table class='config-table'>");
    // configForms += "<tr><td>Odległość przy pustym [mm]</td><td><input type='number' name='tank_empty' value='" + String(config.tank_empty) + "'></td></tr>";
    // configForms += "<tr><td>Odległość przy pełnym [mm]</td><td><input type='number' name='tank_full' value='" + String(config.tank_full) + "'></td></tr>";
    // configForms += "<tr><td>Odległość przy rezerwie [mm]</td><td><input type='number' name='reserve_level' value='" + String(config.reserve_level) + "'></td></tr>";
    // configForms += "<tr><td>Średnica zbiornika [mm]</td><td><input type='number' name='tank_diameter' value='" + String(config.tank_diameter) + "'></td></tr>";
    // configForms += F("</table></div>");
    
    // // Pompa
    // configForms += F("<div class='section'>"
    //                  "<h2>Ustawienia pompy</h2>"
    //                  "<table class='config-table'>");
    // configForms += "<tr><td>Opóźnienie załączenia pompy [s]</td><td><input type='number' name='pump_delay' value='" + String(config.pump_delay) + "'></td></tr>";
    // configForms += "<tr><td>Czas pracy pompy [s]</td><td><input type='number' name='pump_work_time' value='" + String(config.pump_work_time) + "'></td></tr>";
    // configForms += F("</table></div>");
    
    configForms += F("<div class='section'>"
                     "<input type='submit' value='Zapisz ustawienia' class='btn btn-blue'>"
                     "</div></form>");
    
    html.replace("%CONFIG_FORMS%", configForms);
    
    // Sprawdź, czy wszystkie znaczniki zostały zastąpione
    if (html.indexOf('%') != -1) {
        AQUA_DEBUG_PRINTF("Uwaga: Niektóre znaczniki nie zostały zastąpione!");
        int pos = html.indexOf('%');
        AQUA_DEBUG_PRINTF(html.substring(pos - 20, pos + 20));
    }
    
    return html;
}

// Obsługa żądania HTTP do głównej strony konfiguracji
void handleRoot() {
    String content = getConfigPage();

    server.send(200, "text/html", content);
}

// Waliduje wartości konfiguracji wprowadzone przez użytkownika
bool validateConfigValues() {   
    // Walidacja ustawień pomp
    for (int i = 0; i < 8; i++) {
        float dose = server.arg("p" + String(i) + "_dose").toFloat();
        if (dose < 0 || dose > 1000) return false;  // Maksymalna dawka 1L
        
        String timeStr = server.arg("p" + String(i) + "_time");
        int hour = timeStr.substring(0, 2).toInt();
        int minute = timeStr.substring(3, 5).toInt();
        
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    }
    
    return true;
}

// Obsługa zapisania konfiguracji po jej wprowadzeniu przez użytkownika
void handleSave() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

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


    // Zapisz konfigurację pomp
    for (int i = 0; i < 8; i++) {
        String pumpPrefix = "p" + String(i);
        config.pumps[i].enabled = server.hasArg(pumpPrefix + "_enabled");
        
        if (Server.hasArg(pumpPrefix + "_dose")) {
            config.pumps[i].dosage = server.arg(pumpPrefix + "_dose").toFloat();
        }
        
        if (server.hasArg(pumpPrefix + "_time")) {
            String timeStr = server.arg(pumpPrefix + "_time");
            if (timeStr.length() >= 5) { // Format HH:MM
                config.pumps[i].hour = timeStr.substring(0, 2).toInt();
                config.pumps[i].minute = timeStr.substring(3, 5).toInt();
            }
        }
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
    if (needMqttReconnect) {
        if (mqtt.isConnected()) {
            mqtt.disconnect();
        }
        connectMQTT();
    }

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
    
    server.begin();
}

// ** WEBSOCKET I KOMUNIKACJA W SIECI **

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    // Obsługujemy tylko połączenie - reszta nie jest potrzebna
    if (type == WStype_CONNECTED) {
        Serial.printf("[%u] Connected\n", num);
    }
}

// ** Funkcja setup - inicjalizacja urządzeń i konfiguracja **

void setup() {
    Serial.begin(115200);
    Serial.println("\nAquaDoser Starting...");
    
    EEPROM.begin(sizeof(Config));
    
    // Inicjalizacja pinu buzzera (jeśli używany)
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    
    // Wczytaj konfigurację
    if (!loadConfig()) {
        Serial.println("Ładowanie konfiguracji domyślnej");
    }
    
        // Inicjalizacja komponentów
    setupWiFi();
    if (!connectMQTT()) {
        Serial.println("Błąd połączenia z MQTT, restart urządzenia.");
        ESP.restart();
    }
    
    setupPCF8574();  // Inicjalizacja PCF8574
    setupWiFi();     // Konfiguracja WiFi
    setupHA();       // Konfiguracja Home Assistant
    setupWebServer();// Konfiguracja serwera WWW
    
    // Konfiguracja OTA
    ArduinoOTA.setHostname("AquaDoser");  // Ustaw nazwę urządzenia
    ArduinoOTA.setPassword("aquadoser");  // Ustaw hasło dla OTA
    ArduinoOTA.begin();  // Uruchom OTA   

    welcomeMelody();
}

// ** Funkcja loop - główny cykl pracy urządzenia **

void loop() {
    unsigned long currentMillis = millis();
    
    // Obsługa przepełnienia licznika millis()
    handleMillisOverflow();
    
    // Obsługa MQTT
    if (!mqtt.isConnected()) {
        connectMQTT();
    }
    mqtt.loop();
    
    // Obsługa serwera WWW
    server.handleClient();
    
    // Sprawdzenie stanu pomp i bezpieczeństwa
    for(int i = 0; i < 8; i++) {
        if (status.pumps[i].isRunning) {
            // Tutaj później dodamy logikę czasu dozowania
        }
    }
    
    // Dodatkowe zabezpieczenie - wyłącz wszystkie pompy w trybie serwisowym
    if (status.isServiceMode) {
        stopAllPumps();
    }

    if (currentMillis - timers.lastOTACheck >= OTA_CHECK_INTERVAL) {
        ArduinoOTA.handle();                  // Obsługa aktualizacji OTA
        timers.lastOTACheck = currentMillis;  // Aktualizacja znacznika czasu ostatniego sprawdzenia OTA
    }
}
