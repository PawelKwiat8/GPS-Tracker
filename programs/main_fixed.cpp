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
#define LOG_BUFFER_SIZE 512
#define AUTO_PAUSE_SPEED 2.0 // km/h
#define AUTO_PAUSE_TIME 5000 // ms
#define MIN_DIST 5.0 // meters
#define GPS_READ_LIMIT 100 // Max NMEA chars per loop iteration

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

// Struktura do współdzielenia stanu z wątkiem serwera (Atomowość)
struct TrackerStatus {
    double lat, lon, speed, alt, dist;
    int sats;
    float ax, ay, az;
    int state;
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

void setup() {
    Serial.begin(115200);
    
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
    while(gpsSerial.available() && gpsCharsRead < GPS_READ_LIMIT) {
        gps.encode(gpsSerial.read());
        gpsCharsRead++;
    }
    
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
}

// --- IMPLEMENTACJA ---

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
        json.reserve(256);
        
        // Szybka próba pobrania mutexu (nie blokujemy serwera)
        if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Czytamy z kopii (sharedStatus), nie z 'gps'
            json = "{";
            json += "\"state\":" + String(sharedStatus.state) + ",";
            json += "\"sats\":" + String(sharedStatus.sats) + ",";
            json += "\"lat\":" + String(sharedStatus.lat, 6) + ",";
            json += "\"lon\":" + String(sharedStatus.lon, 6) + ",";
            json += "\"speed\":" + String(sharedStatus.speed, 1) + ",";
            json += "\"alt\":" + String(sharedStatus.alt, 1) + ",";
            json += "\"dist\":" + String(sharedStatus.dist, 1) + ",";
            json += "\"ax\":" + String(sharedStatus.ax, 2) + ",";
            json += "\"ay\":" + String(sharedStatus.ay, 2) + ",";
            json += "\"az\":" + String(sharedStatus.az, 2);
            json += "}";
            xSemaphoreGive(sdMutex);
            
            request->send(200, "application/json", json);
        } else {
            request->send(503, "text/plain", "Busy");
        }
    });

    // Controls
    server.on("/api/start", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState == IDLE) {
            startRec();
        } else if(currentState == PAUSED) {
            currentState = RECORDING; // Resume
            Serial.println("Resumed");
        }
        request->send(200);
    });
    
    server.on("/api/pause", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState == RECORDING) {
            currentState = PAUSED;
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

    server.on("/api/stop", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState != IDLE) {
            stopRec();
        }
        request->send(200);
    });
    
    server.on("/api/discard", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState != IDLE) {
            currentState = IDLE;
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

    // Files API
    server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request){
        String list = getFileList();
        request->send(200, "application/json", list);
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
        sharedStatus.lat = gps.location.isValid() ? gps.location.lat() : 0.0;
        sharedStatus.lon = gps.location.isValid() ? gps.location.lng() : 0.0;
        sharedStatus.speed = gps.speed.kmph();
        sharedStatus.alt = gps.altitude.meters();
        sharedStatus.sats = (int)gps.satellites.value();
        sharedStatus.dist = totalDist;
        sharedStatus.ax = mpuReady ? mpu.getAccX() : 0.0;
        sharedStatus.ay = mpuReady ? mpu.getAccY() : 0.0;
        sharedStatus.az = mpuReady ? mpu.getAccZ() : 0.0;
        sharedStatus.state = currentState;
        xSemaphoreGive(sdMutex);
    }
}

bool checkMotion() {
    bool gpsMoving = (gps.speed.kmph() > AUTO_PAUSE_SPEED);
    if(!mpuReady) return gpsMoving;
    float gMagnitude = sqrt(pow(mpu.getAccX(), 2) + pow(mpu.getAccY(), 2) + pow(mpu.getAccZ(), 2));
    bool imuMoving = (fabs(gMagnitude - 1.0) > 0.15);
    return gpsMoving || imuMoving;
}

void logicLoop() {
    gpsFix = gps.location.isValid();
    
    // Zawsze aktualizuj status dla WWW
    updateSharedStatus();
    
    if(currentState == IDLE) return;

    if(checkMotion()) {
        lastMotionTime = millis();
        if(currentState == PAUSED) {
            currentState = RECORDING;
            totalPaused += (millis() - pauseStart);
            Serial.println("Auto-resumed");
        }
    } else {
        if(currentState == RECORDING && (millis() - lastMotionTime > AUTO_PAUSE_TIME)) {
            currentState = PAUSED;
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

    // Obliczenia na zmiennych lokalnych (bez mutexa)
    double d = TinyGPSPlus::distanceBetween(
        gps.location.lat(), gps.location.lng(), 
        lastLat, lastLon
    );
    
    if(d > MIN_DIST || lastLat == 0) {
        char line[150];
        snprintf(line, sizeof(line), 
            "%lu,%.6f,%.6f,%.1f,%.1f,%d,%.2f,%.2f,%.2f\n",
            millis(),
            gps.location.lat(),
            gps.location.lng(),
            gps.speed.kmph(),
            gps.altitude.meters(),
            (int)gps.satellites.value(),
            mpuReady ? mpu.getAccX() : 0.0,
            mpuReady ? mpu.getAccY() : 0.0,
            mpuReady ? mpu.getAccZ() : 0.0
        );
        
        // Zapis do logBuffer i ewentualny flush POD MUTEXEM
        if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            logBuffer += line;
            
            // Aktualizacja stanu
            if(lastLat != 0) totalDist += d;
            lastLat = gps.location.lat();
            lastLon = gps.location.lng();

            if(logBuffer.length() >= LOG_BUFFER_SIZE) {
                File f = SD.open(currentFileName, FILE_APPEND);
                if(f) {
                    f.print(logBuffer);
                    f.close();
                    logBuffer = "";
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
        currentFileName = "/log_" + String(millis()) + ".csv";
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
    if(!sdReady) return "[]";
    
    String json;
    json.reserve(512); 
    json = "[";
    
    if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        File root = SD.open("/");
        if(!root) {
            xSemaphoreGive(sdMutex);
            return "[]";
        }
        
        File f = root.openNextFile();
        bool first = true;
        int fileCount = 0;
        
        while(f && fileCount < 50) {
            if(!f.isDirectory()) {
                if(!first) json += ",";
                String fname = String(f.name());
                fname.replace("\"", "\\\"");
                json += "{\"name\":\"" + fname + "\",\"size\":" + String(f.size()) + "}";
                first = false;
                fileCount++;
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
        xSemaphoreGive(sdMutex);
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
