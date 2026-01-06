# GPS Tracker - ESP32 Multi-Sensor System

Projekt kompletnego trackera GPS opartego na mikrokontrolerze ESP32, wyposażonego w rejestrację danych na kartę SD, podgląd na żywo na ekranie OLED oraz czujnik ruchu (akcelerometr/żyroskop).

System jest przystosowany do zasilania bateryjnego (ogniwo 18650) i idealnie nadaje się do monitorowania trasy, prędkości oraz parametrów ruchu.

---

## 🛠 Elementy Systemu

1.  **Mikrokontroler:** ESP32 DevKit V1 (WROOM-32)
2.  **GPS:** NEO-6M (komunikacja UART)
3.  **IMU:** MPU6050 / MPU6500 (Akcelerometr + Żyroskop, I2C)
4.  **Wyświetlacz:** OLED 0.96" SSD1306 (128x64 px, I2C)
5.  **Pamięć:** Moduł czytnika kart MicroSD (SPI)
6.  **Zasilanie:**
    *   Ogniwo Li-Ion 18650
    *   Moduł ładowania i przetwornica (np. TP4056 + Boost Converter na 5V lub dedykowany shield zasilający)

---

## 🔌 Schemat Podłączenia (Pinout)

Wszystkie masy (GND) muszą być wspólne!

### 1. Magistrala I2C (OLED + MPU6050)
Urządzenia są podłączone równolegle do tych samych pinów.

| Urządzenie | Pin Modułu | Pin ESP32 | Uwagi |
| :--- | :--- | :--- | :--- |
| **OLED** | SDA | **GPIO 21** | Adres domyślny: `0x3C` |
| | SCL | **GPIO 22** | |
| | VCC | 3.3V / 5V | Zależnie od wersji modułu |
| **MPU6050**| SDA | **GPIO 21** | Adres: `0x68` lub `0x69` |
| | SCL | **GPIO 22** | |
| | VCC | 3.3V / 5V | |

### 2. Moduł GPS (NEO-6M)
Wykorzystuje sprzętowy UART2.

| Pin Modułu | Pin ESP32 | Funkcja |
| :--- | :--- | :--- |
| **TX** | **GPIO 16** (RX2) | Transmisja danych z GPS do ESP |
| **RX** | **GPIO 17** (TX2) | Konfiguracja GPS (opcjonalna) |
| VCC | 3.3V / 5V | |
| GND | GND | |

### 3. Moduł Karty SD (SPI)
Standardowe połączenie VSPI.

| Pin Modułu | Pin ESP32 | Funkcja |
| :--- | :--- | :--- |
| **CS** | **GPIO 5** | Chip Select |
| **SCK** | **GPIO 18** | Zegar |
| **MISO** | **GPIO 19** | Dane (Out) |
| **MOSI** | **GPIO 23** | Dane (In) |
| VCC | 5V | Większość modułów wymaga 5V |

---

## 💻 Oprogramowanie

Projekt jest stworzony w środowisku **PlatformIO** (VS Code).

### Wymagane Biblioteki
Zainstalowane automatycznie przez `platformio.ini`:
*   `Adafruit SSD1306` & `Adafruit GFX` (Obsługa ekranu)
*   `TinyGPSPlus` (Parsowanie danych NMEA z GPS)
*   `MPU6050_light` (Obsługa akcelerometru - wersja lekka, kompatybilna z klonami MPU)
*   `SD` (Wbudowana biblioteka Arduino do obsługi kart pamięci)

### Funkcje Kodu (`src/main.cpp`)
*   **Auto-kalibracja MPU:** Przy starcie system kalibruje żyroskopy (nie należy wtedy ruszać układem).
*   **Diagnostyka:** Na ekranie wyświetlany jest status każdego modułu (OK/ERR).
*   **Lokalizacja:** Po złapaniu sygnału GPS (FIX), wyświetla aktualne koordynaty oraz odległość w linii prostej do zdefiniowanego punktu domowego (Dojlidy Górne).
*   **Debug:** Port szeregowy (115200 baud) wypisuje szczegółowe logi diagnostyczne.

---

## 🚀 Jak uruchomić?

1.  Zainstaluj **Visual Studio Code** oraz rozszerzenie **PlatformIO**.
2.  Otwórz folder projektu.
3.  Podłącz ESP32 do komputera kablem USB (upewnij się, że to kabel DATA, a nie tylko do ładowania).
4.  Naciśnij ikonę PlatformIO (głowa obcego) -> **Project Tasks** -> **Upload and Monitor**.
5.  Jeśli GPS nie łapie fixa ("szukam..."), wystaw układ za okno lub na zewnątrz na 15-30 minut (tzw. Cold Start).

---

## 🔋 Zasilanie
Układ zasilany jest z ogniwa 18650. Napięcie z baterii (3.7V - 4.2V) jest podnoszone do 5V (przez przetwornicę step-up) i podawane na pin `VIN` (lub `5V`) w ESP32, co zapewnia stabilną pracę peryferiów (szczególnie modułu SD i GPS, które mogą wymagać stabilnego napięcia).
