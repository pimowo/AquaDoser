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
const int NUMBER_OF_PUMPS = 8;
const int BUZZER_PIN = 13;    // GPIO 13
const int LED_PIN = 12;    // GPIO 12
const int PRZYCISK_PIN = 14;    // GPIO 14
const int SDA_PIN = 4;        // GPIO 4
const int SCL_PIN = 5;        // GPIO 5
const int PCF8574_ADDRESS = 0x20;  // Adres I2C: 0x20

// ** USTAWIENIA CZASOWE **

// Konfiguracja timeoutów i interwałów
const unsigned long WATCHDOG_TIMEOUT = 8000;       // Timeout dla watchdoga
const unsigned long LONG_PRESS_TIME = 1000;        // Czas długiego naciśnięcia przycisku
const unsigned long MQTT_LOOP_INTERVAL = 100;      // Obsługa MQTT co 100ms
const unsigned long OTA_CHECK_INTERVAL = 1000;     // Sprawdzanie OTA co 1s
const unsigned long MQTT_RETRY_INTERVAL = 10000;   // Próba połączenia MQTT co 10s
const unsigned long MILLIS_OVERFLOW_THRESHOLD = 4294967295U - 60000; // ~49.7 dni

// ** KONFIGURACJA SYSTEMU **

// Makra debugowania
#define DEBUG 1  // 0 wyłącza debug, 1 włącza debug

#if DEBUG
    #define DEBUG_PRINT(x) Serial.println(x)
    #define DEBUG_PRINTF(format, ...) Serial.printf(format, __VA_ARGS__)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTF(format, ...)
#endif

// Zmienna przechowująca wersję oprogramowania
const char* SOFTWARE_VERSION = "2.12.24";  // Definiowanie wersji oprogramowania

// Struktury konfiguracyjne i statusowe

// Struktura dla pojedynczej pompy
struct PumpConfig {
    bool enabled;                // czy pompa jest włączona w harmonogramie
    uint8_t dosage_ml;          // dawka w ml
    uint8_t hour;               // godzina dozowania
    uint8_t minute;             // minuta dozowania
    bool workdays_only;         // czy dozować tylko w dni robocze
};

// Konfiguracja
// Główna struktura konfiguracji
struct Config {
    char hostname[32];          // nazwa urządzenia w sieci
    char mqttHost[64];         // adres serwera MQTT
    int mqttPort;              // port MQTT
    char mqttUser[32];         // nazwa użytkownika MQTT
    char mqttPassword[32];     // hasło MQTT
    bool soundEnabled;         // czy dźwięki są włączone
    PumpConfig pumps[NUMBER_OF_PUMPS];  // konfiguracja dla każdej pompy
    uint8_t configVersion;     // wersja konfiguracji (dla EEPROM)
    char checksum;             // suma kontrolna konfiguracji
};

// Struktura do przechowywania różnych stanów i parametrów systemu
struct Status {
    bool mqttConnected;        // status połączenia z MQTT
    bool pumpActive[NUMBER_OF_PUMPS];  // aktualny stan pomp (włączona/wyłączona)
    bool serviceMode;          // czy urządzenie jest w trybie serwisowym
    unsigned long lastDose[NUMBER_OF_PUMPS];  // timestamp ostatniego dozowania
    float totalDosed[NUMBER_OF_PUMPS];  // całkowita ilość dozowana (ml)
    bool networkConnected;     // status połączenia z siecią
    uint8_t errorCode;        // kod błędu (0 = brak błędu)
};

// Stan przycisku
struct ButtonState {
    bool isPressed;           // czy przycisk jest obecnie wciśnięty
    unsigned long pressTime;  // czas początku wciśnięcia
    bool wasLongPress;       // czy ostatnie wciśnięcie było długie
    uint8_t clickCount;      // licznik szybkich kliknięć
    unsigned long lastClickTime; // czas ostatniego kliknięcia
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
HADevice device("HydroSense");  // Definicja urządzenia dla Home Assistant
HAMqtt mqtt(client, device);    // Klient MQTT dla Home Assistant

// Serwer HTTP i WebSockets
ESP8266WebServer server(80);     // Tworzenie instancji serwera HTTP na porcie 80
WebSocketsServer webSocket(81);  // Tworzenie instancji serwera WebSockets na porcie 81

// Czujniki i przełączniki dla Home Assistant

// Sensory pomiarowe
HASensor sensorDistance("water_level");         // Odległość od lustra wody (w mm)
HASensor sensorLevel("water_level_percent");    // Poziom wody w zbiorniku (w procentach)
HASensor sensorVolume("water_volume");          // Objętość wody (w litrach)
HASensor sensorPumpWorkTime("pump_work_time");  // Czas pracy pompy

// Sensory statusu
HASensor sensorPump("pump");    // Praca pompy (ON/OFF)
HASensor sensorWater("water");  // Czujnik poziomu w akwarium (ON=niski/OFF=ok)

// Sensory alarmowe
HASensor sensorAlarm("water_alarm");      // Brak wody w zbiorniku dolewki
HASensor sensorReserve("water_reserve");  // Rezerwa w zbiorniku dolewki

// Przełączniki
HASwitch switchPumpAlarm("pump_alarm");  // Resetowania blokady pompy
HASwitch switchService("service_mode");  // Tryb serwisowy
HASwitch switchSound("sound_switch");    // Dźwięki systemu

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
        
        DEBUG_PRINT(F("Reset timerów - zbliża się przepełnienie millis()"));
    }
}

// Ustawienia domyślne konfiguracji
void setDefaultConfig() {
    strlcpy(config.hostname, "aquadoser", sizeof(config.hostname));
    strlcpy(config.mqttHost, "homeassistant.local", sizeof(config.mqttHost));
    config.mqttPort = 1883;
    strlcpy(config.mqttUser, "", sizeof(config.mqttUser));
    strlcpy(config.mqttPass, "", sizeof(config.mqttPass));
    config.soundEnabled = true;
    
    // Domyślna konfiguracja dla wszystkich pomp
    for(int i = 0; i < 8; i++) {
        config.pumps[i].enabled = false;
        config.pumps[i].dosage = 1.0;      // 1ml domyślna dawka
        config.pumps[i].calibration = 60.0; // 60ml/min domyślna kalibracja
        config.pumps[i].hour = 12;          // domyślnie 12:00
        config.pumps[i].minute = 0;
    }
    
    config.checksum = calculateChecksum(config);
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
    
    // Sprawdź sumę kontrolną
    char calculatedChecksum = calculateChecksum(tempConfig);
    if (calculatedChecksum == tempConfig.checksum) {
        // Jeśli suma kontrolna się zgadza, skopiuj dane do głównej struktury config
        memcpy(&config, &tempConfig, sizeof(Config));
        DEBUG_PRINTF("Konfiguracja wczytana pomyślnie");
        return true;
    } else {
        DEBUG_PRINTF("Błąd sumy kontrolnej - ładowanie ustawień domyślnych");
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
        DEBUG_PRINTF("Konfiguracja zapisana pomyślnie");
    } else {
        DEBUG_PRINTF("Błąd zapisu konfiguracji!");
    }
}

// Oblicz sumę kontrolną dla danej konfiguracji
char calculateChecksum(const Config& cfg) {
    const byte* data = (const byte*)&cfg;
    char checksum = 0;
    
    // Obliczamy sumę kontrolną dla wszystkich danych oprócz ostatniego bajtu (który jest samą sumą kontrolną)
    for (size_t i = 0; i < sizeof(Config) - 1; i++) {
        checksum ^= data[i];
    }
    
    return checksum;
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
    Wire.begin();  // Inicjalizacja I2C (SDA - D2, SCL - D1 dla ESP8266)
    
    if (pcf8574.begin()) {
        Serial.println("PCF8574 zainicjowany");
        
        // Wyłącz wszystkie pompy na starcie
        for(int i = 0; i < 8; i++) {
            pcf8574.write(i, HIGH);  // HIGH = pompa wyłączona
            status.pumps[i].isRunning = false;
            status.pumps[i].lastDose = 0;
            status.pumps[i].totalDosed = 0;
        }
    } else {
        Serial.println("Błąd inicjalizacji PCF8574!");
    }
}

// Włączenie pompy
void turnOnPump(uint8_t pumpIndex) {
    if (pumpIndex >= 8) return;
    
    if (pcf8574.write(pumpIndex, LOW)) {  // LOW = pompa włączona
        status.pumps[pumpIndex].isRunning = true;
    } else {
        Serial.printf("Błąd włączania pompy %d\n", pumpIndex + 1);
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
    if (pumpIndex >= 8) return;
    
    if (pcf8574.write(pumpIndex, HIGH)) {  // HIGH = pompa wyłączona
        status.pumps[pumpIndex].isRunning = false;
    } else {
        Serial.printf("Błąd wyłączania pompy %d\n", pumpIndex + 1);
    }
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
    DEBUG_PRINT(F("Rozpoczynam kasowanie ustawień WiFi..."));
    
    // Najpierw rozłącz WiFi i wyczyść wszystkie zapisane ustawienia
    WiFi.disconnect(false, true);  // false = nie wyłączaj WiFi, true = kasuj zapisane ustawienia
    
    // Upewnij się, że WiFi jest w trybie stacji
    WiFi.mode(WIFI_STA);
    
    // Reset przez WiFiManager
    WiFiManager wm;
    wm.resetSettings();
    
    DEBUG_PRINT(F("Ustawienia WiFi zostały skasowane"));
    delay(100);
}

// Konfiguracja połączenia Wi-Fi
void setupWiFi() {
    WiFiManager wifiManager;
       
    wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
        DEBUG_PRINT("Tryb punktu dostępowego");
        DEBUG_PRINT("SSID: HydroSense");
        DEBUG_PRINT("IP: 192.168.4.1");
    });
    
    wifiManager.setConfigPortalTimeout(180); // 3 minuty na konfigurację
    
    // Próba połączenia lub utworzenia AP
    if (!wifiManager.autoConnect("HydroSense", "hydrosense")) {
        DEBUG_PRINT("Nie udało się połączyć i timeout upłynął");
        ESP.restart(); // Restart ESP w przypadku niepowodzenia
    }
    
    DEBUG_PRINT("Połączono z WiFi!");
    
    // Pokaż uzyskane IP
    DEBUG_PRINT("IP: ");
    DEBUG_PRINT(WiFi.localIP().toString().c_str());
}

// Połączenie z serwerem MQTT
bool connectMQTT() {   
    if (!mqtt.begin(config.mqtt_server, 1883, config.mqtt_user, config.mqtt_password)) {
        DEBUG_PRINT("\nBŁĄD POŁĄCZENIA MQTT!");
        return false;
    }
    
    DEBUG_PRINT("MQTT połączono pomyślnie!");
    return true;
}

// Konfiguracja MQTT z Home Assistant
void setupHA() {
    // Konfiguracja urządzenia dla Home Assistant
    device.setName("AquaDoser");  // Nazwa urządzenia
    device.setModel("AD ESP8266");  // Model urządzenia
    device.setManufacturer("PMW");  // Producent
    device.setSoftwareVersion(SOFTWARE_VERSION);  // Wersja oprogramowania

    for (int i = 0; i < 8; i++) {
        char entityName[32];
        sprintf(entityName, "Pump %d", i + 1);
        
        // Utworzenie przełącznika dla każdej pompy
        HASwitch* pump = new HASwitch(entityName);
        pump->setName(entityName);
        pump->setIcon("mdi:water-pump");
        
        // Licznik całkowitej ilości dozowanej
        char sensorName[32];
        sprintf(sensorName, "Pump %d Total", i + 1);
        HASensor* total = new HASensor(sensorName);
        total->setName(sensorName);
        total->setIcon("mdi:beaker");
        total->setUnitOfMeasurement("ml");
    } 
    
    // Konfiguracja przełączników w HA
    switchService.setName("Serwis");
    switchService.setIcon("mdi:account-wrench-outline");            
    switchService.onCommand(onServiceSwitchCommand);  // Funkcja obsługi zmiany stanu
    switchService.setState(status.isServiceMode);  // Stan początkowy
    // Inicjalizacja stanu - domyślnie wyłączony
    status.isServiceMode = false;
    switchService.setState(false, true);  // force update przy starcie

    switchSound.setName("Dźwięk");
    switchSound.setIcon("mdi:volume-high");        // Ikona głośnika
    switchSound.onCommand(onSoundSwitchCommand);   // Funkcja obsługi zmiany stanu
    switchSound.setState(status.soundEnabled);      // Stan początkowy
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
    float initialDistance = measureDistance();
    
    // Ustaw początkowe stany na podstawie pomiaru
    status.waterAlarmActive = (initialDistance >= config.tank_empty);
    status.waterReserveActive = (initialDistance >= config.reserve_level);
    
    // Wymuś stan OFF na początku
    sensorAlarm.setValue("OFF");
    sensorReserve.setValue("OFF");
    switchSound.setState(false);  // Dodane - wymuś stan początkowy
    mqtt.loop();
    
    // Ustawienie końcowych stanów i wysyłka do HA
    sensorAlarm.setValue(status.waterAlarmActive ? "ON" : "OFF");
    sensorReserve.setValue(status.waterReserveActive ? "ON" : "OFF");
    switchSound.setState(status.soundEnabled);  // Dodane - ustaw aktualny stan dźwięku
    mqtt.loop();

    sensorPumpWorkTime.setValue("0");
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
                    DEBUG_PRINTF("Tryb serwisowy: %s (przez przycisk)\n", status.isServiceMode ? "WŁĄCZONY" : "WYŁĄCZONY");
                    
                    // Jeśli włączono tryb serwisowy podczas pracy pompy
                    if (status.isServiceMode && status.isPumpActive) {
                        digitalWrite(POMPA_PIN, LOW);  // Wyłącz pompę
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
                DEBUG_PRINT("Alarm pompy skasowany");
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
    
    DEBUG_PRINTF("Zmieniono stan dźwięku na: ", state ? "WŁĄCZONY" : "WYŁĄCZONY");
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
            digitalWrite(POMPA_PIN, LOW);  // Wyłączenie pompy
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
    
    DEBUG_PRINTF("Tryb serwisowy: %s (przez HA)\n", state ? "WŁĄCZONY" : "WYŁĄCZONY");
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
    html.replace("%TANK_EMPTY%", String(config.tank_empty));
    html.replace("%TANK_FULL%", String(config.tank_full));
    html.replace("%RESERVE_LEVEL%", String(config.reserve_level));
    html.replace("%TANK_DIAMETER%", String(config.tank_diameter));
    html.replace("%PUMP_DELAY%", String(config.pump_delay));
    html.replace("%PUMP_WORK_TIME%", String(config.pump_work_time));
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
    
    // Zbiornik
    configForms += F("<div class='section'>"
                     "<h2>Ustawienia zbiornika</h2>"
                     "<table class='config-table'>");
    configForms += "<tr><td>Odległość przy pustym [mm]</td><td><input type='number' name='tank_empty' value='" + String(config.tank_empty) + "'></td></tr>";
    configForms += "<tr><td>Odległość przy pełnym [mm]</td><td><input type='number' name='tank_full' value='" + String(config.tank_full) + "'></td></tr>";
    configForms += "<tr><td>Odległość przy rezerwie [mm]</td><td><input type='number' name='reserve_level' value='" + String(config.reserve_level) + "'></td></tr>";
    configForms += "<tr><td>Średnica zbiornika [mm]</td><td><input type='number' name='tank_diameter' value='" + String(config.tank_diameter) + "'></td></tr>";
    configForms += F("</table></div>");
    
    // Pompa
    configForms += F("<div class='section'>"
                     "<h2>Ustawienia pompy</h2>"
                     "<table class='config-table'>");
    configForms += "<tr><td>Opóźnienie załączenia pompy [s]</td><td><input type='number' name='pump_delay' value='" + String(config.pump_delay) + "'></td></tr>";
    configForms += "<tr><td>Czas pracy pompy [s]</td><td><input type='number' name='pump_work_time' value='" + String(config.pump_work_time) + "'></td></tr>";
    configForms += F("</table></div>");
    
    configForms += F("<div class='section'>"
                     "<input type='submit' value='Zapisz ustawienia' class='btn btn-blue'>"
                     "</div></form>");
    
    html.replace("%CONFIG_FORMS%", configForms);
    
    // Sprawdź, czy wszystkie znaczniki zostały zastąpione
    if (html.indexOf('%') != -1) {
        DEBUG_PRINTF("Uwaga: Niektóre znaczniki nie zostały zastąpione!");
        int pos = html.indexOf('%');
        DEBUG_PRINTF(html.substring(pos - 20, pos + 20));
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
    if (webServer.arg("hostname").length() >= sizeof(config.hostname)) return false;
    if (webServer.arg("mqtt_host").length() >= sizeof(config.mqttHost)) return false;
    if (webServer.arg("mqtt_user").length() >= sizeof(config.mqttUser)) return false;
    if (webServer.arg("mqtt_pass").length() >= sizeof(config.mqttPass)) return false;
    
    // Walidacja ustawień pomp
    for (int i = 0; i < 8; i++) {
        float dose = webServer.arg("p" + String(i) + "_dose").toFloat();
        if (dose < 0 || dose > 1000) return false;  // Maksymalna dawka 1L
        
        String timeStr = webServer.arg("p" + String(i) + "_time");
        int hour = timeStr.substring(0, 2).toInt();
        int minute = timeStr.substring(3, 5).toInt();
        
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    }
    
    return true;
}

// Obsługa zapisania konfiguracji po jej wprowadzeniu przez użytkownika
void handleSave() {
    if (webServer.method() != HTTP_POST) {
        webServer.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    // Zapisz poprzednie wartości na wypadek błędów
    Config oldConfig = config;
    bool needMqttReconnect = false;

    // Zapisz poprzednie wartości MQTT do porównania
    String oldHost = config.mqttHost;
    int oldPort = config.mqttPort;
    String oldUser = config.mqttUser;
    String oldPass = config.mqttPass;

    // Zapisz podstawowe ustawienia
    if (webServer.hasArg("hostname")) {
        strlcpy(config.hostname, webServer.arg("hostname").c_str(), sizeof(config.hostname));
    }
    if (webServer.hasArg("mqtt_host")) {
        strlcpy(config.mqttHost, webServer.arg("mqtt_host").c_str(), sizeof(config.mqttHost));
    }
    if (webServer.hasArg("mqtt_port")) {
        config.mqttPort = webServer.arg("mqtt_port").toInt();
    }
    if (webServer.hasArg("mqtt_user")) {
        strlcpy(config.mqttUser, webServer.arg("mqtt_user").c_str(), sizeof(config.mqttUser));
    }
    if (webServer.hasArg("mqtt_pass")) {
        strlcpy(config.mqttPass, webServer.arg("mqtt_pass").c_str(), sizeof(config.mqttPass));
    }
    config.soundEnabled = webServer.hasArg("sound_enabled");

    // Zapisz konfigurację pomp
    for (int i = 0; i < 8; i++) {
        String pumpPrefix = "p" + String(i);
        config.pumps[i].enabled = webServer.hasArg(pumpPrefix + "_enabled");
        
        if (webServer.hasArg(pumpPrefix + "_dose")) {
            config.pumps[i].dosage = webServer.arg(pumpPrefix + "_dose").toFloat();
        }
        
        if (webServer.hasArg(pumpPrefix + "_time")) {
            String timeStr = webServer.arg(pumpPrefix + "_time");
            if (timeStr.length() >= 5) { // Format HH:MM
                config.pumps[i].hour = timeStr.substring(0, 2).toInt();
                config.pumps[i].minute = timeStr.substring(3, 5).toInt();
            }
        }
    }

    // Sprawdź poprawność wartości
    if (!validateConfigValues()) {
        config = oldConfig; // Przywróć poprzednie wartości
        webServer.send(400, "text/plain", "Invalid configuration values! Please check your input.");
        return;
    }

    // Sprawdź czy dane MQTT się zmieniły
    if (oldHost != config.mqttHost ||
        oldPort != config.mqttPort ||
        oldUser != config.mqttUser ||
        oldPass != config.mqttPass) {
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

    // Potwierdź zapisanie i zagraj dźwięk potwierdzenia jeśli włączony
    if (config.soundEnabled) {
        playConfirmationSound();
    }

    webServer.sendHeader("Location", "/");
    webServer.send(303);
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
        setDefaultConfig();
        saveConfig();
    }
    
    setupPCF8574();  // Inicjalizacja PCF8574
    setupWiFi();     // Konfiguracja WiFi
    setupHA();       // Konfiguracja Home Assistant
    setupWebServer();// Konfiguracja serwera WWW
    
    welcomeMelody();
}

// ** Funkcja loop - główny cykl pracy urządzenia **

void loop() {
    unsigned long currentMillis = millis();
    
    // Obsługa przepełnienia licznika millis()
    handleMillisOverflow();
    
    // Obsługa MQTT
    if (!mqtt.connected()) {
        connectMQTT();
    }
    mqtt.loop();
    
    // Obsługa serwera WWW
    webServer.handleClient();
    
    // Sprawdzenie stanu pomp i bezpieczeństwa
    for(int i = 0; i < 8; i++) {
        if (status.pumps[i].isRunning) {
            // Tutaj później dodamy logikę czasu dozowania
        }
    }
    
    // Dodatkowe zabezpieczenie - wyłącz wszystkie pompy w trybie serwisowym
    if (status.serviceMode) {
        stopAllPumps();
    }
}
