#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TinyGPS++.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MPU6050_light.h>
#include <esp_wifi.h> // Potrzebne do zmiany mocy WiFi
#include "webpage.h"

// --- KONFIGURACJA PINÓW ---
#define I2C_SDA 21
#define I2C_SCL 22
#define GPS_RX 16
#define GPS_TX 17
#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

// --- STAŁE ---
#define WIFI_SSID "Ceskie drahy"
#define WIFI_PASS "30788888"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define LOG_BUFFER_SIZE 2048 // Increased for wifi reconnect safety
#define AUTO_PAUSE_SPEED 0.5 // km/h (Lowered for sensitivity)
#define AUTO_PAUSE_TIME 2000 // ms (Faster auto-pause)
#define MIN_DIST 5.0 // meters
#define GPS_READ_LIMIT 1000 // Max NMEA chars per loop iteration

// --- PINY ADC ---
#define BATTERY_PIN 34 // GPIO 34 (Analog Input)
#define BATTERY_MAX_VOLTAGE 4.2 
#define BATTERY_R1 100000.0 // 100k
#define BATTERY_R2 100000.0 // 100k - Adjust based on your divider

#define WIFI_RECONNECT_PIN 0 // Przycisk BOOT (Zmień jeśli używasz innego pinu)

// --- OBIEKTY ---
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);
MPU6050 mpu(Wire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
AsyncWebServer server(80);

// --- MUTEX (Chroniący SD oraz logBuffer i sharedStatus) ---
SemaphoreHandle_t sdMutex = NULL;

// --- ZMIENNE STANU ---
enum State { IDLE, RECORDING, PAUSED };
State currentState = IDLE;
bool manualPause = false; // New flag for manual pause
bool triggerWifiReconnect = false; // Flag for Web API reconnect

// Struktura do współdzielenia stanu z wątkiem serwera (Atomowość)
struct TrackerStatus {
    double lat, lon, speed, alt, dist, hdop;
    int sats;
    float ax, ay, az;
    float batt; // Napięcie baterii
    int state;
    unsigned long elapsed; // Czas trwania nagrania
} sharedStatus;

bool sdReady = false;
bool mpuReady = false;
bool gpsFix = false;

String currentFileName = "";
String logBuffer = "";
unsigned long lastMotionTime = 0;
unsigned long sessionStart = 0;
unsigned long pauseStart = 0;
unsigned long totalPaused = 0;
double totalDist = 0;
float lastValidAlt = 0.0; // Hold last altitude
float speedBuf[5] = {0}; // Speed smoothing buffer
int speedIdx = 0;
double lastLat = 0, lastLon = 0;

// --- PROTOTYPY ---
void setupHardware();
void setupWiFi();
void setupServer();
void logicLoop();
void displayLoop();
void logData();
void startRec();
void stopRec();
bool checkMotion();
String getFileList();
void updateSharedStatus();
float readBattery();
void tryConnectWiFi(); // Manual reconnect

void setup() {
    Serial.begin(115200);
    
    pinMode(WIFI_RECONNECT_PIN, INPUT_PULLUP);

    // Konfiguracja ADC
    pinMode(BATTERY_PIN, INPUT);
    analogReadResolution(12); // ESP32 default 12-bit (0-4095)

    // Mutex MUSI być utworzony PRZED setupHardware (SD init)
    sdMutex = xSemaphoreCreateMutex();
    if(sdMutex == NULL) {
        Serial.println("FATAL: Mutex creation failed!");
        while(1); // halt
    }
    
    setupHardware();
    setupWiFi();
    setupServer();
    
    // Preallokacja bufora dla String (zapobiega fragmentacji)
    logBuffer.reserve(LOG_BUFFER_SIZE + 100);
}

void loop() {
    // 1. GPS Feed (LIMITED to avoid blocking)
    int gpsCharsRead = 0;
    static unsigned long totalGpsBytes = 0;
    while(gpsSerial.available() && gpsCharsRead < GPS_READ_LIMIT) {
        gps.encode(gpsSerial.read());
        gpsCharsRead++;
        totalGpsBytes++;
    }

    // --- DEBUG GPS (Added for troubleshooting) ---
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        lastDebug = millis();
        if (totalGpsBytes == 0) {
             Serial.println("[GPS ERROR] Brak danych z GPS! Sprawdz zasilanie modulu i polaczenia (TX->RX, RX->TX).");
        } else {
             Serial.print("[GPS OK] Odbieram dane. Bytes: ");
             Serial.print(totalGpsBytes);
             Serial.print(" Sats: ");
             Serial.print(gps.satellites.value());
             Serial.print(" Fix: ");
             Serial.print(gps.location.isValid() ? "TAK" : "NIE");
             Serial.print(" Chars: ");
             Serial.print(gps.charsProcessed());
             Serial.print(" ErrCRC: ");
             Serial.println(gps.failedChecksum());
        }
    }
    // ---------------------------------------------
    
    // 2. MPU Update (Always run for IMU)
    if(mpuReady) mpu.update();

    // 3. Logic & Shared State Update
    logicLoop();

    // 4. Display (Low prio)
    static unsigned long lastDisp = 0;
    if(millis() - lastDisp > 500) {
        displayLoop();
        lastDisp = millis();
    }
    
    // 6. WiFi Reconnect Button (BOOT)
    if(digitalRead(WIFI_RECONNECT_PIN) == LOW) {
        // Debounce
        delay(100);
        if(digitalRead(WIFI_RECONNECT_PIN) == LOW) {
            tryConnectWiFi();
        }
    }

    // 7. Handle Web Reconnect Request
    if(triggerWifiReconnect) {
        triggerWifiReconnect = false;
        tryConnectWiFi();
    }
}

// --- IMPLEMENTACJA ---

void tryConnectWiFi() {
    Serial.println("Manual WiFi Reconnect...");
    
    // Use mutex on shared vars/display if needed, but display handles itself simply
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0,0);
    display.println(" Szukam WiFi...");
    display.println(" (" + String(WIFI_SSID) + ")");
    display.display();

    WiFi.disconnect(); 
    // Do not change mode here widely, just reconnect STA
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Try for 5 seconds
    int retries = 0;
    while(WiFi.status() != WL_CONNECTED && retries < 10) {
        delay(500); 
        Serial.print(".");
        retries++;
    }
    
    display.clearDisplay();
    display.setCursor(0,0);
    if(WiFi.status() == WL_CONNECTED) {
        Serial.println("\nReconnect Success!");
        display.println("POLACZONO!");
        display.println(WiFi.localIP());
    } else {
        Serial.println("\nReconnect Failed.");
        display.println("Brak WiFi.");
        display.println("Nadal AP.");
    }
    display.display();
    delay(1500); // Show result
}

float readBattery() {
    int raw = analogRead(BATTERY_PIN);
    // Vout = (raw / 4095.0) * 3.3V
    // Vin = Vout * (R1+R2)/R2
    float vout = (raw / 4095.0) * 3.3;
    float vin = vout * (BATTERY_R1 + BATTERY_R2) / BATTERY_R2;
    // Add small calibration correction if needed
    return vin; 
}

void setupHardware() {
    Wire.begin(I2C_SDA, I2C_SCL);

    // OLED
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED Fail");
    }
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(0,0);
    display.println("Booting...");
    display.display();

    // MPU
    if(mpu.begin() == 0) {
        mpu.calcOffsets(true,true);
        mpuReady = true;
        Serial.println("MPU OK");
    } else {
        Serial.println("MPU Fail");
    }

    // SD (z Mutex protection)
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    
    if(xSemaphoreTake(sdMutex, portMAX_DELAY) == pdTRUE) {
        if(SD.begin(SD_CS)) {
            sdReady = true;
            Serial.println("SD OK");
        } else {
            Serial.println("SD Fail");
        }
        xSemaphoreGive(sdMutex);
    }

    // GPS
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.println("GPS init: RX=" + String(GPS_RX) + ", TX=" + String(GPS_TX));
}

void setupWiFi() {
    WiFi.mode(WIFI_AP_STA);
    
    // ZMNIEJSZENIE MOCY TX (Ochrona GPS przed zakłóceniami RF)
    // Bezpieczniejszy poziom dla trybu AP+STA blisko modułu GPS
    WiFi.setTxPower(WIFI_POWER_11dBm);

    display.clearDisplay();
    display.setCursor(0,0);
    display.println("Lacze z Hotspotem...");
    display.println(WIFI_SSID);
    display.display();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    int retries = 0;
    while(WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }

    if(WiFi.status() == WL_CONNECTED) {
        Serial.println("\nSTA Connected!");
        Serial.print("IP: "); Serial.println(WiFi.localIP());
        display.println("OK! IP:");
        display.println(WiFi.localIP());
    } else {
        Serial.println("\nSTA Failed, AP Only");
        display.println("Brak Hotspotu.");
        display.println("Tryb AP (Offline)");
    }
    display.display();
    delay(2000);

    // Konfiguracja AP
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP("ESP32-Tracker", "12345678");
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

    // CORS Headers
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
}

void setupServer() {
    // Main Page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    // Status API - ATOMIC READ
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json;
        json.reserve(300); // Increased size for new fields
        
        // Zwiększony timeout na pobranie mutexu (100ms) aby uniknąć 503 gdy SD jest zajęte
        if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Czytamy z kopii (sharedStatus), nie z 'gps'
            json = "{";
            json += "\"state\":" + String(sharedStatus.state) + ",";
            json += "\"sats\":" + String(sharedStatus.sats) + ",";
            json += "\"lat\":" + String(sharedStatus.lat, 6) + ",";
            json += "\"lon\":" + String(sharedStatus.lon, 6) + ",";
            json += "\"speed\":" + String(sharedStatus.speed, 1) + ",";
            json += "\"alt\":" + String(sharedStatus.alt, 1) + ",";
            json += "\"hdop\":" + String(sharedStatus.hdop, 1) + ","; // New
            json += "\"dist\":" + String(sharedStatus.dist, 1) + ",";
            json += "\"batt\":" + String(sharedStatus.batt, 2) + ","; // New
            json += "\"ax\":" + String(sharedStatus.ax, 2) + ",";
            json += "\"ay\":" + String(sharedStatus.ay, 2) + ",";
            json += "\"az\":" + String(sharedStatus.az, 2) + ",";
            // Check WiFi Status (WL_CONNECTED = 3)
            json += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? 1 : 0) + ",";
            json += "\"elapsed\":" + String(sharedStatus.elapsed); // Added elapsed time
            json += "}";
            xSemaphoreGive(sdMutex);
            
            request->send(200, "application/json", json);
        } else {
            request->send(503, "text/plain", "Busy");
        }
    });

    // FILES API
    server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request){
        String list = getFileList();
        request->send(200, "application/json", list);
    });

    // TRACK API - Returns current session track data
    server.on("/api/track", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentFileName == "" || !sdReady) {
            request->send(200, "application/json", "[]");
            return;
        }
        
        if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            File f = SD.open(currentFileName, FILE_READ);
            if(!f) {
                xSemaphoreGive(sdMutex);
                request->send(200, "application/json", "[]");
                return;
            }
            
            String json = "[";
            bool first = true;
            
            while(f.available()) {
                String line = f.readStringUntil('\n');
                if(line.length() < 10) continue; // Skip empty lines
                
                // Parse: millis,lat,lon,speed,alt,hdop,sats,ax,ay,az,batt
                int idx = 0;
                String parts[11];
                int partIdx = 0;
                for(int i = 0; i < line.length() && partIdx < 11; i++) {
                    if(line[i] == ',') {
                        partIdx++;
                    } else {
                        parts[partIdx] += line[i];
                    }
                }
                
                if(partIdx >= 6) { // At least time,lat,lon,speed,alt,hdop,sats
                    unsigned long ms = parts[0].toInt();
                    unsigned long sec = (ms - sessionStart) / 1000;
                    
                    if(!first) json += ",";
                    first = false;
                    
                    json += "{";
                    json += "\"lat\":" + parts[1] + ",";
                    json += "\"lon\":" + parts[2] + ",";
                    json += "\"speed\":" + parts[3] + ",";
                    json += "\"alt\":" + parts[4] + ",";
                    json += "\"hdop\":" + parts[5] + ",";
                    json += "\"elapsed\":" + String(sec);
                    json += "}";
                }
            }
            json += "]";
            
            f.close();
            xSemaphoreGive(sdMutex);
            
            request->send(200, "application/json", json);
        } else {
            request->send(503, "text/plain", "Busy");
        }
    });

    server.on("/api/start", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState == IDLE) {
            manualPause = false; // Reset manual flag
            startRec();
        } else if(currentState == PAUSED) {
            manualPause = false; // Resume manually
            currentState = RECORDING; // Resume
            Serial.println("Resumed");
        }
        request->send(200);
    });
    
    server.on("/api/pause", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState == RECORDING) {
            currentState = PAUSED;
            manualPause = true; // Set manual pause
            pauseStart = millis();
            
            // Flush buffer safe
            if(sdReady) {
                if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if(logBuffer.length() > 0) {
                        File f = SD.open(currentFileName, FILE_APPEND);
                        if(f) {
                            f.print(logBuffer);
                            f.flush();
                            f.close();
                            logBuffer = "";
                        }
                    }
                    xSemaphoreGive(sdMutex);
                }
            }
            Serial.println("Paused & Flushed");
        }
        request->send(200);
    });
    
    // RECONNECT (Trigger flag)
    server.on("/api/reconnect", HTTP_GET, [](AsyncWebServerRequest *request){
        triggerWifiReconnect = true;
        request->send(200, "text/plain", "Reconnecting...");
    });

    server.on("/api/stop", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState != IDLE) {
            manualPause = false; // Reset
            stopRec();
        }
        request->send(200);
    });
    
    server.on("/api/discard", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState != IDLE) {
            currentState = IDLE;
            manualPause = false; // Reset
            // Mutex for clearing buffer
            if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                logBuffer = "";
                if(SD.exists(currentFileName)) {
                    SD.remove(currentFileName);
                    Serial.println("File discarded");
                }
                xSemaphoreGive(sdMutex);
            }
        }
        request->send(200);
    });

    // CURRENT TRACK (CSV) - For restoring path on refresh
    server.on("/api/current_track", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState != IDLE && currentFileName != "") {
             if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                 if(SD.exists(currentFileName)) {
                     request->send(SD, currentFileName, "text/csv");
                 } else {
                     request->send(404, "text/plain", "File Missing");
                 }
                 xSemaphoreGive(sdMutex);
             } else {
                 request->send(503, "text/plain", "Busy");
             }
        } else {
            request->send(204); // No Content if idle
        }
    });

    // DOWNLOAD
    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
        if(!request->hasParam("file")) {
            request->send(400, "text/plain", "Missing file param");
            return;
        }
        String fname = request->getParam("file")->value();
        if(!fname.startsWith("/")) fname = "/" + fname;
        if(fname.indexOf("..") >= 0) { request->send(403, "text/plain", "Forbidden"); return; }
        
        if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) == pdTRUE) { // Short wait
            if(SD.exists(fname)) {
                request->send(SD, fname, "application/octet-stream");
            } else {
                request->send(404, "text/plain", "Not Found");
            }
            xSemaphoreGive(sdMutex);
        } else {
            request->send(503, "text/plain", "SD Busy");
        }
    });

    // DELETE
    server.on("/delete", HTTP_DELETE, [](AsyncWebServerRequest *request){
        if(!request->hasParam("file")) { request->send(400, "text/plain", "Missing file param"); return; }
        String fname = request->getParam("file")->value();
        if(!fname.startsWith("/")) fname = "/" + fname;
        if(fname.indexOf("..") >= 0) { request->send(403, "text/plain", "Forbidden"); return; }
        if(fname == currentFileName && currentState != IDLE) { request->send(400, "text/plain", "Cannot delete active log!"); return; }
        
        if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if(SD.exists(fname)) { 
                SD.remove(fname); 
                request->send(200, "text/plain", "Deleted"); 
            } else {
                request->send(404, "text/plain", "Not Found");
            }
            xSemaphoreGive(sdMutex);
        } else {
            request->send(503, "text/plain", "SD Busy");
        }
    });

    server.begin();
    Serial.println("Server started");
}

// --- LOGIKA ---

void updateSharedStatus() {
    // Kopiowanie stanu do sharedStatus POD MUTEXEM
    if(xSemaphoreTake(sdMutex, 0) == pdTRUE) { // 0 ticks - don't block loop if busy
        bool valid = gps.location.isValid();
        
        // 1. Signal Loss Handling: Hold Altitude
        if(valid && gps.altitude.isValid()) {
            lastValidAlt = gps.altitude.meters();
        }
        sharedStatus.alt = lastValidAlt;

        // 2. Speed Smoothing & Signal Loss (Speed 0 if invalid)
        float rawSpeed = (valid && gps.speed.isValid()) ? gps.speed.kmph() : 0.0;
        
        // Moving Average
        speedBuf[speedIdx] = rawSpeed;
        speedIdx = (speedIdx + 1) % 5;
        float sum = 0;
        for(int i=0; i<5; i++) sum += speedBuf[i];
        sharedStatus.speed = sum / 5.0;

        sharedStatus.lat = valid ? gps.location.lat() : 0.0;
        sharedStatus.lon = valid ? gps.location.lng() : 0.0;
        
        sharedStatus.hdop = gps.hdop.hdop(); 
        sharedStatus.sats = (int)gps.satellites.value();
        sharedStatus.dist = totalDist;
        sharedStatus.ax = mpuReady ? mpu.getAccX() : 0.0;
        sharedStatus.ay = mpuReady ? mpu.getAccY() : 0.0;
        sharedStatus.az = mpuReady ? mpu.getAccZ() : 0.0;
        sharedStatus.batt = readBattery(); 
        sharedStatus.state = currentState;

        // Calculate elapsed time securely
        if(currentState == RECORDING || currentState == PAUSED) {
            sharedStatus.elapsed = (millis() - sessionStart - totalPaused) / 1000; // in seconds
        } else {
            sharedStatus.elapsed = 0;
        }

        xSemaphoreGive(sdMutex);
    }
}

bool checkMotion() {
    bool gpsMoving = (gps.speed.kmph() > AUTO_PAUSE_SPEED);
    if(!mpuReady) return gpsMoving;
    float gMagnitude = sqrt(pow(mpu.getAccX(), 2) + pow(mpu.getAccY(), 2) + pow(mpu.getAccZ(), 2));
    bool imuMoving = (fabs(gMagnitude - 1.0) > 0.08); // Threshold lowered
    return gpsMoving || imuMoving;
}

void logicLoop() {
    gpsFix = gps.location.isValid();
    
    // Zawsze aktualizuj status dla WWW
    updateSharedStatus();
    
    if(currentState == IDLE) return;

    if(checkMotion()) {
        lastMotionTime = millis();
        // Only auto-resume if NOT manually paused
        if(currentState == PAUSED && !manualPause) {
            currentState = RECORDING;
            totalPaused += (millis() - pauseStart);
            Serial.println("Auto-resumed");
        }
    } else {
        // Only auto-pause if recording (and not already paused)
        if(currentState == RECORDING && (millis() - lastMotionTime > AUTO_PAUSE_TIME)) {
            currentState = PAUSED;
            // Note: manualPause remains false
            pauseStart = millis();
            Serial.println("Auto-paused");
            
            // Flush buffer safe
            if(sdReady) {
                if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if(logBuffer.length() > 0) {
                        File f = SD.open(currentFileName, FILE_APPEND);
                        if(f) {
                            f.print(logBuffer);
                            f.flush();
                            f.close();
                            logBuffer = "";
                        }
                    }
                    xSemaphoreGive(sdMutex);
                }
            }
        }
    }

    if(currentState == RECORDING) {
        logData();
    }
}

void logData() {
    if(!gpsFix || !sdReady) return;

    static unsigned long lastFlush = 0; // Time based flush

    // Obliczenia na zmiennych lokalnych (bez mutexa)
    double d = TinyGPSPlus::distanceBetween(
        gps.location.lat(), gps.location.lng(), 
        lastLat, lastLon
    );
    
    if(d > MIN_DIST || lastLat == 0) {
        char line[180]; // Increased buffer
        // Format: millis,lat,lon,speed,alt,hdop,sats,ax,ay,az,batt
        snprintf(line, sizeof(line), 
            "%lu,%.6f,%.6f,%.1f,%.1f,%.1f,%d,%.2f,%.2f,%.2f,%.2f\n",
            millis(),
            gps.location.lat(),
            gps.location.lng(),
            gps.speed.kmph(),
            gps.altitude.meters(),
            gps.hdop.hdop(), 
            (int)gps.satellites.value(),
            mpuReady ? mpu.getAccX() : 0.0,
            mpuReady ? mpu.getAccY() : 0.0,
            mpuReady ? mpu.getAccZ() : 0.0,
            readBattery() 
        );
        
        // Zapis do logBuffer i ewentualny flush POD MUTEXEM
        if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            logBuffer += line;
            
            // Aktualizacja stanu
            if(lastLat != 0) totalDist += d;
            lastLat = gps.location.lat();
            lastLon = gps.location.lng();

            // Zapisz jesli bufor pelny LUB minelo 10 sekund
            bool timeToFlush = (millis() - lastFlush > 10000);

            if(logBuffer.length() >= LOG_BUFFER_SIZE || (timeToFlush && logBuffer.length() > 0)) {
                File f = SD.open(currentFileName, FILE_APPEND);
                if(f) {
                    f.print(logBuffer);
                    f.close(); // Close zapisuje fizycznie na karcie
                    logBuffer = "";
                    lastFlush = millis();
                }
            }
            xSemaphoreGive(sdMutex);
        }
    }
}

void startRec() {
    if(!sdReady) {
        Serial.println("Cannot start: SD not ready");
        return;
    }
    
    // Zabezpieczenie całej operacji startu
    if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        // Generowanie nazwy pliku z daty/czasu GPS (jesli dostepny)
        if(gps.date.isValid() && gps.time.isValid() && gps.date.year() > 2020) {
             char fn[32];
             snprintf(fn, sizeof(fn), "/%04d%02d%02d_%02d%02d%02d.csv", 
                gps.date.year(), gps.date.month(), gps.date.day(),
                gps.time.hour(), gps.time.minute(), gps.time.second());
             currentFileName = String(fn);
        } else {
             // Fallback gdy brak fixa
             currentFileName = "/gps_log_" + String(millis()) + ".csv";
        }

        File f = SD.open(currentFileName, FILE_WRITE);
        if(f) {
            f.println("time_ms,lat,lon,speed_kmh,alt_m,sats,ax,ay,az");
            f.close();
            Serial.println("Started: " + currentFileName);
            
            currentState = RECORDING;
            sessionStart = millis();
            lastMotionTime = millis();
            totalPaused = 0;
            totalDist = 0;
            lastLat = 0; 
            lastLon = 0;
            logBuffer = ""; // Clear buffer clearly under mutex
        } else {
            Serial.println("Failed to create file");
        }
        xSemaphoreGive(sdMutex);
    } else {
        Serial.println("Start busy");
    }
}

void stopRec() {
    // Final flush with Mutex
    if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if(logBuffer.length() > 0 && sdReady) {
            File f = SD.open(currentFileName, FILE_APPEND);
            if(f) {
                f.print(logBuffer);
                f.flush();
                f.close();
            }
        }
        logBuffer = ""; // Clear buffer
        Serial.println("Stopped. Total dist: " + String(totalDist/1000.0) + " km");
        currentState = IDLE;
        xSemaphoreGive(sdMutex);
    }
}

String getFileList() {
    
    String json;
    json.reserve(512); 
    
    if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        File root = SD.open("/");
        if(!root) {
            xSemaphoreGive(sdMutex);
            return "[]";
        }
        
        // Collect all files into array
        struct FileInfo {
            String name;
            size_t size;
        };
        FileInfo files[50];
        int fileCount = 0;
        
        File f = root.openNextFile();
        while(f && fileCount < 50) {
            if(!f.isDirectory()) {
                files[fileCount].name = String(f.name());
                files[fileCount].size = f.size();
                fileCount++;
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
        xSemaphoreGive(sdMutex);
        
        // Sort descending (newest first) - simple bubble sort
        for(int i = 0; i < fileCount - 1; i++) {
            for(int j = 0; j < fileCount - i - 1; j++) {
                if(files[j].name < files[j+1].name) {
                    FileInfo temp = files[j];
                    files[j] = files[j+1];
                    files[j+1] = temp;
                }
            }
        }
        
        // Build JSON
        json = "[";
        for(int i = 0; i < fileCount; i++) {
            if(i > 0) json += ",";
            String fname = files[i].name;
            fname.replace("\"", "\\\"");
            json += "{\"name\":\"" + fname + "\",\"size\":" + String(files[i].size) + "}";
        }
    } else {
        json = "[]";
    }
    
    json += "]";
    return json;
}

void displayLoop() {
    // Use COPY of data to avoid holding mutex during slow I2C display update!
    TrackerStatus statusCopy;
    if(xSemaphoreTake(sdMutex, 0) == pdTRUE) {
        statusCopy = sharedStatus;
        xSemaphoreGive(sdMutex);
    } else {
        //If busy, just use sharedStatus (updated under mutex anyway, atomic struct copy safer than reading gps object)
        statusCopy = sharedStatus; 
    }

    display.clearDisplay();

    // Top Bar
    display.setCursor(0,0);
    display.print(sdReady ? "SD" : "NO SD");
    display.setCursor(50,0);
    display.printf("SAT:%d", statusCopy.sats);
    display.setCursor(95,0);
    
    if(statusCopy.state == RECORDING) display.print("REC");
    else if(statusCopy.state == PAUSED) display.print("PAUSE");
    else display.print("IDLE");
    
    display.drawLine(0,9,128,9, WHITE);

    // Main Info - Speed
    display.setTextSize(2);
    display.setCursor(0,18);
    display.printf("%.1f", statusCopy.speed);
    display.setTextSize(1);
    display.print(" km/h");

    // Bottom Info
    display.setCursor(0,45);
    
    if(statusCopy.state == IDLE) {
        String ip = (WiFi.status() == WL_CONNECTED) ? 
                    WiFi.localIP().toString() : 
                    WiFi.softAPIP().toString();
        display.print(ip);
    } else {
        display.printf("D: %.2fkm", statusCopy.dist/1000.0);
    }
    
    display.setCursor(0,55);
    if(gpsFix) { 
        display.printf("%.4f, %.4f", statusCopy.lat, statusCopy.lon);
    } else {
        display.print("Szukam GPS...");
    }

    display.display();
}
