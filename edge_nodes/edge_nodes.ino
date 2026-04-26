// -------------------------------------------------------------------------
// V.O.I.D. Edge Node - Survivor Telemetry (V2.2)
// -------------------------------------------------------------------------
// Architecture: ESP32 / ESP8266 Compatible
//
// -------------------------------------------------------------------------

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

#include <WiFiUdp.h>
#include "secrets.h"
#include "node_registry.h"

// --- GLOBALS ---
uint8_t gatewayMacAddress[6] = {0, 0, 0, 0, 0, 0};
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMac[6] = {0, 0, 0, 0, 0, 0};
uint8_t myNodeId = 0;
bool gatewayConnected = false;
int lastKnownChannel = 0;

#ifdef ESP32
const wifi_interface_t EDGE_IF = WIFI_IF_STA;
#endif

// --- REMOTE LOGGING (UDP) ---
WiFiUDP netlogUdp;
bool netlogReady = false;
#define NETLOG_PORT 4210

// --- PREFERRED PEER CACHE (RAM-only, cleared on reboot) ---
uint8_t preferredPeer[6] = {0, 0, 0, 0, 0, 0};
bool    hasPreferredPeer  = false;

// --- TIMING ---
const int ACK_WAIT_MS    = 750;
const int TX_BURST_COUNT = 4;
#define MESH_TOTAL_TIMEOUT_MS  10000
#define MESH_PER_PEER_WAIT_MS    400
const int MESH_CHANNELS[]    = {11, 6, 1};
const int MESH_CHANNEL_COUNT = 3;

// Duplicate suppression — 16-entry circular cache of (nodeId, seq) pairs
#define DUP_CACHE_SIZE 16
struct DupEntry { uint8_t nodeId; uint8_t seq; };
DupEntry dupCache[DUP_CACHE_SIZE];
int dupCacheIdx = 0;

bool isDuplicate(uint8_t nodeId, uint8_t seq) {
    for (int i = 0; i < DUP_CACHE_SIZE; i++) {
        if (dupCache[i].nodeId == nodeId && dupCache[i].seq == seq) return true;
    }
    return false;
}
void recordSeen(uint8_t nodeId, uint8_t seq) {
    dupCache[dupCacheIdx % DUP_CACHE_SIZE] = {nodeId, seq};
    dupCacheIdx++;
}

// --- TELEMETRY PAYLOADS ---
const uint8_t ACK_MSG_TYPE = 0xA1;

#define EDGE_QUEUE_SIZE 20
typedef struct __attribute__((packed)) SurvivorPayload {
    uint8_t  nodeId;       // offset 0
    uint8_t  batteryPct;   // offset 1
    uint8_t  cpuLoad;      // offset 2
    bool     isSosActive;  // offset 3
    uint32_t uptimeMs;     // offset 4  (4-byte aligned — must match gateway_node.ino)
    uint8_t  sequence;     // offset 8
    uint8_t  ttl;          // offset 9
} SurvivorPayload;

typedef struct __attribute__((packed)) AckPayload {
    uint8_t msgType;
    uint8_t nodeId;
    uint8_t sequence;
} AckPayload;

// --- DEFERRED REPEATER PROCESSING ---
// On ESP32, protect the multi-field buffer with a critical section
// to avoid the dual-core race where the flag is set before both memcpys complete.
#ifdef ESP32
portMUX_TYPE repeatMux = portMUX_INITIALIZER_UNLOCKED;
#endif

volatile bool pendingRepeat      = false;
uint8_t       pendingRepeatSenderMac[6];
SurvivorPayload pendingRepeatPayload;

SurvivorPayload edgeQueue[EDGE_QUEUE_SIZE];
int edgeQueueCount = 0;
SurvivorPayload outgoingTelemetry;

// --- ADAPTIVE TELEMETRY STATE ---
uint8_t lastTxBattery = 0;
uint8_t lastTxCpu = 0;
bool    lastTxSos = false;
unsigned long lastTxTime = 0;
uint8_t nextSequence = 0;

volatile bool    ackReceived        = false;
volatile uint8_t awaitedAckSequence = 0;
volatile uint8_t awaitedAckNode     = 0;

// -------------------------------------------------------------------------
// Utility: MAC Helpers
// -------------------------------------------------------------------------
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

void copyMac(const uint8_t* source, uint8_t* dest) { memcpy(dest, source, 6); }

bool isMacZero(const uint8_t* mac) {
    for (int i = 0; i < 6; i++) if (mac[i] != 0) return false;
    return true;
}

bool isMacEqual(const uint8_t* mac1, const uint8_t* mac2) {
    for (int i = 0; i < 6; i++) if (mac1[i] != mac2[i]) return false;
    return true;
}

String formatMac(const uint8_t* mac) {
    char macText[13];
    snprintf(macText, sizeof(macText), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macText);
}

// -------------------------------------------------------------------------
// Forward Declarations
// -------------------------------------------------------------------------
bool transmitPayload(const SurvivorPayload& payload, const uint8_t* targetMac, bool logAttempts);
bool huntForPeers(const SurvivorPayload& payload);
bool routePayload(const SurvivorPayload& payload);
void processRepeatPayload();
bool huntForGateway();
void connectHomeWifi();
void netlogln(const String& msg);
void netlog(const String& msg);
void flushEdgeQueue();

// -------------------------------------------------------------------------
// ESP-NOW Transport
// -------------------------------------------------------------------------
bool ensureGatewayPeer(int channel) {
    #ifdef ESP32
        if (esp_now_is_peer_exist(gatewayMacAddress)) {
            esp_now_del_peer(gatewayMacAddress);
        }
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, gatewayMacAddress, 6);
        peerInfo.channel = channel;
        peerInfo.ifidx   = EDGE_IF;
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
    // KEEP THIS CALLBACK MINIMAL.
    // Allowed: memcpy, flag set, simple arithmetic.
    // Forbidden: String(), Serial.print(), delay(), esp_now_send(),
    //            any function that allocates heap or blocks.

    if (len == sizeof(AckPayload)) {
        AckPayload ack;
        memcpy(&ack, incomingData, sizeof(AckPayload));
        if (ack.msgType == ACK_MSG_TYPE &&
            ack.nodeId  == awaitedAckNode &&
            ack.sequence == awaitedAckSequence) {
            ackReceived = true;
        }
    } else if (len == sizeof(SurvivorPayload) && !pendingRepeat) {
        // Protect buffer + flag atomically on ESP32
        #ifdef ESP32
            portENTER_CRITICAL(&repeatMux);
        #endif
        memcpy((void*)&pendingRepeatPayload, incomingData, sizeof(SurvivorPayload));
        #ifdef ESP32
            memcpy(pendingRepeatSenderMac, info->src_addr, 6);
        #elif defined(ESP8266)
            memcpy(pendingRepeatSenderMac, mac_addr, 6);
        #endif
        pendingRepeat = true;
        #ifdef ESP32
            portEXIT_CRITICAL(&repeatMux);
        #endif
    }
}

// Called from loop() — safe to use String, Serial, delay, esp_now_send here.
void processRepeatPayload() {
    if (!pendingRepeat) return;

    // Snapshot and clear the flag atomically
    SurvivorPayload fwdPayload;
    uint8_t senderMac[6];
    #ifdef ESP32
        portENTER_CRITICAL(&repeatMux);
    #endif
    memcpy(&fwdPayload, (void*)&pendingRepeatPayload, sizeof(SurvivorPayload));
    memcpy(senderMac, pendingRepeatSenderMac, 6);
    pendingRepeat = false;
    #ifdef ESP32
        portEXIT_CRITICAL(&repeatMux);
    #endif

    // TTL check BEFORE sending ACK (was after — wrong order)
    if (fwdPayload.ttl == 0 || fwdPayload.nodeId == myNodeId) return;

    // Duplicate suppression — drop already-seen (nodeId, seq) pairs
    if (isDuplicate(fwdPayload.nodeId, fwdPayload.sequence)) {
        netlogln("[REPEATER] Duplicate suppressed: Node " + String(fwdPayload.nodeId) +
                 " seq=" + String(fwdPayload.sequence));
        return;
    }
    recordSeen(fwdPayload.nodeId, fwdPayload.sequence);
    fwdPayload.ttl--;

    // Send ACK back to the upstream sender so it stops retrying
    AckPayload edgeAck;
    edgeAck.msgType  = ACK_MSG_TYPE;
    edgeAck.nodeId   = fwdPayload.nodeId;
    edgeAck.sequence = fwdPayload.sequence;
    #ifdef ESP32
        if (!esp_now_is_peer_exist(senderMac)) {
            esp_now_peer_info_t tmpPeer;
            memset(&tmpPeer, 0, sizeof(tmpPeer));
            memcpy(tmpPeer.peer_addr, senderMac, 6);
            tmpPeer.channel = 0;
            tmpPeer.ifidx   = EDGE_IF;
            tmpPeer.encrypt = false;
            esp_now_add_peer(&tmpPeer);
        }
        esp_now_send(senderMac, (uint8_t *)&edgeAck, sizeof(AckPayload));
    #elif defined(ESP8266)
        esp_now_send(senderMac, (uint8_t *)&edgeAck, sizeof(AckPayload));
    #endif

    // Forward toward gateway or save to queue — NO connectHomeWifi() here
    if (fwdPayload.isSosActive) {
        netlogln("[REPEATER] Priority SOS from Node " + String(fwdPayload.nodeId) + " — forwarding");
        bool forwarded = false;
        if (gatewayConnected) {
            uint8_t savedSeq  = awaitedAckSequence;
            uint8_t savedNode = awaitedAckNode;
            bool    savedAck  = ackReceived;
            forwarded = transmitPayload(fwdPayload, gatewayMacAddress, true);
            awaitedAckSequence = savedSeq;
            awaitedAckNode     = savedNode;
            ackReceived        = savedAck;
        }
        if (!forwarded) {
            // Enqueue for the next flushEdgeQueue() in loop()
            if (edgeQueueCount < EDGE_QUEUE_SIZE) {
                edgeQueue[edgeQueueCount++] = fwdPayload;
            }
            netlogln("[REPEATER] GW unavailable. SOS queued for retry.");
        }
    } else {
        netlogln("[REPEATER] Normal payload from Node " + String(fwdPayload.nodeId) + " queued");
        if (edgeQueueCount < EDGE_QUEUE_SIZE) {
            edgeQueue[edgeQueueCount++] = fwdPayload;
        }
    }
}

bool transmitPayload(const SurvivorPayload& payload, const uint8_t* targetMac, bool logAttempts) {
    awaitedAckSequence = payload.sequence;
    awaitedAckNode     = payload.nodeId;
    ackReceived = false;

    for (int attempt = 0; attempt < TX_BURST_COUNT; attempt++) {
        #ifdef ESP32
            esp_err_t sendResult = esp_now_send(targetMac, (uint8_t *) &payload, sizeof(SurvivorPayload));
            if (sendResult != ESP_OK) {
                Serial.print("[RADIO] Unicast dispatch failed code="); Serial.println((int)sendResult);
                return false;
            }
        #elif defined(ESP8266)
            int sendResult = esp_now_send((uint8_t*)targetMac, (uint8_t *) &payload, sizeof(SurvivorPayload));
            if (sendResult != 0) {
                Serial.print("[RADIO] Unicast dispatch failed code="); Serial.println(sendResult);
                return false;
            }
        #endif

        unsigned long waitStart = millis();
        while (!ackReceived && (millis() - waitStart) < ACK_WAIT_MS) {
            yield(); // yield() instead of delay(1) — feeds ESP8266 WDT and allows callbacks
        }

        if (ackReceived) return true;

        if (logAttempts) {
            Serial.print("[RADIO] No ACK after burst ");
            Serial.print(attempt + 1); Serial.print("/"); Serial.println(TX_BURST_COUNT);
        }
    }
    return false;
}

// -------------------------------------------------------------------------
// Mesh Routing & Discovery
// -------------------------------------------------------------------------
void registerEdgePeers(int channel) {
    for (int i = 0; i < numAuthorizedNodes; i++) {
        if (isMacEqual(authorizedEdgeNodes[i], myMac)) continue;
        #ifdef ESP32
            if (esp_now_is_peer_exist(authorizedEdgeNodes[i])) {
                esp_now_del_peer(authorizedEdgeNodes[i]);
            }
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, authorizedEdgeNodes[i], 6);
            peerInfo.channel = channel;
            peerInfo.ifidx = EDGE_IF;
            peerInfo.encrypt = true;
            memcpy(peerInfo.lmk, LMK_KEY, 16);
            esp_now_add_peer(&peerInfo);
        #elif defined(ESP8266)
            esp_now_del_peer((uint8_t *)authorizedEdgeNodes[i]);
            esp_now_add_peer((uint8_t *)authorizedEdgeNodes[i], ESP_NOW_ROLE_COMBO, channel, (uint8_t *)LMK_KEY, 16);
        #endif
    }
}

bool routePayload(const SurvivorPayload& payload) {
    if (gatewayConnected) {
        if (transmitPayload(payload, gatewayMacAddress, true)) return true;
        gatewayConnected = false;
        netlogln("[RADIO] Gateway lost. Escalating to mesh.");
    }
    return huntForPeers(payload);
}

// huntForPeers — cooperative non-blocking design:
//  - fast path via cached preferred peer
//  - channel sweep (each probe is bounded to MESH_PER_PEER_WAIT_MS)
//  - extended retry on lastKnownChannel with yield() in inner loop

//
// awaitedAckNode is now correctly set in all send paths.
bool huntForPeers(const SurvivorPayload& payload) {

    // Try preferred peer cache first (fast path)
    if (hasPreferredPeer) {
        int meshCh = (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
        registerEdgePeers(meshCh);
        netlogln("[MESH] Trying cached peer " + formatMac(preferredPeer));
        // Set awaitedAckNode before transmit
        if (transmitPayload(payload, preferredPeer, false)) {
            netlogln("[MESH] Cached peer ACK'd.");
            return true;
        }
        hasPreferredPeer = false;
        netlogln("[MESH] Cached peer miss. Full channel sweep starting.");
    }

    // Multi-channel sweep
    netlogln("[MESH] Channel sweep across " + String(MESH_CHANNEL_COUNT) + " channels...");
    for (int ci = 0; ci < MESH_CHANNEL_COUNT; ci++) {
        int ch = MESH_CHANNELS[ci];
        registerEdgePeers(ch);
        for (int i = 0; i < numAuthorizedNodes; i++) {
            if (isMacEqual(authorizedEdgeNodes[i], myMac)) continue;

            // Set awaitedAckNode BEFORE sending so OnDataRecv can match the ACK
            awaitedAckSequence = payload.sequence;
            awaitedAckNode     = payload.nodeId;  
            ackReceived = false;

            #ifdef ESP32
                esp_now_send(authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #elif defined(ESP8266)
                esp_now_send((uint8_t*)authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #endif

            unsigned long t = millis();
            while (!ackReceived && millis() - t < MESH_PER_PEER_WAIT_MS) yield(); // if (ackReceived) {
                lastKnownChannel = ch;
                memcpy(preferredPeer, authorizedEdgeNodes[i], 6);
                hasPreferredPeer = true;
                netlogln("[MESH] Peer found on Ch " + String(ch) + " -> " + formatMac(preferredPeer));
                return true;
            }
        }
    }

    // Extended retry on lastKnownChannel (10s window) — with cooperative yielding
    int meshCh = (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
    registerEdgePeers(meshCh);
    netlogln("[MESH] Extended retry on Ch " + String(meshCh) + " (10s)...");
    unsigned long deadline = millis() + MESH_TOTAL_TIMEOUT_MS;
    while (millis() < deadline) {
        processRepeatPayload(); // Service repeater buffer during long retry
        for (int i = 0; i < numAuthorizedNodes; i++) {
            if (isMacEqual(authorizedEdgeNodes[i], myMac)) continue;

            // Set awaitedAckNode every iteration
            awaitedAckSequence = payload.sequence;
            awaitedAckNode     = payload.nodeId;  
            ackReceived = false;

            #ifdef ESP32
                esp_now_send(authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #elif defined(ESP8266)
                esp_now_send((uint8_t*)authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #endif

            unsigned long t = millis();
            while (!ackReceived && millis() - t < MESH_PER_PEER_WAIT_MS) yield(); // if (ackReceived) {
                memcpy(preferredPeer, authorizedEdgeNodes[i], 6);
                hasPreferredPeer = true;
                netlogln("[MESH] Extended retry ACK from " + formatMac(preferredPeer));
                return true;
            }
            if (millis() >= deadline) break;
        }
    }

    Serial.println("[MESH] All peers unreachable.");
    netlogln("[MESH] All peers unreachable after full sweep.");
    return false;
}

// -------------------------------------------------------------------------
// Gateway Hunt
// -------------------------------------------------------------------------
bool huntForGateway() {
    Serial.println("\n[SYS] Executing Lighthouse channel sweep...");
    netlogReady = false;
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
            if (bssid != nullptr) copyMac(bssid, discoveredBssid);
            String extractedMac = foundSSID.substring(5);
            if (extractedMac.length() >= 12) parseMacString(extractedMac, discoveredStaMac);
            Serial.print("[SYS] Gateway acquired on Channel "); Serial.print(targetChannel);
            Serial.print(" | AP MAC: "); Serial.print(formatMac(discoveredBssid));
            Serial.print(" | STA MAC: "); Serial.println(formatMac(discoveredStaMac));
            break;
        }
    }
    #ifdef ESP32
        WiFi.scanDelete();
    #endif

    if (targetChannel == 0) {
        targetChannel = (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
        Serial.print("[SYS] WARNING: Gateway Lighthouse not found. Falling back to Mesh Channel ");
        Serial.println(targetChannel);
        #ifdef ESP32
            WiFi.setChannel(targetChannel);
        #elif defined(ESP8266)
            wifi_set_channel(targetChannel);
        #endif
        registerEdgePeers(targetChannel);
        return false;
    }

    lastKnownChannel = targetChannel;
    #ifdef ESP32
        WiFi.setChannel(targetChannel);
    #elif defined(ESP8266)
        wifi_set_channel(targetChannel);
    #endif

    if (!isMacZero(discoveredBssid)) copyMac(discoveredBssid, gatewayMacAddress);
    else if (!isMacZero(discoveredStaMac)) copyMac(discoveredStaMac, gatewayMacAddress);

    if (!ensureGatewayPeer(targetChannel)) {
        Serial.println("[SYS] WARNING: Gateway peer setup failed.");
        return false;
    }
    registerEdgePeers(targetChannel);
    gatewayConnected = true;
    return true;
}

// -------------------------------------------------------------------------
// Remote UDP Logger
// -------------------------------------------------------------------------
void netlog(const String& msg) {
    Serial.print(msg);
    if (netlogReady) {
        String packet = "[NODE_" + String(myNodeId) + "] " + msg;
        netlogUdp.beginPacket("255.255.255.255", NETLOG_PORT);
        netlogUdp.print(packet);
        netlogUdp.endPacket();
    }
}

void netlogln(const String& msg) { netlog(msg + "\n"); }

// Verify channel before WiFi.begin to avoid disrupting ESP-NOW
void connectHomeWifi() {
    if (netlogReady) return;

    // Sanity check: ESP-NOW channel must match router channel.
    // If lastKnownChannel is set and the router SSID is on a different channel,
    // we skip STA join to protect ESP-NOW operation.
    delay(200);
    WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(200);
        tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        // Verify channel hasn't shifted
        int connectedChannel = WiFi.channel();
        if (lastKnownChannel != 0 && connectedChannel != lastKnownChannel) {
            Serial.print("[NETLOG] WARNING: Router channel ");
            Serial.print(connectedChannel);
            Serial.print(" != ESP-NOW channel ");
            Serial.print(lastKnownChannel);
            Serial.println(". ESP-NOW peers may be unreachable. Re-registering peers.");
            lastKnownChannel = connectedChannel;
            registerEdgePeers(lastKnownChannel);
            ensureGatewayPeer(lastKnownChannel);
        }
        netlogUdp.begin(NETLOG_PORT);
        netlogReady = true;
        Serial.print("[NETLOG] UDP logger active. IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.print("[NETLOG] Home WiFi not reachable (status=");
        Serial.print(WiFi.status());
        Serial.println("). USB serial only.");
    }
}

// -------------------------------------------------------------------------
// System Initialization
// -------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- V.O.I.D. Edge Node Booting ---");

    memset(dupCache, 0, sizeof(dupCache));

    WiFi.mode(WIFI_STA);
    #ifdef ESP32
        WiFi.setSleep(false);
    #elif defined(ESP8266)
        WiFi.setSleepMode(WIFI_NONE_SLEEP);
    #endif

    WiFi.macAddress(myMac);
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

    huntForGateway();
    connectHomeWifi();

    if (!gatewayConnected) {
        netlogln("[SYS] No gateway at boot. Probing mesh peers immediately.");
        registerEdgePeers((lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL);
        SurvivorPayload bootProbe;
        bootProbe.nodeId       = myNodeId;
        bootProbe.batteryPct   = 100;
        bootProbe.cpuLoad      = 0;
        bootProbe.isSosActive  = true;
        bootProbe.sequence     = 0;
        bootProbe.ttl          = 2;
        bootProbe.uptimeMs     = millis();
        if (huntForPeers(bootProbe)) {
            netlogln("[SYS] Boot probe ACK'd by peer — mesh path active.");
        } else {
            netlogln("[SYS] No peers at boot. Will retry on first telemetry trigger.");
        }
    }

    Serial.println("[SYS] Node Armed. Adaptive Telemetry sequence starting.");
    netlogln("[SYS] Node " + String(myNodeId) + " armed. UDP log stream active.");
}

// -------------------------------------------------------------------------
// Queue Flush
// -------------------------------------------------------------------------
void flushEdgeQueue() {
    if (edgeQueueCount == 0) return;
    Serial.print("\n[QUEUE] Flushing ");
    Serial.print(edgeQueueCount);
    Serial.println(" saved payloads to Gateway...");

    int originalCount = edgeQueueCount;
    int sentCount = 0;
    int firstFailedIndex = -1;

    for (int i = 0; i < originalCount; i++) {
        processRepeatPayload(); // Service repeater buffer during flush
        if (!routePayload(edgeQueue[i])) {
            firstFailedIndex = i;
            gatewayConnected = false;
            Serial.print("[QUEUE] Flush halted at payload ");
            Serial.print(i + 1); Serial.print("/"); Serial.print(originalCount);
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
    Serial.print("[QUEUE] Preserved "); Serial.print(edgeQueueCount); Serial.println(" unsent payload(s) in RAM.");
}

// -------------------------------------------------------------------------
// Main Execution Loop
// -------------------------------------------------------------------------
void loop() {
    // Always service the repeater buffer FIRST, unconditionally
    processRepeatPayload();

    // Also flush relay-queued packets during idle time
    // This ensures packets from OTHER nodes that this node relayed
    // are forwarded even when this node has no outgoing telemetry to send.
    if (edgeQueueCount > 0 && gatewayConnected) {
        flushEdgeQueue();
    }

    static float mockBattery = 100.0;
    mockBattery -= 0.05;
    if (mockBattery < 0) mockBattery = 100.0;

    outgoingTelemetry.nodeId      = myNodeId;
    outgoingTelemetry.batteryPct  = (uint8_t)mockBattery;
    outgoingTelemetry.cpuLoad     = random(10, 30);
    outgoingTelemetry.isSosActive = (random(0, 100) > 95);
    outgoingTelemetry.ttl         = 3;
    outgoingTelemetry.uptimeMs    = millis();

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
        outgoingTelemetry.sequence  = nextSequence++;
        outgoingTelemetry.uptimeMs  = millis();

        netlogln("\n--- Telemetry Triggered: " + triggerReason + " ---");

        bool sent = routePayload(outgoingTelemetry);
        if (sent) {
            netlogln("[RADIO] ACK seq=" + String(outgoingTelemetry.sequence) +
                     " bat=" + String(outgoingTelemetry.batteryPct) + "%");
            lastTxTime    = millis();
            lastTxBattery = outgoingTelemetry.batteryPct;
            lastTxCpu     = outgoingTelemetry.cpuLoad;
            lastTxSos     = outgoingTelemetry.isSosActive;

            // On successful TX, flush any queued payloads immediately
            flushEdgeQueue();
        } else {
            netlogln("[RADIO] Delivery FAILED seq=" + String(outgoingTelemetry.sequence));
            gatewayConnected = false;

            // Try gateway rediscovery
            if (huntForGateway()) {
                connectHomeWifi();
                flushEdgeQueue();
            } else {
                connectHomeWifi();
                if (edgeQueueCount < EDGE_QUEUE_SIZE) {
                    edgeQueue[edgeQueueCount++] = outgoingTelemetry;
                    netlogln("[QUEUE] All paths failed. Saved to RAM (" +
                             String(edgeQueueCount) + "/20)");
                }
                lastTxTime    = millis();
                lastTxBattery = outgoingTelemetry.batteryPct;
                lastTxCpu     = outgoingTelemetry.cpuLoad;
                lastTxSos     = outgoingTelemetry.isSosActive;
            }
        }
    }

    yield(); // Feed WDT during idle
    delay(100); // Reduced from 1000ms — makes the loop more responsive
}
