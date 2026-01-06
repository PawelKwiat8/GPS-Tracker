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
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define LOG_BUFFER_SIZE 512
#define AUTO_PAUSE_SPEED 2.0 // km/h
#define AUTO_PAUSE_TIME 5000 // ms
#define MIN_DIST 5.0 // meters

// --- OBIEKTY ---
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);
MPU6050 mpu(Wire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
AsyncWebServer server(80);

// --- ZMIENNE STANU ---
enum State { IDLE, RECORDING, PAUSED };
State currentState = IDLE;

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

void setup() {
    Serial.begin(115200);
    setupHardware();
    setupWiFi();
    setupServer();
}

void loop() {
    // 1. GPS Feed (Always run)
    while(gpsSerial.available()) gps.encode(gpsSerial.read());
    
    // 2. MPU Update (Always run for IMU)
    if(mpuReady) mpu.update();

    // 3. Logic
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
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED Fail");
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
    } else {
        Serial.println("MPU Fail");
    }

    // SD
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if(SD.begin(SD_CS)) {
        sdReady = true;
    } else {
        Serial.println("SD Fail");
    }

    // GPS
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
}

void setupWiFi() {
    // GATEWAY 0.0.0.0 TRICK for Mobile Data
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(0,0,0,0), IPAddress(255,255,255,0));
    WiFi.softAP("ESP32-Tracker", "12345678");
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

    // Fix for Android/Chrome connection issues
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
}

void setupServer() {
    // Main Page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    // Status API
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"state\":" + String(currentState) + ",";
        json += "\"sats\":" + String(gps.satellites.value()) + ",";
        json += "\"lat\":" + String(gps.location.isValid() ? gps.location.lat() : 0.0, 6) + ",";
        json += "\"lon\":" + String(gps.location.isValid() ? gps.location.lng() : 0.0, 6) + ",";
        json += "\"speed\":" + String(gps.speed.kmph()) + ",";
        json += "\"alt\":" + String(gps.altitude.meters()) + ",";
        json += "\"dist\":" + String(totalDist) + ",";
        json += "\"ax\":" + String(mpu.getAccX()) + ",";
        json += "\"ay\":" + String(mpu.getAccY()) + ",";
        json += "\"az\":" + String(mpu.getAccZ()); // no comma at end
        json += "}";
        request->send(200, "application/json", json);
    });

    // Controls
    server.on("/api/start", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState == IDLE) startRec();
        request->send(200);
    });
    server.on("/api/stop", HTTP_GET, [](AsyncWebServerRequest *request){
        if(currentState != IDLE) stopRec();
        request->send(200);
    });

    // Files API
    server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", getFileList());
    });

    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("file")) {
            String fname = request->getParam("file")->value();
            if(!fname.startsWith("/")) fname = "/" + fname; // SANITIZE PATH
            
            if(SD.exists(fname)) request->send(SD, fname, "text/plain");
            else request->send(404, "text/plain", "Not Found");
        } else request->send(400);
    });

    server.on("/delete", HTTP_DELETE, [](AsyncWebServerRequest *request){
         if(request->hasParam("file")) {
            String fname = request->getParam("file")->value();
            if(!fname.startsWith("/")) fname = "/" + fname; // SANITIZE PATH

            if(fname == currentFileName && currentState != IDLE) { 
                request->send(400, "text/plain", "Nie mozna usunac pliku w trakcie nagrywania!"); 
                return; 
            }
            if(SD.exists(fname)) { 
                SD.remove(fname); 
                request->send(200, "text/plain", "Deleted"); 
            }
            else request->send(404, "text/plain", "Not Found");
        } else request->send(400);
    });

    server.begin();
}

// --- LOGIKA ---

bool checkMotion() {
    if(!mpuReady) return gps.speed.kmph() > 1.0;
    // Sensor fusion: either GPS speed or IMU vibration
    float gVar = sqrt(pow(mpu.getAccX(), 2) + pow(mpu.getAccY(), 2) + pow(mpu.getAccZ(), 2)); 
    return (gps.speed.kmph() > AUTO_PAUSE_SPEED) || (abs(gVar - 1.0) > 0.15);
}

void logicLoop() {
    gpsFix = gps.location.isValid();
    
    if(currentState == IDLE) return;

    if(checkMotion()) {
        lastMotionTime = millis();
        if(currentState == PAUSED) {
            currentState = RECORDING;
            totalPaused += (millis() - pauseStart);
        }
    } else {
        if(currentState == RECORDING && (millis() - lastMotionTime > AUTO_PAUSE_TIME)) {
            currentState = PAUSED;
            pauseStart = millis();
        }
    }

    if(currentState == RECORDING) logData();
}

void logData() {
    if(!gpsFix || !sdReady) return;

    double d = TinyGPSPlus::distanceBetween(gps.location.lat(), gps.location.lng(), lastLat, lastLon);
    if(d > MIN_DIST || d < 0.1) { // 0.1 for first point
         // CSV Format: time,lat,lon,speed,alt,sats,ax,ay,az
         String line = String(millis()) + "," + 
                       String(gps.location.lat(), 6) + "," +
                       String(gps.location.lng(), 6) + "," +
                       String(gps.speed.kmph(), 1) + "," +
                       String(gps.altitude.meters(), 1) + "," +
                       String(gps.satellites.value()) + "," +
                       String(mpu.getAccX(), 2) + "," +
                       String(mpu.getAccY(), 2) + "," +
                       String(mpu.getAccZ(), 2) + "\n";
        
        logBuffer += line;
        
        if(lastLat != 0) totalDist += d;
        lastLat = gps.location.lat();
        lastLon = gps.location.lng();

        if(logBuffer.length() > LOG_BUFFER_SIZE) {
            File f = SD.open(currentFileName, FILE_APPEND);
            if(f) {
                f.print(logBuffer);
                f.close();
                logBuffer = "";
            }
        }
    }
}

void startRec() {
    if(!sdReady) return;
    
    // Name: /log_timestamp.csv
    currentFileName = "/log_" + String(millis()) + ".csv";
    
    // Header
    File f = SD.open(currentFileName, FILE_WRITE);
    if(f) {
        f.println("time_ms,lat,lon,speed_kmh,alt_m,sats,ax,ay,az");
        f.close();
    }
    
    currentState = RECORDING;
    sessionStart = millis();
    lastMotionTime = millis();
    totalPaused = 0;
    totalDist = 0;
    lastLat = 0; 
    lastLon = 0;
    logBuffer = "";
}

void stopRec() {
    // Flush
    if(logBuffer.length() > 0) {
        File f = SD.open(currentFileName, FILE_APPEND);
        if(f) { f.print(logBuffer); f.close(); }
    }
    currentState = IDLE;
    logBuffer = "";
}

String getFileList() {
    if(!sdReady) return "[]";
    String json = "[";
    File root = SD.open("/");
    File f = root.openNextFile();
    bool first = true;
    while(f) {
        if(!f.isDirectory()) {
            if(!first) json += ",";
            json += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) + "}";
            first = false;
        }
        f = root.openNextFile();
    }
    json += "]";
    return json;
}

// --- DISPLAY ---

void displayLoop() {
    display.clearDisplay();

    // Top Bar
    display.setCursor(0,0);
    display.print(sdReady ? "SD" : "NO SD");
    display.setCursor(50,0);
    display.printf("SAT:%d", (int)gps.satellites.value());
    display.setCursor(95,0);
    if(currentState == RECORDING) display.print("REC");
    else if(currentState == PAUSED) display.print("PAUSE");
    else display.print("IDLE");
    display.drawLine(0,9,128,9, WHITE);

    // Main Info
    display.setTextSize(2);
    display.setCursor(0,18);
    display.printf("%.1f", gps.speed.kmph());
    display.setTextSize(1);
    display.print(" km/h");

    // Bottom Info
    display.setCursor(0,45);
    display.printf("D: %.2fkm", totalDist/1000.0);
    
    display.setCursor(0,55);
    if(gpsFix) display.printf("%.4f, %.4f", gps.location.lat(), gps.location.lng());
    else display.print("Gps Searching...");

    display.display();
}
