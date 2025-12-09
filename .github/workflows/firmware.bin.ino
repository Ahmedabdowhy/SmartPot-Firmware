#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
// Required for OTA
#include <addons/TokenHelper.h> 
#include <addons/RTDBHelper.h>

// --- 1. CONFIGURATION ---
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define API_KEY "AIzaSyBpT-0T1tQ-i7JtK_kh1v5V0bQC9GldfdY"
#define FIREBASE_PROJECT_ID "iot-pot-bc65c"
// FIND THIS IN FIREBASE CONSOLE -> STORAGE
#define STORAGE_BUCKET_ID "iot-pot-bc65c.appspot.com" 

// Database Paths
#define DOCUMENT_PATH "potReadings/xZsrb7lFHIYMvLjl1yiT"
#define HISTORY_COLLECTION "potReadings/xZsrb7lFHIYMvLjl1yiT/readings_history"

// --- 2. PIN DEFINITIONS (CORRECTED) ---
#define PIN_MOISTURE 34
#define PIN_UV 35
#define PIN_TEMP 4
#define PIN_FLOAT 15  
#define PIN_RELAY 21  

// --- 3. CONSTANTS ---
const int THRESHOLD_TROPICAL = 30; 
const int THRESHOLD_DESERT = 10;
const unsigned long HISTORY_INTERVAL = 3600000; 
const unsigned long AUTO_WATER_COOLDOWN = 3600000; 

// --- 4. OBJECTS ---
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
OneWire oneWire(PIN_TEMP);
DallasTemperature sensors(&oneWire);

unsigned long lastCheckTime = 0;
unsigned long lastHistoryLog = 0;
unsigned long lastAutoWaterTime = 0; 
bool isDesertMode = false; 

// TIME SYNC
const char* ntpServer = "pool.ntp.org";

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW); 
  pinMode(PIN_FLOAT, INPUT_PULLUP); 
  
  sensors.begin();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println(" Connected!");

  configTime(0, 0, ntpServer); 

  config.api_key = API_KEY;
  auth.user.email = ""; // Setup Anonymous Auth in Firebase Console!
  auth.user.password = "";
  config.time_zone = 2; 

  // CRITICAL FOR OTA:
  config.token_status_callback = tokenStatusCallback; 
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  if (millis() - lastCheckTime > 5000) {
    lastCheckTime = millis();
    
    int moisturePercent = getMoisture();
    float tempC = getTemp();
    float uvIndex = getUV();
    bool isWaterLow = digitalRead(PIN_FLOAT) == HIGH; 

    syncSystem(moisturePercent, tempC, uvIndex, isWaterLow);
    checkAutoWater(moisturePercent, isWaterLow);
  }
}

// --- LOGIC FUNCTIONS ---

void syncSystem(int moisture, float temp, float uv, bool waterLow) {
  
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", DOCUMENT_PATH)) {
      FirebaseJson &json = fbdo.jsonObject();
      FirebaseJsonData result;

      // 1. Check for OTA Update Command
      json.get(result, "fields/ota_update/booleanValue");
      if (result.success && result.boolValue == true) {
          Serial.println("⚠️ OTA UPDATE TRIGGERED! ⚠️");
          updateFirmware(); // Call the update function
          return; // Stop everything else
      }

      // 2. Check Manual Water
      json.get(result, "fields/manual_water/booleanValue");
      if (result.success && result.boolValue == true) {
          Serial.println("Manual Command Received!");
          runPump();
          logWateringEvent("manual");
          
          FirebaseJson update;
          update.set("fields/manual_water/booleanValue", false);
          Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", DOCUMENT_PATH, update.raw(), "manual_water");
      }
      
      // 3. Check Plant Mode
      json.get(result, "fields/plant_mode/integerValue");
      if(result.success) {
          isDesertMode = (result.intValue == 1);
      }
  }

  // Live Dashboard Update
  FirebaseJson content;
  content.set("fields/moisture/integerValue", moisture);
  content.set("fields/temperature/doubleValue", temp);
  content.set("fields/uv/doubleValue", uv); 
  content.set("fields/water_level_low/booleanValue", waterLow);
  Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", DOCUMENT_PATH, content.raw(), "moisture,temperature,uv,water_level_low");

  // History Log
  if (millis() - lastHistoryLog > HISTORY_INTERVAL || lastHistoryLog == 0) {
      FirebaseJson history;
      history.set("fields/moisture/integerValue", moisture);
      history.set("fields/temperature/doubleValue", temp);
      history.set("fields/uv/doubleValue", uv);
      history.set("fields/timestamp/mapValue/fields/type/stringValue", "server_timestamp"); 
      Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", HISTORY_COLLECTION, history.raw());
      lastHistoryLog = millis();
  }
}

// --- OTA FUNCTION ---
void updateFirmware() {
  Serial.println("Downloading firmware.bin from Storage...");
  
  // Create a clean object for OTA
  FirebaseData fbdo_ota;
  fbdo_ota.setResponseSize(2048); 

  // Download and Flash
  // Params: fbdo, bucketID, fileName, successCallback, failCallback
  if (!Firebase.Storage.downloadOTA(&fbdo_ota, STORAGE_BUCKET_ID, "firmware.bin", fcsDownloadCallback)) {
    Serial.println(fbdo_ota.errorReason());
  } else {
    Serial.println("Update Complete! Resetting...");
    
    // Reset the flag in Firestore so it doesn't loop-update
    FirebaseJson update;
    update.set("fields/ota_update/booleanValue", false);
    Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", DOCUMENT_PATH, update.raw(), "ota_update");
    
    delay(1000);
    ESP.restart(); // Reboot into new code
  }
}

// Progress Bar for Update
void fcsDownloadCallback(FCS_DownloadStatusInfo info) {
    if (info.status == fcs_download_status_init) {
        Serial.printf("Downloading: %d bytes\n", info.fileSize);
    } else if (info.status == fcs_download_status_download) {
        Serial.printf("Progress: %d%%\n", info.progress);
    } else if (info.status == fcs_download_status_complete) {
        Serial.println("Download & Flash Complete!");
    } else if (info.status == fcs_download_status_error) {
        Serial.printf("Error: %s\n", info.errorMsg.c_str());
    }
}

void checkAutoWater(int moisture, bool waterLow) {
    int threshold = isDesertMode ? THRESHOLD_DESERT : THRESHOLD_TROPICAL;
    if (moisture < threshold && !waterLow) {
       if (millis() - lastAutoWaterTime > AUTO_WATER_COOLDOWN) {
           Serial.println("AUTO-WATERING TRIGGERED!");
           runPump();
           lastAutoWaterTime = millis();
           logWateringEvent("auto");
       }
    }
}

void logWateringEvent(String type) {
    FirebaseJson log;
    log.set("fields/type/stringValue", type); 
    log.set("fields/timestamp/mapValue/fields/type/stringValue", "server_timestamp");
    String logPath = String(DOCUMENT_PATH) + "/watering_log";
    Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "", logPath.c_str(), log.raw());
}

float getUV() {
  int sensorValue = analogRead(PIN_UV);
  return (sensorValue * (3.3 / 4095.0)) / 0.1;
}

int getMoisture() {
  int raw = analogRead(PIN_MOISTURE);
  int percent = map(raw, 4095, 1500, 0, 100);
  return constrain(percent, 0, 100);
}

float getTemp() {
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  return (t < -100) ? 0 : t;
}

void runPump() {
  Serial.println("PUMP ON");
  digitalWrite(PIN_RELAY, HIGH); 
  delay(5000);                   
  digitalWrite(PIN_RELAY, LOW);  
  Serial.println("PUMP OFF");
}