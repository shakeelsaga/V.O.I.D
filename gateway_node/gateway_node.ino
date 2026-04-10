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

#include <MQTT.h>
WiFiClient espClient;
MQTTClient mqtt(256); // Allocate a 256-byte buffer for QoS tracking
#include "secrets.h"
#include "node_registry.h"

// --- NETWORK CONFIGURATION ---
const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASS;
const char* mqtt_broker_ip = SECRET_MQTT_BROKER_IP; 

// Payload structure must perfectly mirror the Edge Node definitions
typedef struct SurvivorPayload {
    uint8_t nodeId;      
    uint8_t batteryPct;  
    uint8_t cpuLoad;     
    bool isSosActive;    
} SurvivorPayload;

// --- V2.0 ESP-NOW HARDWARE QUEUE ---
#define QUEUE_SIZE 20
SurvivorPayload rxQueue[QUEUE_SIZE];
volatile int queueHead = 0; 
volatile int queueTail = 0; 
unsigned long lastReconnectAttempt = 0;

// --- V2.0 DTN RAM BATCHING & DELTA TRACKING ---
#define RAM_BUFFER_SIZE 50 
#define MAX_NODES 256 

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
    // Calculate the next available slot in the line
    int nextHead = (queueHead + 1) % QUEUE_SIZE;
    
    // If the queue isn't completely full, save the data and move the head
    if (nextHead != queueTail) { 
        memcpy((void*)&rxQueue[queueHead], incomingData, sizeof(SurvivorPayload));
        queueHead = nextHead;
    }
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
    
    // Reset the RAM buffer index
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
    bool flushComplete = true; // Track if the entire file was successfully sent
    
    while (file.available()) {
        String payload = file.readStringUntil('\n');
        payload.trim(); 
        
        if (payload.length() > 0) {
            // Check physical connection and explicitly use QoS 1
            if (WiFi.status() == WL_CONNECTED && mqtt.publish("void/telemetry", payload.c_str(), false, 1)) {
                Serial.print("[DTN] Flushed: ");
                Serial.println(payload);
                delay(50); // Pacing prevents flooding the upstream MQTT broker
            } else {
                Serial.println("[DTN] ERROR: Link severed mid-flush. Halting to protect data.");
                flushComplete = false; 
                break; // Stop reading the file!
            }
        }
    }
    file.close();
    
    // Only delete the flash file if EVERY payload was successfully ACKed
    if (flushComplete) {
        LittleFS.remove("/void_buffer.txt");
        Serial.println("[DTN] --- BUFFER FLUSH COMPLETE ---\n");
    }
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
            Serial.print(" Failed, error code=");
            Serial.print(mqtt.lastError());
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

    // Initialize Flash File System
    #ifdef ESP32
        if(!LittleFS.begin(true)){
    #elif defined(ESP8266)
        if(!LittleFS.begin()){
    #endif
            Serial.println("[SYS] FATAL: LittleFS Mount Failed. Halting.");
            return;
        }
    Serial.println("[SYS] Flash Storage Mounted.");

    // Upstream Wi-Fi Connection
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[SYS] Upstream Wi-Fi Connected.");

    // Lighthouse Beacon Initialization (Auto-Discovery)
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

    // ESP-NOW Mesh Setup
    #ifdef ESP8266
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    #endif

    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: ESP-NOW initialization failed.");
        return;
    }
    
    // Set the Primary Master Key
    #ifdef ESP32
        esp_now_set_pmk(PMK_KEY);
    #elif defined(ESP8266)
        esp_now_set_kok(PMK_KEY, 16);
    #endif

    // Register Authorized Edge Nodes dynamically to enable decryption
    #ifdef ESP32
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        
        // Define static parameters once
        peerInfo.channel = routerChannel;
        peerInfo.encrypt = true;
        memcpy(peerInfo.lmk, LMK_KEY, 16);
        
        // Loop through the registry
        for (int i = 0; i < numAuthorizedNodes; i++) {
            memcpy(peerInfo.peer_addr, authorizedEdgeNodes[i], 6);
            esp_now_add_peer(&peerInfo);
        }
        
    #elif defined(ESP8266)
        for (int i = 0; i < numAuthorizedNodes; i++) {
            // Using (uint8_t *) to cast away const-ness and satisfy the older ESP8266 API
            esp_now_add_peer((uint8_t *)authorizedEdgeNodes[i], ESP_NOW_ROLE_SLAVE, routerChannel, (uint8_t *)LMK_KEY, 16);
        }
    #endif

    // Typecasting the callback ensures strict C++ compiler compliance across core versions
    #ifdef ESP32
        esp_now_register_recv_cb(OnDataRecv);
    #elif defined(ESP8266)
        esp_now_register_recv_cb(reinterpret_cast<esp_now_recv_cb_t>(OnDataRecv));
    #endif

    // MQTT Configuration
    mqtt.begin(mqtt_broker_ip, 1883, espClient);
    
    // Set Options: keepAlive (5s), cleanSession (true), timeout (2000ms)
    mqtt.setOptions(5, true, 2000);
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    // Rapid-process all incoming radio payloads waiting in the queue
    while (queueTail != queueHead) {
        // Grab the oldest payload from the queue
        SurvivorPayload currentPayload = rxQueue[queueTail];
        
        // Move the tail forward to "delete" it from the line
        queueTail = (queueTail + 1) % QUEUE_SIZE;
        
        char jsonPayload[128];
        snprintf(jsonPayload, sizeof(jsonPayload), 
                  "{\"node_id\":%d, \"battery\":%d, \"cpu\":%d, \"sos_alert\":%d}", 
                  currentPayload.nodeId, currentPayload.batteryPct, 
                  currentPayload.cpuLoad, currentPayload.isSosActive);

        bool publishSuccess = false;

        // Check physical Wi-Fi link AND logical MQTT link
        if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
            
            // Attempt the publish with QoS 1 (Requires Server PUBACK)
            // Parameters: topic, payload, retained, qos
            publishSuccess = mqtt.publish("void/telemetry", jsonPayload, false, 1);
            
            if (publishSuccess) {
                Serial.print("[PUB] Upstream ACK:");
                Serial.println(jsonPayload);
            } else {
                Serial.println("[SYS] WARNING: Server failed to ACK. Rerouting to DTN.");
            }
        }

        // Fallback: If disconnected OR if the publish failed, trigger the DTN
        if (!publishSuccess) {
            // --- V2.0 OFFLINE DELTA COMPRESSION ---
            uint8_t id = currentPayload.nodeId; 
            
            if (id < MAX_NODES) {
                bool criticalChange = false;
                
                // Evaluate Delta
                if (!networkState[id].active) {
                    criticalChange = true; 
                    networkState[id].active = true;
                } else {
                    if (currentPayload.isSosActive && !networkState[id].lastSos) criticalChange = true;
                    if (networkState[id].lastBattery > currentPayload.batteryPct + 5) criticalChange = true;
                    if (currentPayload.batteryPct > networkState[id].lastBattery + 5) criticalChange = true;
                }
                
                // Route Data
                if (criticalChange) {
                    networkState[id].lastBattery = currentPayload.batteryPct;
                    networkState[id].lastSos = currentPayload.isSosActive;
                    
                    ramBuffer[ramBufferCount] = currentPayload;
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
    }

    // Background process upstream connectivity
    if (!mqtt.connected()) {
        reconnectMqtt();
    } else {
        mqtt.loop();
    }
}