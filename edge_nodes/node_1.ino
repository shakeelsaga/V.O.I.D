// ---------------------------------------------------------
// Edge Telemetry Header - Hardware Agnostic Setup
// ---------------------------------------------------------

// The compiler checks which board you selected in the Arduino IDE
#ifdef ESP32
  #include <WiFi.h>
  #include <esp_now.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <espnow.h>
#else
  #error "Architecture not supported. Please select ESP8266 or ESP32."
#endif

// Placeholder MAC Address for the Gateway (Receiver)
// Standard 6-byte MAC address. Same format for both ESP8266 and ESP32.
// uint8_t gatewayMacAddress[] = X; 

// ---------------------------------------------------------
// Edge Telemetry Payload Structure
// ---------------------------------------------------------
// This struct is identical for both boards because uint8_t is a standard 
// cross-platform data type. It will always be 1 byte, regardless of the CPU.
typedef struct SurvivorPayload {
    uint8_t nodeId;      // Logical ID (e.g., 1, 2, 3)
    uint8_t batteryPct;  // 0-100 (1 byte)
    uint8_t cpuLoad;     // 0-100 (1 byte)
    bool isSosActive;    // Emergency state (1 byte)
} SurvivorPayload;

// Global instance to hold our transmission data
SurvivorPayload outgoingTelemetry;

// ---------------------------------------------------------
// Asynchronous Callback: Triggered when the radio finishes transmitting
// ---------------------------------------------------------
#ifdef ESP32
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    Serial.print("Transmission Status: ");
    // A simple ternary operator to print success or failure
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}
#elif defined(ESP8266)
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
    Serial.print("Transmission Status: ");
    // On the 8266, a status of 0 means success
    Serial.println(sendStatus == 0 ? "Delivery Success" : "Delivery Fail");
}
#endif

void setup() {
    // 1. Initialize the Serial Monitor for local debugging
    // 115200 is the standard stable baud rate for modern ESP boards.
    Serial.begin(115200);
    Serial.println("\n--- Survivor Node Booting ---");

    // 2. Turn on the Radio (Station Mode)
    // WIFI_STA turns the antenna on as a "client", but we don't give it credentials to connect to a router.
    WiFi.mode(WIFI_STA);

    // 3. Initialize the ESP-NOW Protocol
    // esp_now_init() returns 0 (or ESP_OK on ESP32) if the radio hardware starts successfully.
    if (esp_now_init() != 0) {
        Serial.println("Fatal Error: ESP-NOW failed to initialize. Halting.");
        return; // Stop execution. If the radio is dead, the node is dead.
    }
    Serial.println("ESP-NOW Protocol Initialized.");

    esp_now_register_send_cb(OnDataSent);

    // 4. Register the Gateway as a Peer (Hardware Agnostic)
    #ifdef ESP32
        // ESP32 requires us to define a struct with peer parameters
        esp_now_peer_info_t peerInfo;
        
        // Uninitialized memory contains random garbage data which will break the network routing.
        memset(&peerInfo, 0, sizeof(peerInfo)); 
        
        // Copy our placeholder MAC address into the peer info struct
        memcpy(peerInfo.peer_addr, gatewayMacAddress, 6);
        peerInfo.channel = 0;      // Channel 0 defaults to the current Wi-Fi channel
        peerInfo.encrypt = false;  // No encryption for Phase A

        // Register the peer
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("Failed to add Gateway peer (ESP32)");
            return;
        }

    #elif defined(ESP8266)
        // CONTROLLER means this device will be transmitting data.
        esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
        
        // Parameters: MAC Address, Peer Role (Gateway is SLAVE/Receiver), Wi-Fi Channel, Key, Key Length
        if (esp_now_add_peer(gatewayMacAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0) != 0) {
            Serial.println("Failed to add Gateway peer (ESP8266)");
            return;
        }
    #endif
    
    Serial.println("Gateway Peer Registered. Ready to transmit.");
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    // 1. Generate Simulated Telemetry Data
    // We are hardcoding the ID, but using random() to simulate fluctuating hardware metrics.
    outgoingTelemetry.nodeId = 1; 
    outgoingTelemetry.batteryPct = random(10, 100); // Simulate battery draining between 10-100%
    outgoingTelemetry.cpuLoad = random(0, 100);     // Simulate CPU load between 0-100%
    
    // Simulate a disaster SOS randomly (approx. 10% chance of triggering)
    outgoingTelemetry.isSosActive = (random(0, 10) > 8); 

    Serial.println("\n--- Preparing to Transmit ---");
    Serial.print("Node: "); Serial.print(outgoingTelemetry.nodeId);
    Serial.print(" | Battery: "); Serial.print(outgoingTelemetry.batteryPct); Serial.println("%");

    // 2. Transmit the Payload via ESP-NOW
    // We pass: The Gateway MAC, the memory address of our struct, and the exact size of the struct.
    #ifdef ESP32
        esp_err_t result = esp_now_send(gatewayMacAddress, (uint8_t *) &outgoingTelemetry, sizeof(outgoingTelemetry));
        if (result != ESP_OK) {
            Serial.println("Error: Failed to queue data into the radio hardware (ESP32)");
        }
    #elif defined(ESP8266)
        int result = esp_now_send(gatewayMacAddress, (uint8_t *) &outgoingTelemetry, sizeof(outgoingTelemetry));
        if (result != 0) {
            Serial.println("Error: Failed to queue data into the radio hardware (ESP8266)");
        }
    #endif

    // 3. Pace the transmissions
    // A delay of 5 seconds for testing.
    delay(5000); 
}