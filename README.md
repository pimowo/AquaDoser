# 🌊 AquaDoser

## 📝 Opis
AquaDoser to automatyczny system dozowania nawozów i dodatków do akwarium. Urządzenie zostało zaprojektowane do precyzyjnego i regularnego dozowania płynów według ustalonego harmonogramu.

## ⚡ Funkcje
- 🕒 Automatyczne dozowanie według harmonogramu
- 📱 Sterowanie przez WiFi
- 🏠 Integracja z Home Assistant
- 🔄 Synchronizacja czasu przez NTP
- 🚨 Sygnalizacja dźwiękowa i świetlna (LED)
- 🛠️ Tryb serwisowy
- 💾 Zapisywanie ustawień w pamięci EEPROM

## 🛠️ Instrukcja obsługi

### Pierwsze uruchomienie
1. Po włączeniu urządzenia, AquaDoser utworzy punkt dostępowy WiFi
2. Połącz się z siecią WiFi urządzenia
3. Skonfiguruj połączenie z Twoją siecią WiFi poprzez panel konfiguracyjny

### Panel sterowania
Panel webowy pozwala na:
- Konfigurację ustawień pomp:
  - Godziny dozowania
  - Ilość dozowanego płynu (ml)
  - Kalibrację wydajności pompy
- Ustawienia MQTT dla Home Assistant
- Włączanie/wyłączanie dźwięków
- Monitoring stanu urządzenia

### 💡 Wskaźniki LED
- 🟢 Zielony - pompa pracuje prawidłowo
- 🔵 Niebieski - trwa dozowanie
- 🔴 Czerwony - błąd lub problem z pompą
- 💫 Pulsowanie - tryb serwisowy

### 🔊 Sygnały dźwiękowe
- Pojedynczy sygnał - potwierdzenie operacji
- Podwójny sygnał - ostrzeżenie
- Melodia powitalna - uruchomienie urządzenia

### 🛠️ Tryb serwisowy
1. Aby włączyć tryb serwisowy, przytrzymaj przycisk przez 3 sekundy
2. W trybie serwisowym możesz:
   - Ręcznie testować pompy
   - Kalibrować wydajność pomp
   - Sprawdzać poprawność działania
3. Ponowne przytrzymanie przycisku wyłącza tryb serwisowy

### 🏠 Integracja z Home Assistant
AquaDoser automatycznie integruje się z Home Assistant poprzez MQTT, oferując:
- Status każdej pompy
- Możliwość zdalnego sterowania
- Monitorowanie najbliższego zaplanowanego dozowania
- Informacje o ostatnim dozowaniu

### ⚙️ Kalibracja pomp
1. Włącz tryb serwisowy
2. Przygotuj menzurkę lub inny pojemnik z miarką
3. Uruchom pompę na określony czas
4. Zmierz ilość przepompowanego płynu
5. Wprowadź wartość kalibracji w panelu konfiguracyjnym

### 🚨 Rozwiązywanie problemów
- Brak połączenia z WiFi:
  - Sprawdź ustawienia sieci
  - Zresetuj urządzenie do ustawień fabrycznych
- Pompa nie dozuje:
  - Sprawdź czy rurki nie są zapowietrzone
  - Skontroluj ustawienia harmonogramu
  - Sprawdź kalibrację pompy
- Nieprawidłowy czas dozowania:
  - Sprawdź połączenie z serwerem NTP
  - Zweryfikuj strefę czasową w ustawieniach

### ⚠️ Ważne uwagi
- Regularnie sprawdzaj szczelność połączeń
- Kontroluj poziom płynów w zbiornikach
- Wykonuj okresową kalibrację pomp
- Nie odłączaj zasilania podczas aktualizacji ustawień

## 🔄 Aktualizacje
System posiada możliwość aktualizacji oprogramowania przez interfejs webowy. Aby zaktualizować:
1. Przejdź do zakładki aktualizacji
2. Wybierz plik z nowym oprogramowaniem
3. Potwierdź aktualizację
4. Poczekaj na restart urządzenia

## 📝 Dane techniczne
- Zasilanie: 12V DC
- Liczba pomp: 8
- Interfejs: WiFi
- Wyświetlacz: LED RGB
- Pamięć konfiguracji: EEPROM
- Zegar: RTC z podtrzymaniem bateryjnym

## 🔧 Reset do ustawień fabrycznych
W przypadku problemów możesz przywrócić ustawienia fabryczne:
1. Wyłącz urządzenie
2. Przytrzymaj przycisk
3. Włącz urządzenie trzymając przycisk
4. Poczekaj na sygnał dźwiękowy
5. Puść przycisk

==============================================================

🌿 AquaDoser - Automatyczny dozownik nawozów
     Precyzyjne dozowanie nawozów płynnych według harmonogramu

💧 HydroSense - System kontroli poziomu wody
     Automatyczne uzupełnianie wody w akwarium

💡 LumaSense - Sterownik oświetlenia
     Kontrola oświetlenia LED, symulacja wschodu/zachodu słońca

🌡️ ClimaCore - Sterownik parametrów środowiskowych
     Kontrola temperatury i dozowania CO2

⚖️ GasSense - System monitorowania CO2
     Pomiar wagi butli, kontrola ciśnienia w układzie CO2

==============================================================
