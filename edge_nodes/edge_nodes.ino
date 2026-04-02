// ---------------------------------------------------------
// V.O.I.D. Edge Node - Survivor Telemetry
// ---------------------------------------------------------
// Architecture: ESP32 / ESP8266 Compatible
// Description: Distributed mesh sensor node. Employs a Lighthouse hunter 
// protocol to dynamically discover the central Gateway channel, bypassing 
// static channel configurations, before transmitting telemetry via ESP-NOW.

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

// --- TARGET GATEWAY MAC ADDRESS ---
uint8_t gatewayMacAddress[6] = {0, 0, 0, 0, 0, 0};

// ---------------------------------------------------------
// Utility: Convert a String MAC (e.g. "4C11AE0D83B1") to Hex Bytes
// ---------------------------------------------------------
void parseMacString(String macText, uint8_t* macArray) {
    for (int i = 0; i < 6; i++) {
        // Grab 2 characters at a time
        String byteString = macText.substring(i * 2, i * 2 + 2);
        // Convert the string to a base-16 (hex) integer and store it in the array
        macArray[i] = (uint8_t) strtol(byteString.c_str(), NULL, 16);
    }
}

// ---------------------------------------------------------
// Data Structures
// ---------------------------------------------------------
typedef struct SurvivorPayload {
    uint8_t nodeId;      
    uint8_t batteryPct;  
    uint8_t cpuLoad;     
    bool isSosActive;    
} SurvivorPayload;

SurvivorPayload outgoingTelemetry;

// ---------------------------------------------------------
// Asynchronous Transmission Callback
// ---------------------------------------------------------
#ifdef ESP32
// Restored correct signature for ESP32 Arduino Core v3.x
void OnDataSent(const wifi_tx_info_t *mac_addr, esp_now_send_status_t status) {
    Serial.print("[RADIO] Tx Status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Acknowledged" : "Delivery Failed");
}
#elif defined(ESP8266)
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
    Serial.print("[RADIO] Tx Status: ");
    Serial.println(sendStatus == 0 ? "Delivery Acknowledged" : "Delivery Failed");
}
#endif

// ---------------------------------------------------------
// System Initialization
// ---------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- V.O.I.D. Edge Node Booting ---");

    // 1. Radio Initialization
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(); 
    delay(100);

    // 2. Lighthouse Auto-Discovery Protocol
    Serial.println("[SYS] Executing Lighthouse channel sweep...");
    int targetChannel = 0;
    int networkCount = WiFi.scanNetworks();

    for (int i = 0; i < networkCount; i++) {
        String foundSSID = WiFi.SSID(i);
        
        // Look for the V.O.I.D. signature prefix
        if (foundSSID.startsWith("VOID_")) {
            targetChannel = WiFi.channel(i);
            
            // Extract the MAC address part (starts at character index 5)
            String extractedMac = foundSSID.substring(5);
            
            // Overwrite our global gatewayMacAddress array with the discovered MAC
            parseMacString(extractedMac, gatewayMacAddress);
            
            Serial.print("[SYS] Gateway acquired on Channel ");
            Serial.print(targetChannel);
            Serial.print(" | Target MAC: ");
            Serial.println(extractedMac);
            break; 
        }
    }

    if (targetChannel == 0) {
        Serial.println("[SYS] FATAL: Gateway Lighthouse not found. Suspending boot.");
        while(1) delay(1000); 
    }

    // 3. Dynamic Hardware-Agnostic Channel Tuning
    #ifdef ESP32
        WiFi.setChannel(targetChannel);
    #elif defined(ESP8266)
        wifi_set_channel(targetChannel);
    #endif

    // 4. ESP-NOW Initialization
    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: ESP-NOW hardware initialization failed.");
        return; 
    }
    
    esp_now_register_send_cb(OnDataSent);

    // 5. Peer Registration
    #ifdef ESP32
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo)); 
        memcpy(peerInfo.peer_addr, gatewayMacAddress, 6);
        peerInfo.channel = 0;      
        peerInfo.encrypt = false;  

        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("[SYS] FATAL: Failed to register Gateway peer (ESP32)");
            return;
        }

    #elif defined(ESP8266)
        esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
                 
        // Inject the dynamic 'targetChannel' instead of hardcoding '1'
        if (esp_now_add_peer(gatewayMacAddress, ESP_NOW_ROLE_SLAVE, targetChannel, NULL, 0) != 0) {
            Serial.println("[SYS] FATAL: Failed to register Gateway peer (ESP8266)");
            return;
        }
    #endif
    
    Serial.println("[SYS] Node Armed. Telemetry sequence starting.");
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    // 1. Simulate hardware sensor ingestion
    outgoingTelemetry.nodeId = 1; 
    outgoingTelemetry.batteryPct = random(10, 100); 
    outgoingTelemetry.cpuLoad = random(0, 100);     
    outgoingTelemetry.isSosActive = (random(0, 10) > 8); 

    Serial.println("\n--- Telemetry Generation ---");
    Serial.print("Target ID: "); Serial.print(outgoingTelemetry.nodeId);
    Serial.print(" | VCC: "); Serial.print(outgoingTelemetry.batteryPct); Serial.println("%");

    // 2. Fire ESP-NOW Payload
    #ifdef ESP32
        esp_err_t result = esp_now_send(gatewayMacAddress, (uint8_t *) &outgoingTelemetry, sizeof(outgoingTelemetry));
        if (result != ESP_OK) {
            Serial.println("[RADIO] ERROR: TX hardware queue failure (ESP32)");
        }
    #elif defined(ESP8266)
        int result = esp_now_send(gatewayMacAddress, (uint8_t *) &outgoingTelemetry, sizeof(outgoingTelemetry));
        if (result != 0) {
            Serial.println("[RADIO] ERROR: TX hardware queue failure (ESP8266)");
        }
    #endif

    // 3. Pacing interval
    delay(5000); 
}