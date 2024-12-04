// ** BIBLIOTEKI **

// Biblioteki podstawowe
#include <Arduino.h>                  // Podstawowa biblioteka Arduino - zawiera funkcje rdzenia (pinMode, digitalRead itp.)
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

// Zegar
#include <RTClib.h>
#include <TimeLib.h>
#include <Timezone.h>    // dla obsługi czasu letniego/zimowego

// Pozostałe
#include <Adafruit_NeoPixel.h>  // Sterowanie LED

// ** DEFINICJE PINÓW **

// Przypisanie pinów do urządzeń
const uint8_t NUMBER_OF_PUMPS = 8;  // Ilość pomp
const int BUZZER_PIN = 13;      // GPIO 13
const int LED_PIN = 12;         // GPIO 12
const int BUTTON_PIN = 14;    // GPIO 14
const int SDA_PIN = 4;          // GPIO 4
const int SCL_PIN = 5;          // GPIO 5

PCF8574 pcf8574(0x20);

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
#define DEBUG 0  // 0 wyłącza debug, 1 włącza debug

#if DEBUG
    #define AQUA_DEBUG_PRINT(x) Serial.println(x)
    #define AQUA_DEBUG_PRINTF(format, ...) Serial.printf(format, __VA_ARGS__)
#else
    #define AQUA_DEBUG_PRINT(x)
    #define AQUA_DEBUG_PRINTF(format, ...)
#endif

// Zmienna przechowująca wersję oprogramowania
const char* SOFTWARE_VERSION = "4.12.24";  // Definiowanie wersji oprogramowania

// Struktury konfiguracyjne i statusowe

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

// Globalne instancje struktur
Config config;
Status status;
ButtonState buttonState;
Timers timers;
CustomTimeStatus currentStatus = getCustomTimeStatus();

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

// ** STRONA KONFIGURACYJNA **

// Strona konfiguracji przechowywana w pamięci programu
const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
  <html>
    <head>
        <meta charset='UTF-8'>
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>AquaDoser</title>
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
                .pump-section {
                    background-color: #2d2d2d;
                    padding: 15px;
                    margin-bottom: 15px;
                    border-radius: 4px;
                }
                
                .pump-section h3 {
                    color: #2196F3;
                    margin-top: 0;
                    margin-bottom: 15px;
                }
                
                .weekdays {
                    display: grid;
                    grid-template-columns: repeat(7, auto);
                    gap: 5px;
                    justify-content: start;
                }
                
                .weekdays label {
                    background-color: #1a1a1a;
                    padding: 5px 8px;
                    border-radius: 4px;
                    cursor: pointer;
                    transition: background-color 0.3s;
                }
                
                .weekdays label:hover {
                    background-color: #2d2d2d;
                }
                
                .weekdays input[type="checkbox"]:checked + span {
                    color: #2196F3;
                }
                input[type="number"] {
                    width: 80px;
                    text-align: right;
                    -moz-appearance: textfield;
                }
                
                input[type="number"]::-webkit-outer-spin-button,
                input[type="number"]::-webkit-inner-spin-button {
                    -webkit-appearance: none;
                    margin: 0;
                }
                
                .time-input {
                    width: 60px;
                }
                .pump-status {
                    padding: 4px 8px;
                    border-radius: 4px;
                    font-size: 0.9em;
                }
                
                .pump-status.active {
                    background-color: #4CAF50;
                    color: white;
                }
                
                .pump-status.inactive {
                    background-color: #666;
                    color: white;
                }
                
                .time-info { 
                    font-weight: bold; 
                }
                .season-info { 
                    margin-left: 10px; font-size: 0.9em; 
                }
            }
        </style>
        <script>
            // Potwierdzenie zapisu formularza
            document.querySelector('form').addEventListener('submit', function(e) {
                if (!confirm('Czy na pewno chcesz zapisać zmiany w konfiguracji pomp?')) {
                    e.preventDefault();
                }
            });
    
            // Obsługa przycisku testowego pompy
            document.querySelectorAll('.test-pump').forEach(button => {
                button.addEventListener('click', function() {
                    const pumpId = this.dataset.pump;
                    if (confirm('Czy chcesz wykonać test pompy ' + (parseInt(pumpId) + 1) + '?')) {
                        // Wysłanie przez WebSocket komendy testu
                        socket.send('test_pump:' + pumpId);
                    }
                });
            });
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
        <h1>AquaDoser</h1>
        
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
    <a href='https://github.com/pimowo/AquaDoser' target='_blank'>Project by PMW</a>
</div>
)rawliteral";

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

// CustomTimeStatus getCustomTimeStatus() {
//     CustomTimeStatus status;
//     DateTime now = rtc.now();
//     time_t localTime = CE.toLocal(now.unixtime());
    
//     // Format czasu GG:MM
//     char timeStr[6];
//     sprintf(timeStr, "%02d:%02d", hour(localTime), minute(localTime));
//     status.time = String(timeStr);
    
//     // Format daty DD/MM/RRRR
//     char dateStr[11];
//     sprintf(dateStr, "%02d/%02d/%04d", day(localTime), month(localTime), year(localTime));
//     status.date = String(dateStr);
    
//     // Sprawdzenie czy jest czas letni
//     status.season = CE.locIsDST(now.unixtime()) ? "LATO" : "ZIMA";
    
//     return status;
// }

CustomTimeStatus getCustomTimeStatus() {
    CustomTimeStatus status;
    // Pobierz aktualny czas
    status.time = String(hour()) + ":" + String(minute()) + ":" + String(second());
    // Pobierz aktualną datę
    status.date = String(day()) + "/" + String(month()) + "/" + String(year());
    // Ustaw sezon (możesz dostosować logikę)
    int currentMonth = month();
    if (currentMonth >= 3 && currentMonth <= 5) status.season = "Wiosna";
    else if (currentMonth >= 6 && currentMonth <= 8) status.season = "Lato";
    else if (currentMonth >= 9 && currentMonth <= 11) status.season = "Jesień";
    else status.season = "Zima";
    
    return status;
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
    configForms += F("'></td></tr>");
        
    CustomTimeStatus currentStatus = getCustomTimeStatus();

    // Dodaj informacje o czasie
    configForms += F("<tr><td>Czas</td><td>");
    configForms += currentStatus.time;
    configForms += F(" (");
    configForms += currentStatus.season;
    configForms += F(")</td></tr>");
    
    configForms += F("<tr><td>Data</td><td>");
    configForms += currentStatus.date;
    configForms += F("</td></tr>");    
                     "</table></div>");

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
        configForms += F("' title='Ilość ml/min pompowana przez pompę'></td></tr>");
        
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
            configForms += F(">");
            configForms += days[day];
            configForms += F("</label>");
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

    // W sekcji pompy
    configForms += F("<tr><td colspan='2' class='calibration-history'>");
    configForms += F("<strong>Ostatnia kalibracja:</strong><br>");

    for (int i = 0; i < NUMBER_OF_PUMPS; i++) {
        if (config.pumps[i].lastCalibration.timestamp > 0) {  // jeśli była kalibracja
            // Konwersja timestamp na czytelną datę
            char dateStr[20];
            time_t ts = config.pumps[i].lastCalibration.timestamp;
            strftime(dateStr, sizeof(dateStr), "%d.%m.%Y %H:%M", localtime(&ts));
            
            configForms += String(dateStr);
            configForms += F("<br>Czas: ");
            configForms += String(config.pumps[i].lastCalibration.time);
            configForms += F("s<br>Objętość: ");
            configForms += String(config.pumps[i].lastCalibration.volume);
            configForms += F("ml<br>Wydajność: ");
            configForms += String(config.pumps[i].lastCalibration.flowRate);
            configForms += F(" ml/min");
        } else {
            configForms += F("Brak kalibracji");
        }
    }

    configForms += F("</td></tr>");
    
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
    
    server.begin();
}

// ** WEBSOCKET I KOMUNIKACJA W SIECI **

// void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
//     // Obsługujemy tylko połączenie - reszta nie jest potrzebna
//     if (type == WStype_CONNECTED) {
//         Serial.printf("[%u] Connected\n", num);
//     }
// }

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
       
    // Wczytaj konfigurację na początku
    if (!loadConfig()) {
        AQUA_DEBUG_PRINTF("Błąd wczytywania konfiguracji - używam ustawień domyślnych");
        setDefaultConfig();
        saveConfig();  // Zapisz domyślną konfigurację do EEPROM
    }

    setupPin();
    setupPump();
  
    WiFiManager wifiManager;
    wifiManager.autoConnect("AquaDoser");  // Samo zadba o połączenie
    setupWebServer();
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    setupHA();

    // Inicjalizacja NTP
    configTime(0, 0, "pool.ntp.org"); // Ustawiamy strefę na UTC (offset 0)
    
    // Inicjalizacja RTC
    if (!initRTC()) {
        // Obsługa błędu inicjalizacji RTC
        AQUA_DEBUG_PRINT(F("Błąd inicjalizacji RTC!"));
    }
    
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
