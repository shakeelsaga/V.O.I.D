// ---------------------------------------------------------
// V.O.I.D. Gateway - Hardware Agnostic DTN & Bridge
// ---------------------------------------------------------
// Architecture: ESP32 / ESP8266 Compatible
// Description: Acts as the central mesh receiver. Connects to the local network,
// broadcasts a dynamic Lighthouse beacon for Edge Nodes, and bridges ESP-NOW 
// telemetry payloads to an upstream MQTT broker. Implements LittleFS for 
// Delay-Tolerant Networking (DTN) during network outages.

#ifdef ESP32
  #include <WiFi.h>
  #include <esp_now.h>
  #include <LittleFS.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <espnow.h>
  #include <LittleFS.h>
  extern "C" {
    #include <user_interface.h>
  }
#else
  #error "Architecture not supported. Target must be ESP8266 or ESP32."
#endif

#include <PubSubClient.h>

// --- NETWORK CONFIGURATION ---
// USER ACTION REQUIRED: Update these credentials before deployment.
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqtt_broker_ip = "YOUR_MQTT_BROKER_IP"; 

WiFiClient espClient;
PubSubClient mqtt(espClient);

// Payload structure must perfectly mirror the Edge Node definitions
typedef struct SurvivorPayload {
    uint8_t nodeId;      
    uint8_t batteryPct;  
    uint8_t cpuLoad;     
    bool isSosActive;    
} SurvivorPayload;

SurvivorPayload incomingTelemetry;
volatile bool newDataReady = false; 

unsigned long lastReconnectAttempt = 0;

// ---------------------------------------------------------
// Asynchronous Receiver Callback
// ---------------------------------------------------------
#ifdef ESP32
// Updated signature for ESP32 Arduino Core v3.x
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
#elif defined(ESP8266)
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
#endif
    memcpy((void*)&incomingTelemetry, incomingData, sizeof(incomingTelemetry));
    newDataReady = true; 
}

// ---------------------------------------------------------
// Delay-Tolerant Networking (DTN) Storage Module
// ---------------------------------------------------------
void saveToFlash(const char* jsonPayload) {
    File file = LittleFS.open("/void_buffer.txt", "a");
    if(!file) {
        Serial.println("[DTN] ERROR: Flash storage unavailable.");
        return;
    }
    file.println(jsonPayload); 
    file.close();
    Serial.println("[DTN] Network unreachable. Payload buffered to local flash.");
}

void flushFlashBuffer() {
    if (!LittleFS.exists("/void_buffer.txt")) return;

    File file = LittleFS.open("/void_buffer.txt", "r");
    if (!file || file.size() == 0) {
        if (file) file.close();
        return;
    }

    Serial.println("\n[DTN] --- INITIATING FLASH BUFFER FLUSH ---");
    while (file.available()) {
        String payload = file.readStringUntil('\n');
        payload.trim(); 
        
        if (payload.length() > 0) {
            mqtt.publish("void/telemetry", payload.c_str());
            Serial.print("[DTN] Flushed: ");
            Serial.println(payload);
            delay(50); // Pacing prevents flooding the upstream MQTT broker
        }
    }
    file.close();
    
    LittleFS.remove("/void_buffer.txt");
    Serial.println("[DTN] --- BUFFER FLUSH COMPLETE ---\n");
}

// ---------------------------------------------------------
// MQTT Connection Manager
// ---------------------------------------------------------
void reconnectMqtt() {
    if (millis() - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = millis();
        Serial.print("[MQTT] Attempting connection...");
        
        String clientId = "VOID-Gateway-";
        clientId += String(random(0xffff), HEX);
        
        if (mqtt.connect(clientId.c_str())) {
            Serial.println(" Established.");
            flushFlashBuffer(); // Attempt to offload stale data upon reconnection
        } else {
            Serial.print(" Failed, rc=");
            Serial.print(mqtt.state());
            Serial.println(" (Retrying asynchronously)");
        }
    }
}

// ---------------------------------------------------------
// System Initialization
// ---------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- V.O.I.D. Gateway Initializing ---");

    // 1. Initialize Flash File System
    #ifdef ESP32
        if(!LittleFS.begin(true)){
    #elif defined(ESP8266)
        if(!LittleFS.begin()){
    #endif
            Serial.println("[SYS] FATAL: LittleFS Mount Failed. Halting.");
            return;
        }
    Serial.println("[SYS] Flash Storage Mounted.");

    // 2. Upstream Wi-Fi Connection
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[SYS] Upstream Wi-Fi Connected.");

    // 3. Lighthouse Beacon Initialization (Auto-Discovery)
    int routerChannel = WiFi.channel();
    Serial.print("[SYS] Upstream Assigned Channel: ");
    Serial.println(routerChannel);

    // Turn on the Lighthouse Beacon (With SSID Injection)
    WiFi.mode(WIFI_AP_STA); 
    
    // Fetch the true Station MAC and strip the colons
    String macStr = WiFi.macAddress();
    macStr.replace(":", ""); 
    
    // Combine them to create the dynamic Lighthouse name
    String lighthouseSSID = "VOID_" + macStr;
    
    // Broadcast the dynamic Lighthouse
    WiFi.softAP(lighthouseSSID.c_str(), "", routerChannel);
    
    Serial.print("[SYS] Lighthouse Beacon Active: ");
    Serial.println(lighthouseSSID);

    // 4. ESP-NOW Mesh Setup
    #ifdef ESP8266
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    #endif

    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: ESP-NOW initialization failed.");
        return;
    }
    
    // Typecasting the callback ensures strict C++ compiler compliance across core versions
    #ifdef ESP32
        esp_now_register_recv_cb(OnDataRecv);
    #elif defined(ESP8266)
        esp_now_register_recv_cb(reinterpret_cast<esp_now_recv_cb_t>(OnDataRecv));
    #endif

    // 5. MQTT Configuration
    mqtt.setServer(mqtt_broker_ip, 1883);
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    // Phase 1: Rapid-process incoming radio payloads
    if (newDataReady) {
        char jsonPayload[128];
        snprintf(jsonPayload, sizeof(jsonPayload), 
                 "{\"node_id\":%d, \"battery\":%d, \"cpu\":%d, \"sos_alert\":%d}", 
                 incomingTelemetry.nodeId, 
                 incomingTelemetry.batteryPct, 
                 incomingTelemetry.cpuLoad, 
                 incomingTelemetry.isSosActive);

        if (mqtt.connected()) {
            Serial.print("[PUB] Live Stream: ");
            Serial.println(jsonPayload);
            mqtt.publish("void/telemetry", jsonPayload);
        } else {
            saveToFlash(jsonPayload);
        }
        newDataReady = false; 
    }

    // Phase 2: Background process upstream connectivity
    if (!mqtt.connected()) {
        reconnectMqtt();
    } else {
        mqtt.loop();
    }
}