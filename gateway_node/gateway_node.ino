// ---------------------------------------------------------
// Edge Telemetry Gateway - Receiver Logic
// ---------------------------------------------------------

#ifdef ESP32
  #include <WiFi.h>
  #include <esp_now.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <espnow.h>
#else
  #error "Architecture not supported."
#endif

// ---------------------------------------------------------
// Edge Telemetry Payload Structure
// ---------------------------------------------------------
typedef struct SurvivorPayload {
    uint8_t nodeId;      
    uint8_t batteryPct;  
    uint8_t cpuLoad;     
    bool isSosActive;    
} SurvivorPayload;

// Create a variable to hold the incoming data
SurvivorPayload incomingTelemetry;

// ---------------------------------------------------------
// Asynchronous Callback: Triggered when the radio receives a packet
// ---------------------------------------------------------
#ifdef ESP32
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    // 1. Copy the raw incoming bytes into our structured variable
    memcpy(&incomingTelemetry, incomingData, sizeof(incomingTelemetry));
    
    // 2. Print the decoded data to the Serial Monitor
    Serial.println("\n--- INCOMING TELEMETRY ---");
    Serial.print("Bytes received: "); Serial.println(len);
    Serial.print("Node ID: "); Serial.println(incomingTelemetry.nodeId);
    Serial.print("Battery: "); Serial.print(incomingTelemetry.batteryPct); Serial.println("%");
    Serial.print("CPU Load: "); Serial.print(incomingTelemetry.cpuLoad); Serial.println("%");
    
    if (incomingTelemetry.isSosActive) {
        Serial.println("STATUS: [CRITICAL] SOS ALERT TRIGGERED!");
    } else {
        Serial.println("STATUS: [NOMINAL]");
    }
}
#elif defined(ESP8266)
void OnDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
    memcpy(&incomingTelemetry, incomingData, sizeof(incomingTelemetry));
    // (Print statements omitted for brevity, same as ESP32 above)
    Serial.print("\n--- INCOMING TELEMETRY from Node "); 
    Serial.print(incomingTelemetry.nodeId); Serial.println(" ---");
}
#endif

// ---------------------------------------------------------
// Main Setup and Loop
// ---------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Edge Gateway Booting ---");

    // 1. Turn on the Radio (Station Mode)
    WiFi.mode(WIFI_STA);

    // 2. Initialize ESP-NOW
    if (esp_now_init() != 0) {
        Serial.println("Fatal Error: ESP-NOW failed to initialize. Halting.");
        return;
    }
    Serial.println("ESP-NOW Protocol Initialized.");

    // 3. Register the Receive Callback Function
    #ifdef ESP32
        esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
    #elif defined(ESP8266)
        esp_now_set_self_role(ESP_NOW_ROLE_SLAVE); // Gateway acts as receiver
        esp_now_register_recv_cb(OnDataRecv);
    #endif
    
    Serial.println("Listening for Survivor Nodes...");
}

void loop() {
  
}
