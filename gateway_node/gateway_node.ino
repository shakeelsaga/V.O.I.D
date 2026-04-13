// ---------------------------------------------------------
// V.O.I.D. Gateway - HA Auto-Election & DTN Bridge (V2.0)
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

#include <MQTT.h>
WiFiClient espClient;
MQTTClient mqtt(256); 

#include "secrets.h"
#include "node_registry.h"

// --- NETWORK CONFIGURATION ---
const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASS;
const char* mqtt_broker_ip = SECRET_MQTT_BROKER_IP; 

// --- HA PROTOCOL GLOBALS ---
enum GatewayState { STATE_ELECTION, STATE_PRIMARY, STATE_SHADOW };
GatewayState currentState = STATE_ELECTION;

enum HaMessageType { HA_MSG_ELECTION_PING = 0, HA_MSG_HEARTBEAT = 1 };

const bool HA_PREEMPTION_ENABLED = false;
const unsigned long HA_ELECTION_PING_INTERVAL_MS = 500;
const unsigned long HA_HEARTBEAT_INTERVAL_MS = 1000;
const unsigned long HA_HEARTBEAT_TIMEOUT_MS = 6000;
const unsigned long HA_ELECTION_WINDOW_MIN_MS = 3500;
const unsigned long HA_ELECTION_WINDOW_MAX_MS = 4500;
const unsigned long HA_UPTIME_TIE_MARGIN_MS = 500;

uint8_t myMac[6];
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
int meshChannel = 0;

unsigned long stateStartTime = 0;
unsigned long lastHeartbeatTx = 0;
unsigned long lastHeartbeatRx = 0;
unsigned long electionDuration = 0;

typedef struct __attribute__((packed)) HaPayload {
    uint8_t msgType;
    uint8_t mac[6];
    uint32_t senderUptimeMs;
} HaPayload;

// --- EDGE PAYLOAD STRUCTURE ---
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

// --- V2.0 DTN RAM BATCHING & DELTA TRACKING ---
#define RAM_BUFFER_SIZE 50 
#define MAX_NODES 256 

SurvivorPayload ramBuffer[RAM_BUFFER_SIZE];
int ramBufferCount = 0;
unsigned long lastReconnectAttempt = 0;

struct NodeState {
    bool active = false;
    uint8_t lastBattery = 0;
    bool lastSos = false;
};
NodeState networkState[MAX_NODES];

// ---------------------------------------------------------
// Forward Declarations
// ---------------------------------------------------------
void sendHaMessage(uint8_t type);
void demoteToShadow();
void promoteToPrimary();
void startElection();
void reconnectMqtt();
bool shouldYieldToRemotePrimary(const HaPayload& msg);
bool configureEspNow(int channel);
#ifdef ESP32
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len);
#elif defined(ESP8266)
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len);
#endif

// ---------------------------------------------------------
// HA MAC Comparison Utilities
// ---------------------------------------------------------
bool isMacEqual(const uint8_t* mac1, const uint8_t* mac2) {
    for (int i=0; i<6; i++) if (mac1[i] != mac2[i]) return false;
    return true;
}

bool isMacLower(const uint8_t* mac1, const uint8_t* mac2) {
    for (int i=0; i<6; i++) {
        if (mac1[i] < mac2[i]) return true;
        if (mac1[i] > mac2[i]) return false;
    }
    return false;
}

bool shouldYieldToRemotePrimary(const HaPayload& msg) {
    if (HA_PREEMPTION_ENABLED) {
        return isMacLower(msg.mac, myMac);
    }

    // With preemption disabled, prefer the gateway that has been alive longer.
    int32_t uptimeDelta = (int32_t)(msg.senderUptimeMs - millis());
    if (uptimeDelta > (int32_t)HA_UPTIME_TIE_MARGIN_MS) {
        return true;
    }
    if (uptimeDelta < -(int32_t)HA_UPTIME_TIE_MARGIN_MS) {
        return false;
    }

    return isMacLower(msg.mac, myMac);
}

// ---------------------------------------------------------
// HA Protocol Engine
// ---------------------------------------------------------
void sendHaMessage(uint8_t type) {
    HaPayload msg;
    msg.msgType = type;
    memcpy(msg.mac, myMac, 6);
    msg.senderUptimeMs = millis();
    
    #ifdef ESP32
        esp_now_send(broadcastMac, (uint8_t *)&msg, sizeof(HaPayload));
    #elif defined(ESP8266)
        esp_now_send(broadcastMac, (uint8_t *)&msg, sizeof(HaPayload));
    #endif
}

bool configureEspNow(int channel) {
    #ifdef ESP32
        esp_now_deinit();
    #elif defined(ESP8266)
        esp_now_deinit();
    #endif

    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: ESP-NOW initialization failed.");
        return false;
    }

    #ifdef ESP8266
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    #endif
    
    #ifdef ESP32
        esp_now_set_pmk(PMK_KEY);
    #elif defined(ESP8266)
        esp_now_set_kok(PMK_KEY, 16);
    #endif

    // Rebuild peers after every Wi-Fi mode change. ESP-NOW state can be lost
    // when the radio moves between STA and AP+STA.
    #ifdef ESP32
        esp_now_peer_info_t bcastPeer;
        memset(&bcastPeer, 0, sizeof(bcastPeer));
        bcastPeer.channel = channel;
        bcastPeer.encrypt = false;
        memcpy(bcastPeer.peer_addr, broadcastMac, 6);
        esp_now_add_peer(&bcastPeer);

        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        peerInfo.channel = channel;
        peerInfo.encrypt = true;
        memcpy(peerInfo.lmk, LMK_KEY, 16);
        
        for (int i = 0; i < numAuthorizedNodes; i++) {
            memcpy(peerInfo.peer_addr, authorizedEdgeNodes[i], 6);
            esp_now_add_peer(&peerInfo);
        }

        esp_now_register_recv_cb(OnDataRecv);
    #elif defined(ESP8266)
        esp_now_add_peer(broadcastMac, ESP_NOW_ROLE_COMBO, channel, NULL, 0);

        for (int i = 0; i < numAuthorizedNodes; i++) {
            esp_now_add_peer((uint8_t *)authorizedEdgeNodes[i], ESP_NOW_ROLE_SLAVE, channel, (uint8_t *)LMK_KEY, 16);
        }

        esp_now_register_recv_cb(reinterpret_cast<esp_now_recv_cb_t>(OnDataRecv));
    #endif

    Serial.print("[SYS] ESP-NOW Ready on Channel ");
    Serial.println(channel);
    return true;
}

void handleHaMessage(HaPayload msg) {
    if (isMacEqual(msg.mac, myMac)) return; // Ignore own echoes

    if (msg.msgType == HA_MSG_ELECTION_PING) {
        if (currentState == STATE_ELECTION) {
            if (isMacLower(msg.mac, myMac)) {
                demoteToShadow(); // Yield to superior MAC
            }
        }
    } 
    else if (msg.msgType == HA_MSG_HEARTBEAT) {
        if (currentState == STATE_ELECTION || currentState == STATE_SHADOW) {
            lastHeartbeatRx = millis();
            if (currentState == STATE_ELECTION) {
                Serial.println("\n[HA] Active Primary detected. Yielding election.");
                demoteToShadow(); // The "Incumbent Rule" (Preemption Disabled)
            }
        } 
        else if (currentState == STATE_PRIMARY) {
            if (shouldYieldToRemotePrimary(msg)) {
                Serial.println("\n[HA] Split-Brain: Incumbent Primary retained. Demoting.");
                demoteToShadow();
            } else {
                sendHaMessage(HA_MSG_HEARTBEAT); // Assert dominance immediately
            }
        }
    }
}

void startElection() {
    currentState = STATE_ELECTION;
    stateStartTime = millis();
    lastHeartbeatTx = 0;
    lastHeartbeatRx = 0;
    electionDuration = random((long)HA_ELECTION_WINDOW_MIN_MS, (long)HA_ELECTION_WINDOW_MAX_MS + 1);
    
    #ifdef ESP32
        WiFi.softAPdisconnect(true);
    #elif defined(ESP8266)
        WiFi.softAPdisconnect(true);
    #endif
    WiFi.mode(WIFI_STA); // Ensure AP is off during election
    mqtt.disconnect();
    configureEspNow(meshChannel);
    
    Serial.print("\n[HA] Entering ELECTION mode for ");
    Serial.print(electionDuration);
    Serial.println("ms...");
}

void promoteToPrimary() {
    currentState = STATE_PRIMARY;
    Serial.println("\n[HA] *** CROWNED PRIMARY GATEWAY ***");
    
    WiFi.mode(WIFI_AP_STA); // Turn on Lighthouse routing capabilities
    
    String macStr = WiFi.macAddress();
    macStr.replace(":", ""); 
    String lighthouseSSID = "VOID_" + macStr;
    WiFi.softAP(lighthouseSSID.c_str(), "", meshChannel);
    configureEspNow(meshChannel);
    
    Serial.print("[SYS] Lighthouse Beacon Active: ");
    Serial.println(lighthouseSSID);

    sendHaMessage(HA_MSG_HEARTBEAT); // Collapse any late-joiner election immediately
    lastHeartbeatTx = millis();
    
    reconnectMqtt();
}

void demoteToShadow() {
    if (currentState == STATE_PRIMARY) {
        Serial.println("\n[HA] *** DEMOTED TO SHADOW GATEWAY ***");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA); // Terminate Lighthouse immediately
        mqtt.disconnect();
        configureEspNow(meshChannel);
    }
    if (currentState == STATE_ELECTION) {
        Serial.println("[HA] Yielding election to peer gateway.");
    }
    currentState = STATE_SHADOW;
    lastHeartbeatTx = 0;
    lastHeartbeatRx = millis();
}

// ---------------------------------------------------------
// Asynchronous Receiver Callback
// ---------------------------------------------------------
#ifdef ESP32
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
#elif defined(ESP8266)
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
#endif
    // Route incoming data based on struct payload size
    if (len == sizeof(SurvivorPayload)) {
        if (currentState == STATE_PRIMARY) {
            int nextHead = (queueHead + 1) % QUEUE_SIZE;
            if (nextHead != queueTail) { 
                memcpy((void*)&rxQueue[queueHead], incomingData, sizeof(SurvivorPayload));
                queueHead = nextHead;
            }
        }
    } 
    else if (len == sizeof(HaPayload)) {
        HaPayload haMsg;
        memcpy(&haMsg, incomingData, sizeof(HaPayload));
        handleHaMessage(haMsg);
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
    bool flushComplete = true; 
    
    while (file.available()) {
        // [HA FIX]: Starvation Prevention. Maintain dominance during long flushes.
        if (millis() - lastHeartbeatTx > HA_HEARTBEAT_INTERVAL_MS) {
            lastHeartbeatTx = millis();
            sendHaMessage(HA_MSG_HEARTBEAT); 
        }

        String payload = file.readStringUntil('\n');
        payload.trim(); 
        
        if (payload.length() > 0) {
            if (WiFi.status() == WL_CONNECTED && mqtt.publish("void/telemetry", payload.c_str(), false, 1)) {
                Serial.print("[DTN] Flushed: ");
                Serial.println(payload);
                delay(50); 
            } else {
                Serial.println("[DTN] ERROR: Link severed mid-flush. Halting to protect data.");
                flushComplete = false; 
                break; 
            }
        }
    }
    file.close();
    
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
    WiFi.macAddress(myMac);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[SYS] Upstream Wi-Fi Connected.");

    int routerChannel = WiFi.channel();
    Serial.print("[SYS] Upstream Assigned Channel: ");
    Serial.println(routerChannel);

    meshChannel = routerChannel;

    mqtt.begin(mqtt_broker_ip, 1883, espClient);
    mqtt.setOptions(5, true, 2000);

    // Enter Election Phase
    startElection();
}

// ---------------------------------------------------------
// Primary Worker Task
// ---------------------------------------------------------
void processPrimaryTasks() {
    while (queueTail != queueHead) {
        SurvivorPayload currentPayload = rxQueue[queueTail];
        queueTail = (queueTail + 1) % QUEUE_SIZE;
        
        char jsonPayload[128];
        snprintf(jsonPayload, sizeof(jsonPayload), 
                  "{\"node_id\":%d, \"battery\":%d, \"cpu\":%d, \"sos_alert\":%d}", 
                  currentPayload.nodeId, currentPayload.batteryPct, 
                  currentPayload.cpuLoad, currentPayload.isSosActive);

        bool publishSuccess = false;

        if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
            publishSuccess = mqtt.publish("void/telemetry", jsonPayload, false, 1);
            if (publishSuccess) {
                Serial.print("[PUB] Delivery Verified: ");
                Serial.println(jsonPayload);
            } else {
                Serial.println("[SYS] WARNING: Server failed to ACK. Rerouting to DTN.");
            }
        }

        if (!publishSuccess) {
            uint8_t id = currentPayload.nodeId; 
            
            if (id < MAX_NODES) {
                bool criticalChange = false;
                
                if (!networkState[id].active) {
                    criticalChange = true; 
                    networkState[id].active = true;
                } else {
                    if (currentPayload.isSosActive && !networkState[id].lastSos) criticalChange = true;
                    if (networkState[id].lastBattery > currentPayload.batteryPct + 5) criticalChange = true;
                    if (currentPayload.batteryPct > networkState[id].lastBattery + 5) criticalChange = true;
                }
                
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
                    
                    if (ramBufferCount >= RAM_BUFFER_SIZE) flushRamToFlash();
                } else {
                    Serial.print("[DTN] Node ");
                    Serial.print(id);
                    Serial.println(" telemetry redundant. Discarding to protect flash wear.");
                }
            }
        }
    }

    if (!mqtt.connected()) {
        reconnectMqtt();
    } else {
        mqtt.loop();
    }
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    if (currentState == STATE_ELECTION) {
        if (millis() - lastHeartbeatTx > HA_ELECTION_PING_INTERVAL_MS) {
            lastHeartbeatTx = millis();
            sendHaMessage(HA_MSG_ELECTION_PING);
        }
        if (millis() - stateStartTime > electionDuration) {
            promoteToPrimary();
        }
    } 
    else if (currentState == STATE_SHADOW) {
        if (millis() - lastHeartbeatRx > HA_HEARTBEAT_TIMEOUT_MS) {
            Serial.println("\n[HA] Primary Heartbeat Timeout! Initiating failover.");
            startElection();
        }
    } 
    else if (currentState == STATE_PRIMARY) {
        if (millis() - lastHeartbeatTx > HA_HEARTBEAT_INTERVAL_MS) {
            lastHeartbeatTx = millis();
            sendHaMessage(HA_MSG_HEARTBEAT);
        }
        processPrimaryTasks();
    }
}
