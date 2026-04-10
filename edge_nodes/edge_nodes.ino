// ---------------------------------------------------------
// V.O.I.D. Edge Node - Survivor Telemetry (V2.0 HA & Adaptive)
// ---------------------------------------------------------
// Architecture: ESP32 / ESP8266 Compatible

#ifdef ESP32
#include <WiFi.h>
#include <esp_now.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" {
    #include <user_interface.h>
}
#else
#error "Architecture not supported. Target must be ESP8266 or ESP32."
#endif

#include "secrets.h"

// --- GLOBALS ---
uint8_t gatewayMacAddress[6] = {0, 0, 0, 0, 0, 0};
uint8_t myNodeId = 0;
bool gatewayConnected = false;

// --- V2.0 EDGE SURVIVAL QUEUE ---
#define EDGE_QUEUE_SIZE 20
typedef struct SurvivorPayload {
    uint8_t nodeId;      
    uint8_t batteryPct;  
    uint8_t cpuLoad;     
    bool isSosActive;    
} SurvivorPayload;

SurvivorPayload edgeQueue[EDGE_QUEUE_SIZE];
int edgeQueueCount = 0;

SurvivorPayload outgoingTelemetry;

// --- V2.0 ADAPTIVE TELEMETRY STATE ---
uint8_t lastTxBattery = 0;
uint8_t lastTxCpu = 0;
bool lastTxSos = false;
unsigned long lastTxTime = 0;

// Hardware Sync Flags
volatile bool txComplete = false;
volatile bool txSuccess = false;

// ---------------------------------------------------------
// Utility: Derive Node ID & Parse MAC
// ---------------------------------------------------------
uint8_t deriveNodeId() {
    // Give the Wi-Fi hardware 200ms to fully power up and stabilize
    delay(200); 
    
    // Fetch and print the raw string for absolute visibility
    String fullMac = WiFi.macAddress();
    Serial.print("[SYS] Raw Hardware MAC: ");
    Serial.println(fullMac);
    
    // Parse the string safely
    if (fullMac.length() >= 17) {
        String lastByteStr = fullMac.substring(15, 17); 
        return (uint8_t) strtol(lastByteStr.c_str(), NULL, 16);
    }
    
    Serial.println("[SYS] ERROR: Hardware MAC malformed.");
    return 0; // Fallback
}

void parseMacString(String macText, uint8_t* macArray) {
    for (int i = 0; i < 6; i++) {
        String byteString = macText.substring(i * 2, i * 2 + 2);
        macArray[i] = (uint8_t) strtol(byteString.c_str(), NULL, 16);
    }
}

// ---------------------------------------------------------
// Asynchronous Transmission Callback
// ---------------------------------------------------------
#ifdef ESP32
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    txSuccess = (status == ESP_NOW_SEND_SUCCESS);
    txComplete = true;
}
#elif defined(ESP8266)
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
    txSuccess = (sendStatus == 0);
    txComplete = true;
}
#endif

// ---------------------------------------------------------
// V2.0 HA Protocol: Hunt For Gateway
// ---------------------------------------------------------
bool huntForGateway() {
    Serial.println("\n[SYS] Executing Lighthouse channel sweep...");
    
    WiFi.disconnect();
    delay(100);
    int networkCount = WiFi.scanNetworks();
    int targetChannel = 0;

    for (int i = 0; i < networkCount; i++) {
        String foundSSID = WiFi.SSID(i);
        
        if (foundSSID.startsWith("VOID_")) {
            targetChannel = WiFi.channel(i);
            String extractedMac = foundSSID.substring(5);
            parseMacString(extractedMac, gatewayMacAddress);
            
            Serial.print("[SYS] Gateway acquired on Channel ");
            Serial.print(targetChannel);
            Serial.print(" | Target MAC: ");
            Serial.println(extractedMac);
            break; 
        }
    }

    if (targetChannel == 0) {
        Serial.println("[SYS] WARNING: Gateway Lighthouse not found.");
        return false;
    }

    // Tune Hardware
    #ifdef ESP32
        WiFi.setChannel(targetChannel);
        
        // Remove old peer if exists, then add new one
        if (esp_now_is_peer_exist(gatewayMacAddress)) {
            esp_now_del_peer(gatewayMacAddress);
        }

        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, gatewayMacAddress, 6);
        peerInfo.channel = targetChannel;
        peerInfo.encrypt = true; 
        memcpy(peerInfo.lmk, LMK_KEY, 16);
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) return false;
        
    #elif defined(ESP8266)
        wifi_set_channel(targetChannel);
        
        // ESP8266 peer update
        esp_now_del_peer(gatewayMacAddress); 
        if (esp_now_add_peer(gatewayMacAddress, ESP_NOW_ROLE_COMBO, targetChannel, (uint8_t *)LMK_KEY, 16) != 0) {
            return false;
        }
    #endif

    gatewayConnected = true;
    return true;
}

// ---------------------------------------------------------
// System Initialization
// ---------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- V.O.I.D. Edge Node Booting ---");

    WiFi.mode(WIFI_STA);
    myNodeId = deriveNodeId();
    Serial.print("[SYS] Hardware-Derived Node ID: ");
    Serial.println(myNodeId);

    #ifdef ESP8266
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    #endif

    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: ESP-NOW hardware initialization failed.");
        while(1) delay(1000); 
    }
    
    // Typecast callback for strict cross-core compliance
    #ifdef ESP32
        esp_now_register_send_cb(reinterpret_cast<esp_now_send_cb_t>(OnDataSent));
        esp_now_set_pmk(PMK_KEY);
    #elif defined(ESP8266)
        esp_now_register_send_cb(reinterpret_cast<esp_now_send_cb_t>(OnDataSent));
        esp_now_set_kok(PMK_KEY, 16);
    #endif

    // Initial Hunt
    while (!huntForGateway()) {
        Serial.println("[SYS] Boot delayed. Retrying hunt in 5s...");
        delay(5000);
    }
    
    Serial.println("[SYS] Node Armed. Adaptive Telemetry sequence starting.");
}

// ---------------------------------------------------------
// V2.0 HA Protocol: Flush Survival Queue
// ---------------------------------------------------------
void flushEdgeQueue() {
    if (edgeQueueCount == 0) return;
    Serial.print("\n[QUEUE] Flushing ");
    Serial.print(edgeQueueCount);
    Serial.println(" saved payloads to Gateway...");

    for (int i = 0; i < edgeQueueCount; i++) {
        txComplete = false;
        
        #ifdef ESP32
            esp_now_send(gatewayMacAddress, (uint8_t *) &edgeQueue[i], sizeof(SurvivorPayload));
        #elif defined(ESP8266)
            esp_now_send(gatewayMacAddress, (uint8_t *) &edgeQueue[i], sizeof(SurvivorPayload));
        #endif

        // Wait up to 100ms for hardware ACK
        int timeout = 0;
        while (!txComplete && timeout < 100) { delay(1); timeout++; }
        delay(50); // Pacing
    }
    
    edgeQueueCount = 0;
    Serial.println("[QUEUE] Flush complete.");
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    // 1. Mock Sensor Ingestion (Smoothing the randoms to test Adaptive logic)
    static float mockBattery = 100.0;
    mockBattery -= 0.05; // Slowly drain
    if (mockBattery < 0) mockBattery = 100.0;
    
    outgoingTelemetry.nodeId = myNodeId; 
    outgoingTelemetry.batteryPct = (uint8_t)mockBattery; 
    outgoingTelemetry.cpuLoad = random(10, 30);     
    outgoingTelemetry.isSosActive = (random(0, 100) > 95); // Rare SOS

    // 2. V2.0 Adaptive Telemetry Logic
    bool triggerTransmission = false;
    String triggerReason = "";

    // GEAR 3: Emergency Burst Mode
    if (outgoingTelemetry.isSosActive) {
        triggerTransmission = true;
        triggerReason = "EMERGENCY SOS BURST";
    }
    // GEAR 2: Exception Mode
    else if (lastTxBattery > outgoingTelemetry.batteryPct + 2) {
        triggerTransmission = true;
        triggerReason = "BATTERY DROP EXCEPTION";
    }
    else if (abs(lastTxCpu - outgoingTelemetry.cpuLoad) > 30) {
        triggerTransmission = true;
        triggerReason = "CPU SPIKE EXCEPTION";
    }
    // GEAR 1: Heartbeat Mode
    else if (millis() - lastTxTime > 60000) {
        triggerTransmission = true;
        triggerReason = "60s HEARTBEAT";
    }

    // 3. Execution
    if (triggerTransmission) {
        Serial.print("\n--- Telemetry Triggered: "); 
        Serial.print(triggerReason); 
        Serial.println(" ---");
        
        // If Gateway is lost, append to queue immediately
        if (!gatewayConnected) {
            if (edgeQueueCount < EDGE_QUEUE_SIZE) {
                edgeQueue[edgeQueueCount] = outgoingTelemetry;
                edgeQueueCount++;
                Serial.print("[QUEUE] Gateway missing. Payload saved to volatile RAM (");
                Serial.print(edgeQueueCount);
                Serial.println("/20)");
            }
            
            // Attempt to find the Shadow Gateway
            if (huntForGateway()) {
                flushEdgeQueue();
            }
        } 
        // If Gateway is connected, attempt transmission
        else {
            txComplete = false;
            
            #ifdef ESP32
                esp_now_send(gatewayMacAddress, (uint8_t *) &outgoingTelemetry, sizeof(outgoingTelemetry));
            #elif defined(ESP8266)
                esp_now_send(gatewayMacAddress, (uint8_t *) &outgoingTelemetry, sizeof(outgoingTelemetry));
            #endif

            // Wait up to 100ms for hardware ACK
            int timeout = 0;
            while (!txComplete && timeout < 100) { delay(1); timeout++; }

            if (txSuccess) {
                Serial.println("[RADIO] Delivery Acknowledged.");
                // Update state memory only on success
                lastTxTime = millis();
                lastTxBattery = outgoingTelemetry.batteryPct;
                lastTxCpu = outgoingTelemetry.cpuLoad;
                lastTxSos = outgoingTelemetry.isSosActive;
            } else {
                Serial.println("[RADIO] Delivery Failed! Gateway presumed dead.");
                gatewayConnected = false;
                
                // Save the failed payload
                if (edgeQueueCount < EDGE_QUEUE_SIZE) {
                    edgeQueue[edgeQueueCount] = outgoingTelemetry;
                    edgeQueueCount++;
                }
            }
        }
    }

    // Pacing interval (Node sleeps for 1 second between checks)
    delay(1000); 
}