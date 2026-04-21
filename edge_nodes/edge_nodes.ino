// ---------------------------------------------------------
// V.O.I.D. Edge Node - Survivor Telemetry (V2.1 Broadcast ACK)
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
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myNodeId = 0;
bool gatewayConnected = false;

#ifdef ESP32
const wifi_interface_t EDGE_IF = WIFI_IF_STA;
#endif

// --- TELEMETRY PAYLOADS ---
const uint8_t ACK_MSG_TYPE = 0xA1;
const int ACK_WAIT_MS = 750;
const int TX_BURST_COUNT = 4;

#define EDGE_QUEUE_SIZE 20
typedef struct __attribute__((packed)) SurvivorPayload {
    uint8_t nodeId;
    uint8_t batteryPct;
    uint8_t cpuLoad;
    bool isSosActive;
    uint8_t sequence;
} SurvivorPayload;

typedef struct __attribute__((packed)) AckPayload {
    uint8_t msgType;
    uint8_t nodeId;
    uint8_t sequence;
} AckPayload;

SurvivorPayload edgeQueue[EDGE_QUEUE_SIZE];
int edgeQueueCount = 0;
SurvivorPayload outgoingTelemetry;

// --- ADAPTIVE TELEMETRY STATE ---
uint8_t lastTxBattery = 0;
uint8_t lastTxCpu = 0;
bool lastTxSos = false;
unsigned long lastTxTime = 0;
uint8_t nextSequence = 0;

volatile bool ackReceived = false;
volatile uint8_t awaitedAckSequence = 0;

// ---------------------------------------------------------
// Utility: Derive Node ID & MAC Helpers
// ---------------------------------------------------------
uint8_t deriveNodeId() {
    delay(200);

    String fullMac = WiFi.macAddress();
    Serial.print("[SYS] Raw Hardware MAC: ");
    Serial.println(fullMac);

    if (fullMac.length() >= 17) {
        String lastByteStr = fullMac.substring(15, 17);
        return (uint8_t) strtol(lastByteStr.c_str(), NULL, 16);
    }

    Serial.println("[SYS] ERROR: Hardware MAC malformed.");
    return 0;
}

void parseMacString(String macText, uint8_t* macArray) {
    for (int i = 0; i < 6; i++) {
        String byteString = macText.substring(i * 2, i * 2 + 2);
        macArray[i] = (uint8_t) strtol(byteString.c_str(), NULL, 16);
    }
}

void copyMac(const uint8_t* source, uint8_t* dest) {
    memcpy(dest, source, 6);
}

bool isMacZero(const uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) return false;
    }
    return true;
}

String formatMac(const uint8_t* mac) {
    char macText[13];
    snprintf(macText, sizeof(macText), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macText);
}

// ---------------------------------------------------------
// ESP-NOW Transport
// ---------------------------------------------------------
bool ensureGatewayPeer(int channel) {
    #ifdef ESP32
        if (esp_now_is_peer_exist(gatewayMacAddress)) {
            esp_now_del_peer(gatewayMacAddress);
        }

        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, gatewayMacAddress, 6);
        peerInfo.channel = channel;
        peerInfo.ifidx = EDGE_IF;
        peerInfo.encrypt = true;
        memcpy(peerInfo.lmk, LMK_KEY, 16);

        return esp_now_add_peer(&peerInfo) == ESP_OK;
    #elif defined(ESP8266)
        esp_now_del_peer(gatewayMacAddress);
        return esp_now_add_peer(gatewayMacAddress, ESP_NOW_ROLE_COMBO, channel, (uint8_t *)LMK_KEY, 16) == 0;
    #endif
}

#ifdef ESP32
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
#elif defined(ESP8266)
void OnDataRecv(uint8_t *mac_addr, uint8_t *incomingData, uint8_t len) {
#endif
    if (len == sizeof(AckPayload)) {
        AckPayload ack;
        memcpy(&ack, incomingData, sizeof(AckPayload));
        if (ack.msgType == ACK_MSG_TYPE && ack.nodeId == myNodeId && ack.sequence == awaitedAckSequence) {
            ackReceived = true;
        }
    }
}

bool transmitPayload(const SurvivorPayload& payload, bool logAttempts) {
    awaitedAckSequence = payload.sequence;
    ackReceived = false;

    for (int attempt = 0; attempt < TX_BURST_COUNT; attempt++) {
        #ifdef ESP32
            esp_err_t sendResult = esp_now_send(gatewayMacAddress, (uint8_t *) &payload, sizeof(SurvivorPayload));
            if (sendResult != ESP_OK) {
                Serial.print("[RADIO] Unicast dispatch failed with code ");
                Serial.println((int)sendResult);
                return false;
            }
        #elif defined(ESP8266)
            int sendResult = esp_now_send(gatewayMacAddress, (uint8_t *) &payload, sizeof(SurvivorPayload));
            if (sendResult != 0) {
                Serial.print("[RADIO] Unicast dispatch failed with code ");
                Serial.println(sendResult);
                return false;
            }
        #endif

        unsigned long waitStart = millis();
        while (!ackReceived && (millis() - waitStart) < ACK_WAIT_MS) {
            delay(1);
        }

        if (ackReceived) {
            return true;
        }

        if (logAttempts) {
            Serial.print("[RADIO] No app ACK after burst ");
            Serial.print(attempt + 1);
            Serial.print("/");
            Serial.println(TX_BURST_COUNT);
        }
    }

    return false;
}

// ---------------------------------------------------------
// Gateway Hunt
// ---------------------------------------------------------
bool huntForGateway() {
    Serial.println("\n[SYS] Executing Lighthouse channel sweep...");

    WiFi.disconnect();
    delay(100);
    int networkCount = WiFi.scanNetworks();
    int targetChannel = 0;
    uint8_t discoveredBssid[6] = {0, 0, 0, 0, 0, 0};
    uint8_t discoveredStaMac[6] = {0, 0, 0, 0, 0, 0};

    for (int i = 0; i < networkCount; i++) {
        String foundSSID = WiFi.SSID(i);

        if (foundSSID.startsWith("VOID_")) {
            targetChannel = WiFi.channel(i);
            const uint8_t* bssid = WiFi.BSSID(i);
            if (bssid != nullptr) {
                copyMac(bssid, discoveredBssid);
            }

            String extractedMac = foundSSID.substring(5);
            if (extractedMac.length() >= 12) {
                parseMacString(extractedMac, discoveredStaMac);
            }

            Serial.print("[SYS] Gateway acquired on Channel ");
            Serial.print(targetChannel);
            Serial.print(" | AP MAC: ");
            Serial.print(formatMac(discoveredBssid));
            Serial.print(" | STA MAC: ");
            Serial.println(formatMac(discoveredStaMac));
            break;
        }
    }

    #ifdef ESP32
        WiFi.scanDelete();
    #endif

    if (targetChannel == 0) {
        Serial.println("[SYS] WARNING: Gateway Lighthouse not found.");
        return false;
    }

    #ifdef ESP32
        WiFi.setChannel(targetChannel);
    #elif defined(ESP8266)
        wifi_set_channel(targetChannel);
    #endif

    if (!isMacZero(discoveredBssid)) {
        copyMac(discoveredBssid, gatewayMacAddress);
    } else if (!isMacZero(discoveredStaMac)) {
        copyMac(discoveredStaMac, gatewayMacAddress);
    }

    if (!ensureGatewayPeer(targetChannel)) {
        Serial.println("[SYS] WARNING: Gateway peer setup failed.");
        return false;
    }

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
    #ifdef ESP32
        WiFi.setSleep(false);
    #elif defined(ESP8266)
        WiFi.setSleepMode(WIFI_NONE_SLEEP);
    #endif

    myNodeId = deriveNodeId();
    Serial.print("[SYS] Hardware-Derived Node ID: ");
    Serial.println(myNodeId);

    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: ESP-NOW hardware initialization failed.");
        while (1) delay(1000);
    }

    #ifdef ESP8266
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
        esp_now_set_kok(PMK_KEY, 16);
        esp_now_register_recv_cb(reinterpret_cast<esp_now_recv_cb_t>(OnDataRecv));
    #elif defined(ESP32)
        esp_now_set_pmk(PMK_KEY);
        esp_now_register_recv_cb(OnDataRecv);
    #endif

    while (!huntForGateway()) {
        Serial.println("[SYS] Boot delayed. Retrying hunt in 5s...");
        delay(5000);
    }

    Serial.println("[SYS] Node Armed. Adaptive Telemetry sequence starting.");
}

// ---------------------------------------------------------
// Queue Flush
// ---------------------------------------------------------
void flushEdgeQueue() {
    if (edgeQueueCount == 0) return;
    Serial.print("\n[QUEUE] Flushing ");
    Serial.print(edgeQueueCount);
    Serial.println(" saved payloads to Gateway...");

    int originalCount = edgeQueueCount;
    int sentCount = 0;
    int firstFailedIndex = -1;

    for (int i = 0; i < originalCount; i++) {
        if (!transmitPayload(edgeQueue[i], true)) {
            firstFailedIndex = i;
            gatewayConnected = false;
            Serial.print("[QUEUE] Flush halted at payload ");
            Serial.print(i + 1);
            Serial.print("/");
            Serial.print(originalCount);
            Serial.println(". ACK missing.");
            break;
        }

        sentCount++;
        delay(50);
    }

    if (sentCount == originalCount) {
        edgeQueueCount = 0;
        Serial.println("[QUEUE] Flush complete.");
        return;
    }

    if (firstFailedIndex < 0) firstFailedIndex = 0;

    int unsentCount = originalCount - firstFailedIndex;
    for (int i = 0; i < unsentCount; i++) {
        edgeQueue[i] = edgeQueue[firstFailedIndex + i];
    }
    edgeQueueCount = unsentCount;

    Serial.print("[QUEUE] Preserved ");
    Serial.print(edgeQueueCount);
    Serial.println(" unsent payload(s) in RAM.");
}

// ---------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------
void loop() {
    static float mockBattery = 100.0;
    mockBattery -= 0.05;
    if (mockBattery < 0) mockBattery = 100.0;

    outgoingTelemetry.nodeId = myNodeId;
    outgoingTelemetry.batteryPct = (uint8_t)mockBattery;
    outgoingTelemetry.cpuLoad = random(10, 30);
    outgoingTelemetry.isSosActive = (random(0, 100) > 95);

    bool triggerTransmission = false;
    String triggerReason = "";

    if (outgoingTelemetry.isSosActive) {
        triggerTransmission = true;
        triggerReason = "EMERGENCY SOS BURST";
    } else if (lastTxBattery > outgoingTelemetry.batteryPct + 2) {
        triggerTransmission = true;
        triggerReason = "BATTERY DROP EXCEPTION";
    } else if (abs(lastTxCpu - outgoingTelemetry.cpuLoad) > 30) {
        triggerTransmission = true;
        triggerReason = "CPU SPIKE EXCEPTION";
    } else if (millis() - lastTxTime > 60000) {
        triggerTransmission = true;
        triggerReason = "60s HEARTBEAT";
    }

    if (triggerTransmission) {
        outgoingTelemetry.sequence = nextSequence++;

        Serial.print("\n--- Telemetry Triggered: ");
        Serial.print(triggerReason);
        Serial.println(" ---");

        if (!gatewayConnected) {
            if (edgeQueueCount < EDGE_QUEUE_SIZE) {
                edgeQueue[edgeQueueCount] = outgoingTelemetry;
                edgeQueueCount++;
                Serial.print("[QUEUE] Gateway missing. Payload saved to volatile RAM (");
                Serial.print(edgeQueueCount);
                Serial.println("/20)");
            }

            if (huntForGateway()) {
                flushEdgeQueue();
            }
        } else {
            if (transmitPayload(outgoingTelemetry, true)) {
                Serial.println("[RADIO] Delivery Acknowledged.");
                lastTxTime = millis();
                lastTxBattery = outgoingTelemetry.batteryPct;
                lastTxCpu = outgoingTelemetry.cpuLoad;
                lastTxSos = outgoingTelemetry.isSosActive;
            } else {
                Serial.println("[RADIO] Delivery Failed! Gateway presumed dead.");
                gatewayConnected = false;

                if (edgeQueueCount < EDGE_QUEUE_SIZE) {
                    edgeQueue[edgeQueueCount] = outgoingTelemetry;
                    edgeQueueCount++;
                }
            }
        }
    }

    delay(1000);
}
