### README.md po polsku z instrukcją obsługi i ikonkami

```markdown
# AquaDoser

AquaDoser to system dozowania cieczy przeznaczony do automatyzacji procesu dozowania płynów do akwarium. Projekt oparty jest na mikrokontrolerze Arduino, który steruje pompami, zarządza konfiguracjami i integruje się z Home Assistant, umożliwiając zdalne monitorowanie i kontrolę.

## Funkcje

- **Sterowanie pompami**: Automatyczne sterowanie wieloma pompami dla precyzyjnego dozowania cieczy.
- **Integracja z Home Assistant**: Bezproblemowa integracja z Home Assistant przez MQTT do zdalnego monitorowania i kontroli.
- **Wskaźniki LED**: Wizualne wskaźniki statusu pomp, stanu systemu i błędów.
- **Interfejs webowy**: Wbudowany serwer WWW do konfiguracji i monitorowania.
- **Pamięć EEPROM**: Trwałe przechowywanie konfiguracji w pamięci EEPROM.
- **Zegar czasu rzeczywistego (RTC)**: Dokładne odmierzanie czasu z synchronizacją NTP.
- **Powiadomienia dźwiękowe**: Dźwiękowe alerty i powiadomienia.

## Wymagania wstępne

- Arduino IDE
- Wymagane biblioteki:
  - [PCF8574](https://github.com/xreef/PCF8574_library) ![Library](https://img.shields.io/badge/library-PCF8574-blue)
  - [PubSubClient](https://github.com/knolleary/pubsubclient) ![Library](https://img.shields.io/badge/library-PubSubClient-blue)
  - [ArduinoJson](https://github.com/bblanchon/ArduinoJson) ![Library](https://img.shields.io/badge/library-ArduinoJson-blue)
  - [NTPClient](https://github.com/arduino-libraries/NTPClient) ![Library](https://img.shields.io/badge/library-NTPClient-blue)
  - [ESP8266WiFi](https://github.com/esp8266/Arduino) ![Library](https://img.shields.io/badge/library-ESP8266WiFi-blue)

## Wymagania sprzętowe

- Mikrokontroler kompatybilny z Arduino (np. ESP8266)
- Ekspander I/O PCF8574
- Moduł RTC (np. DS3231)
- Wskaźniki LED
- Pompy i czujniki
- Buzzer do powiadomień dźwiękowych

## Instalacja

1. Sklonuj repozytorium:
   ```sh
   git clone https://github.com/pimowo/AquaDoser.git
   ```
2. Otwórz `AquaDoser.ino` w Arduino IDE.
3. Zainstaluj wymagane biblioteki za pomocą Arduino Library Manager.
4. Skonfiguruj ustawienia WiFi, MQTT i inne w kodzie.
5. Załaduj kod na swój mikrokontroler.

## Użytkowanie

- **Interfejs webowy**: Uzyskaj dostęp do interfejsu webowego poprzez adres IP mikrokontrolera, aby skonfigurować i monitorować system.
- **Home Assistant**: Skonfiguruj Home Assistant do połączenia z brokerem MQTT i zdalnego sterowania systemem dozowania.
- **Przycisk**: Użyj fizycznego przycisku do bezpośredniej interakcji z systemem.
- **Wskaźniki LED**: Obserwuj wskaźniki LED dla statusu systemu i aktywności pompy.

### Instrukcja Obsługi

1. **Podłączenie sprzętu**:
   - Podłącz mikrokontroler do ekspandera I/O PCF8574, modułu RTC, wskaźników LED, pomp i czujników zgodnie z dokumentacją sprzętu.
   - Podłącz buzzer do odpowiedniego pinu mikrokontrolera.

2. **Konfiguracja**:
   - Otwórz `AquaDoser.ino` w Arduino IDE i skonfiguruj ustawienia WiFi, MQTT oraz inne odpowiednie parametry.
   - Załaduj kod na mikrokontroler.

3. **Uruchomienie**:
   - Podłącz mikrokontroler do zasilania.
   - Uzyskaj dostęp do interfejsu webowego poprzez adres IP mikrokontrolera.
   - Skonfiguruj Home Assistant do połączenia z brokerem MQTT.

4. **Codzienne użytkowanie**:
   - Monitoruj system poprzez interfejs webowy lub Home Assistant.
   - Używaj przycisku do ręcznego sterowania dozowaniem.
   - Obserwuj wskaźniki LED dla statusu systemu i aktywności pomp.

## Wkład

Chętnie przyjmujemy wkład! Prosimy o otwieranie problemów lub przesyłanie pull requestów dotyczących wszelkich usprawnień lub poprawek błędów.

## Licencja

Ten projekt jest licencjonowany na warunkach licencji MIT. Zobacz plik [LICENSE](LICENSE) po więcej szczegółów.

```

Możesz dostosować i rozszerzyć ten README.md, aby lepiej pasował do wymagań Twojego projektu.
