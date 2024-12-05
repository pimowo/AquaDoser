
// Definicje pinów
#define PIN_PUMP1 32
#define PIN_PUMP2 33

// Stałe czasowe
#define NTP_SYNC_INTERVAL (24 * 60 * 60 * 1000)  // 24h w ms
#define PUMP_CHECK_INTERVAL 1000                  // 1s w ms

// EEPROM
#define EEPROM_SIZE 512
#define EEPROM_WIFI_SSID_ADDR 0
#define EEPROM_WIFI_PASS_ADDR 32
#define EEPROM_SCHEDULE_ADDR 64

// WiFi
#define WIFI_AP_SSID "AquaDoser"
#define WIFI_AP_PASSWORD "aquadoser"
