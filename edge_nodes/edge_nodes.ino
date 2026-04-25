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

#ifdef ESP32
#include <WiFiUdp.h>
#elif defined(ESP8266)
#include <WiFiUdp.h>
#endif

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

// --- DEFERRED REPEATER PROCESSING (flag only; buffer declared after struct) ---
// OnDataRecv on ESP8266 runs in the SDK WiFi context with a very small
// stack. Calling String/Serial/delay/esp_now_send from within it causes
// a stack overflow (Exception 9). We buffer the payload and process it
// safely in loop() via processRepeatPayload().
volatile bool pendingRepeat     = false;
uint8_t       pendingRepeatSenderMac[6];
// SurvivorPayload pendingRepeatPayload is declared after the struct below.
// --- TELEMETRY PAYLOADS ---
const uint8_t ACK_MSG_TYPE = 0xA1;
const int ACK_WAIT_MS      = 750;   // ms to wait for ACK per burst (gateway path)
const int TX_BURST_COUNT   = 4;     // burst retries on gateway path

// Mesh-path timing: 10s total across all peers, 400ms wait per peer per round
#define MESH_TOTAL_TIMEOUT_MS  10000
#define MESH_PER_PEER_WAIT_MS    400

// Channel sweep: priority common channels only (11→6→1).
// Scanning all 13 channels is too slow for field conditions.
const int MESH_CHANNELS[]   = {11, 6, 1};
const int MESH_CHANNEL_COUNT = 3;

#define EDGE_QUEUE_SIZE 20
typedef struct __attribute__((packed)) SurvivorPayload {
    uint8_t  nodeId;      // offset 0
    uint8_t  batteryPct;  // offset 1
    uint8_t  cpuLoad;     // offset 2
    bool     isSosActive; // offset 3
    uint32_t uptimeMs;    // offset 4  ← MUST stay at offset 4 for 4-byte alignment
                          //              on ESP8266 (packed struct, stack is 4-byte
                          //              aligned so offset 4 is safe; offset 6 was not)
    uint8_t  sequence;    // offset 8
    uint8_t  ttl;         // offset 9
} SurvivorPayload;

// Buffer for deferred repeater processing (needs SurvivorPayload defined first)
SurvivorPayload pendingRepeatPayload;


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
volatile uint8_t awaitedAckNode     = 0; // nodeId whose ACK we are waiting for
                                          // (may differ from myNodeId when repeating)

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

bool isMacEqual(const uint8_t* mac1, const uint8_t* mac2) {
    for (int i = 0; i < 6; i++) {
        if (mac1[i] != mac2[i]) return false;
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
// Forward Declarations
// ---------------------------------------------------------
bool transmitPayload(const SurvivorPayload& payload, const uint8_t* targetMac, bool logAttempts);
bool huntForPeers(const SurvivorPayload& payload);
bool routePayload(const SurvivorPayload& payload);
void processRepeatPayload();

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
    // ---------------------------------------------------------------
    // KEEP THIS CALLBACK MINIMAL — especially on ESP8266 where it runs
    // in the SDK WiFi context with very limited stack space.
    // Allowed: memcpy, flag set, simple arithmetic.
    // Forbidden: String(), Serial.print(), delay(), esp_now_send(),
    //            any function that allocates heap or blocks.
    // ---------------------------------------------------------------
    if (len == sizeof(AckPayload)) {
        AckPayload ack;
        memcpy(&ack, incomingData, sizeof(AckPayload));
        // Use awaitedAckNode (not hardcoded myNodeId) so the repeater
        // correctly recognises ACKs for forwarded packets from other nodes.
        if (ack.msgType == ACK_MSG_TYPE && ack.nodeId == awaitedAckNode && ack.sequence == awaitedAckSequence) {
            ackReceived = true;
        }
    } else if (len == sizeof(SurvivorPayload) && !pendingRepeat) {
        // Buffer the incoming payload; processRepeatPayload() in loop() handles the rest.
        memcpy((void*)&pendingRepeatPayload, incomingData, sizeof(SurvivorPayload));
        #ifdef ESP32
            memcpy(pendingRepeatSenderMac, info->src_addr, 6);
        #elif defined(ESP8266)
            memcpy(pendingRepeatSenderMac, mac_addr, 6);
        #endif
        pendingRepeat = true;
    }
}

// Called from loop() — safe to use String, Serial, delay, esp_now_send here.
void processRepeatPayload() {
    if (!pendingRepeat) return;

    // Snapshot and clear the flag first so new packets aren't blocked
    SurvivorPayload fwdPayload;
    uint8_t senderMac[6];
    memcpy(&fwdPayload, (void*)&pendingRepeatPayload, sizeof(SurvivorPayload));
    memcpy(senderMac, pendingRepeatSenderMac, 6);
    pendingRepeat = false;

    if (fwdPayload.ttl == 0 || fwdPayload.nodeId == myNodeId) return;
    fwdPayload.ttl--;

    // Send ACK back to the original sender so it stops retrying
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

    if (fwdPayload.isSosActive) {
        netlogln("[REPEATER] Priority SOS from Node " + String(fwdPayload.nodeId) + " — forwarding");
        uint8_t savedSeq  = awaitedAckSequence;
        uint8_t savedNode = awaitedAckNode;
        bool    savedAck  = ackReceived;
        bool forwarded    = false;
        if (gatewayConnected) {
            forwarded = transmitPayload(fwdPayload, gatewayMacAddress, true);
        }
        if (!forwarded) {
            // Gateway direct send failed — try re-discovering the gateway.
            // Do NOT call huntForPeers() here: that sweeps for edge peers
            // and is the wrong fallback for a repeater that needs the gateway.
            netlogln("[REPEATER] Direct GW failed. Hunting gateway...");
            if (huntForGateway()) {
                connectHomeWifi();
                forwarded = transmitPayload(fwdPayload, gatewayMacAddress, true);
            }
        }
        if (!forwarded) netlogln("[REPEATER] Forward failed — no path to gateway.");
        awaitedAckSequence = savedSeq;
        awaitedAckNode     = savedNode;
        ackReceived        = savedAck;
    } else {
        netlogln("[REPEATER] Normal payload from Node " + String(fwdPayload.nodeId) + " queued");
        if (edgeQueueCount < EDGE_QUEUE_SIZE) {
            edgeQueue[edgeQueueCount++] = fwdPayload;
        }
    }
}

bool transmitPayload(const SurvivorPayload& payload, const uint8_t* targetMac, bool logAttempts) {
    awaitedAckSequence = payload.sequence;
    awaitedAckNode     = payload.nodeId; // could be myNodeId or a relayed node's ID
    ackReceived = false;

    for (int attempt = 0; attempt < TX_BURST_COUNT; attempt++) {
        #ifdef ESP32
            esp_err_t sendResult = esp_now_send(targetMac, (uint8_t *) &payload, sizeof(SurvivorPayload));
            if (sendResult != ESP_OK) {
                Serial.print("[RADIO] Unicast dispatch failed with code ");
                Serial.println((int)sendResult);
                return false;
            }
        #elif defined(ESP8266)
            int sendResult = esp_now_send((uint8_t*)targetMac, (uint8_t *) &payload, sizeof(SurvivorPayload));
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
// Mesh Routing & Discovery
// ---------------------------------------------------------
void registerEdgePeers(int channel) {
    for (int i = 0; i < numAuthorizedNodes; i++) {
        if (isMacEqual(authorizedEdgeNodes[i], myMac)) continue;
        #ifdef ESP32
            // Always delete first to force a channel refresh — stale channel = silent fail
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
            // ESP8266: del_peer is best-effort (ignores error if not registered)
            esp_now_del_peer((uint8_t *)authorizedEdgeNodes[i]);
            esp_now_add_peer((uint8_t *)authorizedEdgeNodes[i], ESP_NOW_ROLE_COMBO, channel, (uint8_t *)LMK_KEY, 16);
        #endif
    }
}

bool routePayload(const SurvivorPayload& payload) {
    // 1. Try gateway if still marked connected
    if (gatewayConnected) {
        if (transmitPayload(payload, gatewayMacAddress, true)) return true;
        gatewayConnected = false;
        netlogln("[RADIO] Gateway lost. Escalating to mesh.");
    }

    return huntForPeers(payload);
}

// huntForPeers: multi-channel sweep with preferred-peer cache and
// round-robin extended retry over all known peers (10s total).
bool huntForPeers(const SurvivorPayload& payload) {
    // --- Step 1: Try preferred peer cache first (fast path) ---
    if (hasPreferredPeer) {
        int meshCh = (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
        registerEdgePeers(meshCh);
        String msg = "[MESH] Trying cached peer " + formatMac(preferredPeer);
        Serial.println(msg); netlogln(msg);
        if (transmitPayload(payload, preferredPeer, false)) {
            netlogln("[MESH] Cached peer ACK'd.");
            return true;
        }
        // Cache miss — clear and fall through to full scan
        hasPreferredPeer = false;
        netlogln("[MESH] Cached peer miss. Full channel sweep starting.");
    }

    // --- Step 2: Multi-channel sweep to discover reachable peer ---
    netlogln("[MESH] Channel sweep across " + String(MESH_CHANNEL_COUNT) + " channels...");
    for (int ci = 0; ci < MESH_CHANNEL_COUNT; ci++) {
        int ch = MESH_CHANNELS[ci];
        registerEdgePeers(ch);
        for (int i = 0; i < numAuthorizedNodes; i++) {
            if (isMacEqual(authorizedEdgeNodes[i], myMac)) continue;
            // Quick single-burst probe per peer per channel
            awaitedAckSequence = payload.sequence;
            ackReceived = false;
            #ifdef ESP32
                esp_now_send(authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #elif defined(ESP8266)
                esp_now_send((uint8_t*)authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #endif
            unsigned long t = millis();
            while (!ackReceived && millis() - t < MESH_PER_PEER_WAIT_MS) delay(1);
            if (ackReceived) {
                lastKnownChannel = ch;
                memcpy(preferredPeer, authorizedEdgeNodes[i], 6);
                hasPreferredPeer = true;
                String ok = "[MESH] Peer found on Ch " + String(ch) + " -> " + formatMac(preferredPeer);
                Serial.println(ok); netlogln(ok);
                return true;
            }
        }
    }

    // --- Step 3: Round-robin extended retry on last known channel (10s) ---
    // Channel sweep found nothing; stay on lastKnownChannel and hammer
    // all peers in rotation until the 10s window closes.
    int meshCh = (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
    registerEdgePeers(meshCh);
    netlogln("[MESH] Extended retry on Ch " + String(meshCh) + " (10s)...");
    unsigned long deadline = millis() + MESH_TOTAL_TIMEOUT_MS;
    while (millis() < deadline) {
        for (int i = 0; i < numAuthorizedNodes; i++) {
            if (isMacEqual(authorizedEdgeNodes[i], myMac)) continue;
            awaitedAckSequence = payload.sequence;
            ackReceived = false;
            #ifdef ESP32
                esp_now_send(authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #elif defined(ESP8266)
                esp_now_send((uint8_t*)authorizedEdgeNodes[i], (uint8_t *)&payload, sizeof(SurvivorPayload));
            #endif
            unsigned long t = millis();
            while (!ackReceived && millis() - t < MESH_PER_PEER_WAIT_MS) delay(1);
            if (ackReceived) {
                memcpy(preferredPeer, authorizedEdgeNodes[i], 6);
                hasPreferredPeer = true;
                String ok = "[MESH] Extended retry ACK from " + formatMac(preferredPeer);
                Serial.println(ok); netlogln(ok);
                return true;
            }
            if (millis() >= deadline) break;
        }
    }

    Serial.println("[MESH] All peers unreachable.");
    netlogln("[MESH] All peers unreachable after full sweep.");
    return false;
}

// ---------------------------------------------------------
// Gateway Hunt
bool huntForGateway() {
    Serial.println("\n[SYS] Executing Lighthouse channel sweep...");

    // WiFi.disconnect clears the home WiFi connection (used for UDP logging).
    // We mark netlogReady=false here and reconnect after the scan via
    // connectHomeWifi(), which is called from setup() and loop().
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

    if (!isMacZero(discoveredBssid)) {
        copyMac(discoveredBssid, gatewayMacAddress);
    } else if (!isMacZero(discoveredStaMac)) {
        copyMac(discoveredStaMac, gatewayMacAddress);
    }

    if (!ensureGatewayPeer(targetChannel)) {
        Serial.println("[SYS] WARNING: Gateway peer setup failed.");
        return false;
    }

    registerEdgePeers(targetChannel);
    gatewayConnected = true;
    return true;
}

// ---------------------------------------------------------
// Remote UDP Logger
// ---------------------------------------------------------
void netlog(const String& msg) {
    // Always mirror to hardware Serial
    Serial.print(msg);
    // Also broadcast over WiFi when home network is connected
    if (netlogReady) {
        String packet = "[NODE_" + String(myNodeId) + "] " + msg;
        netlogUdp.beginPacket("255.255.255.255", NETLOG_PORT);
        netlogUdp.print(packet);
        netlogUdp.endPacket();
    }
}

void netlogln(const String& msg) {
    // netlog() already calls Serial.print(msg + "\n"),
    // so no extra Serial.println() needed here.
    netlog(msg + "\n");
}

// Connect to the home router for UDP log reachability.
// Called after huntForGateway() so the ESP-NOW channel is already locked.
// The home router is on the SAME channel as the VOID_ AP (by design),
// so WiFi.begin() does not disturb ESP-NOW operation.
void connectHomeWifi() {
    if (netlogReady) return; // already up

    // Give the radio 200ms to settle after huntForGateway's scan+setChannel
    // sequence. Without this pause, WiFi.begin() can fail immediately on ESP32.
    delay(200);

    WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);

    // 40 x 200ms = 8 seconds. ESP32 STA association typically takes 3-5s
    // after a WiFi.disconnect() + scan + setChannel sequence.
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(200);
        tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
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
    connectHomeWifi(); // Start UDP logging after channel is locked

    // Item 1: If gateway not found at boot, try peers immediately.
    // This ensures the node doesn't silently sit idle waiting for the
    // first 60s heartbeat before attempting mesh routing.
    if (!gatewayConnected) {
        netlogln("[SYS] No gateway at boot. Probing mesh peers immediately.");
        registerEdgePeers((lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL);
        // Dummy probe: build a minimal SOS to wake up a nearby repeater
        SurvivorPayload bootProbe;
        bootProbe.nodeId    = myNodeId;
        bootProbe.batteryPct = 100;
        bootProbe.cpuLoad   = 0;
        bootProbe.isSosActive = true;
        bootProbe.sequence  = 0;
        bootProbe.ttl       = 1;   // one hop only for the probe
        bootProbe.uptimeMs  = millis();
        if (huntForPeers(bootProbe)) {
            netlogln("[SYS] Boot probe ACK'd by peer — mesh path active.");
        } else {
            netlogln("[SYS] No peers at boot. Will retry on first telemetry trigger.");
        }
    }

    Serial.println("[SYS] Node Armed. Adaptive Telemetry sequence starting.");
    netlogln("[SYS] Node " + String(myNodeId) + " armed. UDP log stream active.");
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
        if (!routePayload(edgeQueue[i])) {
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
    // Process any buffered incoming relay payload FIRST (deferred from OnDataRecv)
    processRepeatPayload();

    static float mockBattery = 100.0;
    mockBattery -= 0.05;
    if (mockBattery < 0) mockBattery = 100.0;

    outgoingTelemetry.nodeId     = myNodeId;
    outgoingTelemetry.batteryPct = (uint8_t)mockBattery;
    outgoingTelemetry.cpuLoad    = random(10, 30);
    outgoingTelemetry.isSosActive = (random(0, 100) > 95);
    outgoingTelemetry.ttl        = 3;
    outgoingTelemetry.uptimeMs   = millis(); // uptime since boot, for ordering

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
        outgoingTelemetry.uptimeMs = millis(); // refresh uptime at transmit time

        netlogln("\n--- Telemetry Triggered: " + triggerReason + " ---");

        if (!gatewayConnected) {
            // Try to route directly (to peers or gateway if re-discovered)
            if (routePayload(outgoingTelemetry)) {
                netlogln("[RADIO] ACK seq=" + String(outgoingTelemetry.sequence) + " bat=" + String(outgoingTelemetry.batteryPct) + "%");
                lastTxTime    = millis();
                lastTxBattery = outgoingTelemetry.batteryPct;
                lastTxCpu     = outgoingTelemetry.cpuLoad;
                lastTxSos     = outgoingTelemetry.isSosActive;
            } else {
                // routePayload failed — try gateway hunt then queue
                if (huntForGateway()) {
                    connectHomeWifi();
                    flushEdgeQueue();
                } else {
                    connectHomeWifi();
                    if (edgeQueueCount < EDGE_QUEUE_SIZE) {
                        edgeQueue[edgeQueueCount] = outgoingTelemetry;
                        edgeQueueCount++;
                        netlogln("[QUEUE] Both paths failed. Saved to RAM (" + String(edgeQueueCount) + "/20)");
                    }
                    lastTxTime    = millis();
                    lastTxBattery = outgoingTelemetry.batteryPct;
                    lastTxCpu     = outgoingTelemetry.cpuLoad;
                    lastTxSos     = outgoingTelemetry.isSosActive;
                }
            }
        } else {
            if (routePayload(outgoingTelemetry)) {
                netlogln("[RADIO] ACK seq=" + String(outgoingTelemetry.sequence) + " bat=" + String(outgoingTelemetry.batteryPct) + "%");
                lastTxTime    = millis();
                lastTxBattery = outgoingTelemetry.batteryPct;
                lastTxCpu     = outgoingTelemetry.cpuLoad;
                lastTxSos     = outgoingTelemetry.isSosActive;
            } else {
                netlogln("[RADIO] Delivery FAILED seq=" + String(outgoingTelemetry.sequence));
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
