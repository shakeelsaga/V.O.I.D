// ---------------------------------------------------------
// V.O.I.D. Gateway - Hardware Agnostic DTN & Bridge
// ---------------------------------------------------------
// Architecture: ESP32 / ESP8266 Compatible

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
#include "secrets.h"
#include "node_registry.h"

// --- NETWORK CONFIGURATION ---
const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASS;
const char* mqtt_broker_ip = SECRET_MQTT_BROKER_IP; 

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

// --- V2.0 DTN RAM BATCHING & DELTA TRACKING ---
#define RAM_BUFFER_SIZE 50 
#define MAX_NODES 256 // Expanded for dynamic MAC-derived IDs

SurvivorPayload ramBuffer[RAM_BUFFER_SIZE];
int ramBufferCount = 0;

// Struct to hold the last known state of an Edge Node
struct NodeState {
    bool active = false;
    uint8_t lastBattery = 0;
    bool lastSos = false;
};
NodeState networkState[MAX_NODES];

// ---------------------------------------------------------
// Asynchronous Receiver Callback
// ---------------------------------------------------------
#ifdef ESP32
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
void flushRamToFlash() {
    if (ramBufferCount == 0) return;
    
    File file = LittleFS.open("/void_buffer.txt", "a");
    if(!file) {
        Serial.println("[DTN] ERROR: Flash storage unavailable.");
        return;
    }
    
    Serial.println("\n[DTN] --- EXECUTING BATCH FLASH WRITE ---");
    for (int i = 0; i < ramBufferCount; i++) {
        char jsonPayload[128];
        snprintf(jsonPayload, sizeof(jsonPayload), 
                  "{\"node_id\":%d, \"battery\":%d, \"cpu\":%d, \"sos_alert\":%d}", 
                  ramBuffer[i].nodeId, ramBuffer[i].batteryPct, 
                  ramBuffer[i].cpuLoad, ramBuffer[i].isSosActive);
        file.println(jsonPayload);
    }
    
    file.close();
    Serial.print("[DTN] Successfully batched ");
    Serial.print(ramBufferCount);
    Serial.println(" payloads to non-volatile flash memory.\n");
    
    ramBufferCount = 0; 
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
            delay(50); 
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
            if (ramBufferCount > 0) flushRamToFlash();
            flushFlashBuffer(); 
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

    #ifdef ESP32
        if(!LittleFS.begin(true)){
    #elif defined(ESP8266)
        if(!LittleFS.begin()){
    #endif
            Serial.println("[SYS] FATAL: LittleFS Mount Failed. Halting.");
            return;
        }
    Serial.println("[SYS] Flash Storage Mounted.");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[SYS] Upstream Wi-Fi Connected.");

    int routerChannel = WiFi.channel();
    Serial.print("[SYS] Upstream Assigned Channel: ");
    Serial.println(routerChannel);

    WiFi.mode(WIFI_AP_STA); 
    String macStr = WiFi.macAddress();
    macStr.replace(":", ""); 
    String lighthouseSSID = "VOID_" + macStr;
    WiFi.softAP(lighthouseSSID.c_str(), "", routerChannel);
    
    Serial.print("[SYS] Lighthouse Beacon Active: ");
    Serial.println(lighthouseSSID);

    #ifdef ESP8266
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    #endif

    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: ESP-NOW initialization failed.");
        return;
    }
    
    #ifdef ESP32
        esp_now_set_pmk(PMK_KEY);
    #elif defined(ESP8266)
        esp_now_set_kok(PMK_KEY, 16);
    #endif

    // Register Authorized Edge Nodes dynamically
    #ifdef ESP32
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        peerInfo.channel = routerChannel;
        peerInfo.encrypt = true;
        memcpy(peerInfo.lmk, LMK_KEY, 16);
        
        for (int i = 0; i < numAuthorizedNodes; i++) {
            memcpy(peerInfo.peer_addr, authorizedEdgeNodes[i], 6);
            esp_now_add_peer(&peerInfo);
        }
    #elif defined(ESP8266)
        for (int i = 0; i < numAuthorizedNodes; i++) {
            esp_now_add_peer((uint8_t *)authorizedEdgeNodes[i], ESP_NOW_ROLE_SLAVE, routerChannel, (uint8_t *)LMK_KEY, 16);
        }
    #endif

    #ifdef ESP32
        esp_now_register_recv_cb(OnDataRecv);
    #elif defined(ESP8266)
        esp_now_register_recv_cb(reinterpret_cast<esp_now_recv_cb_t>(OnDataRecv));
    #endif

    mqtt.setServer(mqtt_broker_ip, 1883);
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    if (newDataReady) {
        char jsonPayload[128];
        snprintf(jsonPayload, sizeof(jsonPayload), 
                  "{\"node_id\":%d, \"battery\":%d, \"cpu\":%d, \"sos_alert\":%d}", 
                  incomingTelemetry.nodeId, incomingTelemetry.batteryPct, 
                  incomingTelemetry.cpuLoad, incomingTelemetry.isSosActive);

        if (mqtt.connected()) {
            Serial.print("[PUB] Live Stream: ");
            Serial.println(jsonPayload);
            mqtt.publish("void/telemetry", jsonPayload);
        } else {
            // --- V2.0 OFFLINE DELTA COMPRESSION ---
            uint8_t id = incomingTelemetry.nodeId;
            
            if (id < MAX_NODES) {
                bool criticalChange = false;
                
                if (!networkState[id].active) {
                    criticalChange = true; 
                    networkState[id].active = true;
                } else {
                    if (incomingTelemetry.isSosActive && !networkState[id].lastSos) criticalChange = true;
                    if (networkState[id].lastBattery > incomingTelemetry.batteryPct + 5) criticalChange = true;
                    if (incomingTelemetry.batteryPct > networkState[id].lastBattery + 5) criticalChange = true;
                }
                
                if (criticalChange) {
                    networkState[id].lastBattery = incomingTelemetry.batteryPct;
                    networkState[id].lastSos = incomingTelemetry.isSosActive;
                    
                    ramBuffer[ramBufferCount] = incomingTelemetry;
                    ramBufferCount++;
                    
                    Serial.print("[DTN] State change detected for Node ");
                    Serial.print(id);
                    Serial.print(". Appended to RAM Buffer (");
                    Serial.print(ramBufferCount);
                    Serial.println("/50)");
                    
                    if (ramBufferCount >= RAM_BUFFER_SIZE) {
                        flushRamToFlash();
                    }
                } else {
                    Serial.print("[DTN] Node ");
                    Serial.print(id);
                    Serial.println(" telemetry redundant. Discarding to protect flash wear.");
                }
            }
        }
        newDataReady = false; 
    }

    if (!mqtt.connected()) {
        reconnectMqtt();
    } else {
        mqtt.loop();
    }
}