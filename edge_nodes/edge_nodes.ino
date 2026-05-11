// -------------------------------------------------------------------------
// V.O.I.D. Edge Node - Survivor Telemetry (V2.3)
// -------------------------------------------------------------------------
// Architecture: ESP32 / ESP8266 Compatible
//
// V2.3 — Hop-Distance Vector Routing with RSSI Peer Scoring:
//   Extends the mesh with passive distributed Bellman-Ford routing.
//   Each node maintains a 5-entry neighbor table populated from received
//   payloads (zero probe overhead). Relay candidates are scored by a
//   composite formula (RSSI + hop-distance bonus) and probed in
//   priority order. The originating node's hopDist is preserved through
//   relays for gateway-side topology logging.
//
//   SurvivorPayload grows to 11 bytes (hopDist at offset 10).
//   MUST match gateway_node.ino — both ends use sizeof() checks.
// -------------------------------------------------------------------------

#ifdef ESP32
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" {
#include <user_interface.h>
}
#else
#error "Architecture not supported. Target must be ESP8266 or ESP32."
#endif

#include "node_registry.h"
#include "secrets.h"
#include <WiFiUdp.h>

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

// --- TIMING ---
const int ACK_WAIT_MS = 750;
const int TX_BURST_COUNT = 4;
#define MESH_TOTAL_TIMEOUT_MS 10000
#define MESH_PER_PEER_WAIT_MS 400
const int MESH_CHANNELS[] = {11, 6, 1};
const int MESH_CHANNEL_COUNT = 3;

// =========================================================================
// Hop-Distance Vector Constants
// =========================================================================
#define HOPDIST_UNKNOWN 255 // Node has not yet confirmed any path to GW
#define HOPDIST_GATEWAY 0   // Gateway's own distance (implicit, not stored)
#define MAX_HOPS 10         // Maximum plausible hop count in the mesh
#define RSSI_FLOOR -80      // dBm — peers below this are disqualified
#define HOPDIST_WEIGHT 10   // Score bonus per hop closer to gateway
#define NEIGHBOR_EXPIRY_MS 120000UL // 2 minutes — evict stale neighbors

// Neighbor table — populated passively from received payloads, zero probe
// overhead
#define NEIGHBOR_TABLE_SIZE 5
struct Neighbor {
  uint8_t mac[6];
  int8_t rssi;         // Last known signal strength (dBm)
  uint8_t hopDist;     // Their reported distance to gateway
  uint32_t lastSeenMs; // millis() when last heard
  bool valid;          // Slot is occupied
};
Neighbor neighborTable[NEIGHBOR_TABLE_SIZE];

// This node's self-estimated hop distance to gateway
uint8_t myHopDist = HOPDIST_UNKNOWN;

// --- PREFERRED PEER CACHE (RAM-only, cleared on reboot) ---
// Seeded from neighbor table best-score entry.
// Serves as a fast-path to avoid full table scan on every TX.
uint8_t preferredPeer[6] = {0, 0, 0, 0, 0, 0};
bool hasPreferredPeer = false;

// Duplicate suppression — 16-entry circular cache of (nodeId, seq) pairs
#define DUP_CACHE_SIZE 16
struct DupEntry {
  uint8_t nodeId;
  uint8_t seq;
};
DupEntry dupCache[DUP_CACHE_SIZE];
int dupCacheIdx = 0;

bool isDuplicate(uint8_t nodeId, uint8_t seq) {
  for (int i = 0; i < DUP_CACHE_SIZE; i++) {
    if (dupCache[i].nodeId == nodeId && dupCache[i].seq == seq)
      return true;
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

// =========================================================================
// SURVIVORPAYLOAD — MUST be byte-identical in gateway_node.ino.
// Both files use __attribute__((packed)) and sizeof() for len checks.
// =========================================================================
typedef struct __attribute__((packed)) SurvivorPayload {
  uint8_t nodeId;     // offset 0
  uint8_t batteryPct; // offset 1
  uint8_t cpuLoad;    // offset 2
  bool isSosActive;   // offset 3
  uint32_t uptimeMs;  // offset 4  (4-byte aligned — do not reorder)
  uint8_t sequence;   // offset 8
  uint8_t ttl;        // offset 9
  uint8_t hopDist;    // offset 10 — sender's hop distance to GW
} SurvivorPayload;    // sizeof = 11 bytes

typedef struct __attribute__((packed)) AckPayload {
  uint8_t msgType;
  uint8_t nodeId;
  uint8_t sequence;
} AckPayload;

// --- DEFERRED REPEATER PROCESSING ---
// On ESP32, protect the multi-field buffer with a critical section
// to avoid the dual-core race where the flag is set before both memcpys
// complete.
#ifdef ESP32
portMUX_TYPE repeatMux = portMUX_INITIALIZER_UNLOCKED;
#endif

volatile bool pendingRepeat = false;
uint8_t pendingRepeatSenderMac[6];
SurvivorPayload pendingRepeatPayload;

// --- PROMISCUOUS MODE RSSI SIDECAR (ESP32 only) ---
// ESP-NOW action frames don't expose RSSI in the receive callback.
// Promiscuous mode intercepts raw 802.11 frames and extracts hardware RSSI.
//
// 802.11 vendor-specific action frame layout:
//   Byte 0-1:   Frame Control (subtype 0xD0 = action frame)
//   Byte 2-3:   Duration/ID
//   Byte 4-9:   Address 1 (destination)
//   Byte 10-15: Address 2 (source / transmitter MAC)
//   Byte 16-21: Address 3 (BSSID)
//   Byte 22-23: Sequence Control
//   Byte 24:    Category (0x7F = vendor-specific)
//   Byte 25-27: OUI (Espressif = 0x18, 0xFE, 0x34)
#define RSSI_CACHE_SIZE 8
struct RssiEntry { uint8_t mac[6]; int8_t rssi; };
volatile RssiEntry rssiCache[RSSI_CACHE_SIZE];
volatile int rssiCacheIdx = 0;

void updateRssiCache(const uint8_t* mac, int8_t rssi) {
    int idx = rssiCacheIdx % RSSI_CACHE_SIZE;
    memcpy((void*)rssiCache[idx].mac, mac, 6);
    rssiCache[idx].rssi = rssi;
    rssiCacheIdx++;
}

int8_t lookupRssi(const uint8_t* mac) {
    for (int i = 0; i < RSSI_CACHE_SIZE; i++) {
        if (memcmp((void*)rssiCache[i].mac, mac, 6) == 0)
            return rssiCache[i].rssi;
    }
    return 0;
}

// Diagnostic counters — incremented in ISR, read in printNeighborTable
volatile uint32_t promiscTotal = 0;     // callback fired at all
volatile uint32_t promiscMgmt = 0;      // passed MGMT type check
volatile uint32_t promiscAction = 0;    // passed 0xD0 action check
volatile uint32_t promiscCategory = 0;  // passed 0x7F category check
volatile uint32_t promiscOui = 0;       // passed Espressif OUI check
volatile uint32_t promiscCached = 0;    // successfully cached RSSI

#ifdef ESP32
void IRAM_ATTR promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    promiscTotal++;
    if (type != WIFI_PKT_MGMT) return;
    promiscMgmt++;

    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = ppkt->payload;
    int frameLen = ppkt->rx_ctrl.sig_len;

    if (frameLen < 28) return;

    // Action frame: type=0 (mgmt), subtype=1101 → frame[0] = 0xD0
    if (frame[0] != 0xD0) return;
    promiscAction++;

    // Byte 24: category must be 0x7F (vendor-specific)
    if (frame[24] != 0x7F) return;
    promiscCategory++;

    // Byte 25-27: Espressif OUI {0x18, 0xFE, 0x34}
    static const uint8_t ESPRESSIF_OUI[] = {0x18, 0xFE, 0x34};
    if (memcmp(&frame[25], ESPRESSIF_OUI, 3) != 0) return;
    promiscOui++;

    // frame[10..15] = addr2 = transmitter MAC
    updateRssiCache(&frame[10], (int8_t)ppkt->rx_ctrl.rssi);
    promiscCached++;
}

// Must be called after any WiFi.begin() — the WiFi stack reset disables promiscuous mode
void enablePromiscuousRssi() {
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb);
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
}
#elif defined(ESP8266)
// ESP8266 fallback — no promiscuous RSSI available

#if 0 // DISABLED: Promiscuous mode on ESP8266 intercepts all packets at baseband, breaking ESP-NOW Rx and Station mode.
// ESP8266 SDK structures for promiscuous mode (often hidden from user headers)
struct RxControl {
    signed rssi:8;
    unsigned rate:4;
    unsigned is_group:1;
    unsigned:1;
    unsigned sig_mode:2;
    unsigned legacy_length:12;
    unsigned damatch0:1;
    unsigned damatch1:1;
    unsigned bssidmatch0:1;
    unsigned bssidmatch1:1;
    unsigned MCS:7;
    unsigned CWB:1;
    unsigned HT_length:16;
    unsigned Smoothing:1;
    unsigned Not_Sounding:1;
    unsigned:1;
    unsigned Aggregation:1;
    unsigned STBC:2;
    unsigned FEC_CODING:1;
    unsigned SGI:1;
    unsigned rxend_state:8;
    unsigned ampdu_cnt:8;
    unsigned channel:4;
    unsigned:12;
};

struct sniffer_buf2 {
    struct RxControl rx_ctrl;
    uint8_t buf[112];
    uint16_t cnt;
    uint16_t len;
};

void ICACHE_RAM_ATTR promiscuous_rx_cb_esp8266(uint8_t *buf, uint16_t len) {
    promiscTotal++;

    // On ESP8266, buf points to a sniffer_buf2 structure for management frames
    if (len < sizeof(struct RxControl)) return;
    
    struct sniffer_buf2 *sniffer = (struct sniffer_buf2 *)buf;
    uint8_t *frame = sniffer->buf;
    promiscMgmt++; // No strict packet type filtering upstream like ESP32

    // NOTE: 'len' here is the size of the sniffer_buf2 struct (~128 bytes),
    // not the 802.11 frame length. Since sniffer_buf2.buf is 112 bytes, 
    // there are always at least 28 readable bytes. This check is harmless 
    // but semantically misleading on ESP8266.
    if (len - sizeof(struct RxControl) < 28) return;

    // Frame Control byte 0 = 0xD0 for action frame
    if (frame[0] != 0xD0) return;
    promiscAction++;

    // Byte 24: category = 0x7F
    if (frame[24] != 0x7F) return;
    promiscCategory++;

    // Bytes 25-27: Espressif OUI
    static const uint8_t ESPRESSIF_OUI[] = {0x18, 0xFE, 0x34};
    if (memcmp(&frame[25], ESPRESSIF_OUI, 3) != 0) return;
    promiscOui++;

    // addr2 = transmitter MAC
    updateRssiCache(&frame[10], sniffer->rx_ctrl.rssi);
    promiscCached++;
}
#endif // #if 0

void enablePromiscuousRssi() {
    // CRITICAL HARDWARE LIMITATION: 
    // On the ESP8266, enabling promiscuous mode intercepts ALL incoming packets
    // at the baseband level. This completely severs the ESP-NOW receive callback
    // (making the node "deaf" to peers) and breaks the Station interface (causing
    // UDP logger crashes / infinite reboots).
    //
    // We proved the RSSI extraction works in isolation (diagnostics.ino), but 
    // it cannot run concurrently with a functioning ESP-NOW mesh on this hardware.
    // 
    // wifi_promiscuous_enable(0);
    // wifi_set_promiscuous_rx_cb(promiscuous_rx_cb_esp8266);
    // wifi_promiscuous_enable(1);
}

#endif

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
volatile uint8_t awaitedAckNode = 0;

// =========================================================================
// Utility: MAC Helpers
// =========================================================================
uint8_t deriveNodeId() {
  delay(200);
  String fullMac = WiFi.macAddress();
  Serial.print("[SYS] Raw Hardware MAC: ");
  Serial.println(fullMac);
  if (fullMac.length() >= 17) {
    String lastByteStr = fullMac.substring(15, 17);
    return (uint8_t)strtol(lastByteStr.c_str(), NULL, 16);
  }
  Serial.println("[SYS] ERROR: Hardware MAC malformed.");
  return 0;
}

void parseMacString(String macText, uint8_t *macArray) {
  for (int i = 0; i < 6; i++) {
    String byteString = macText.substring(i * 2, i * 2 + 2);
    macArray[i] = (uint8_t)strtol(byteString.c_str(), NULL, 16);
  }
}

void copyMac(const uint8_t *source, uint8_t *dest) { memcpy(dest, source, 6); }

bool isMacZero(const uint8_t *mac) {
  for (int i = 0; i < 6; i++)
    if (mac[i] != 0)
      return false;
  return true;
}

bool isMacEqual(const uint8_t *mac1, const uint8_t *mac2) {
  for (int i = 0; i < 6; i++)
    if (mac1[i] != mac2[i])
      return false;
  return true;
}

String formatMac(const uint8_t *mac) {
  char macText[13];
  snprintf(macText, sizeof(macText), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(macText);
}

// =========================================================================
// Neighbor Table Operations
// All called from loop() context — safe to use Serial here.
// =========================================================================

// Compute composite routing score for a neighbor entry.
// Higher = better relay candidate.
// Formula: rssi + (MAX_HOPS - hopDist) * HOPDIST_WEIGHT
// RSSI_FLOOR check must be done before calling this.
int neighborScore(const Neighbor &n) {
  if (n.hopDist == HOPDIST_UNKNOWN)
    return -9999; // Unroutable peer
  int distBonus = (MAX_HOPS - (int)n.hopDist) * HOPDIST_WEIGHT;
  return (int)n.rssi + distBonus;
}

// Update or insert a neighbor record. Called from processRepeatPayload()
// where we have a fully decoded SurvivorPayload and its RSSI.
// O(NEIGHBOR_TABLE_SIZE) — safe for main loop, not for ISR.
void updateNeighborTable(const uint8_t *mac, int8_t rssi, uint8_t hopDist) {
  int freeSlot = -1;
  uint32_t now = millis();

  // Scan for existing entry or find a free/expired slot
  for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
    if (neighborTable[i].valid && isMacEqual(neighborTable[i].mac, mac)) {
      // Update existing entry
      neighborTable[i].rssi = rssi;
      neighborTable[i].hopDist = hopDist;
      neighborTable[i].lastSeenMs = now;
      return;
    }
    if (!neighborTable[i].valid ||
        (now - neighborTable[i].lastSeenMs > NEIGHBOR_EXPIRY_MS)) {
      freeSlot = i; // Prefer first expired/free slot
    }
  }

  // Insert into free or expired slot
  if (freeSlot >= 0) {
    memcpy(neighborTable[freeSlot].mac, mac, 6);
    neighborTable[freeSlot].rssi = rssi;
    neighborTable[freeSlot].hopDist = hopDist;
    neighborTable[freeSlot].lastSeenMs = now;
    neighborTable[freeSlot].valid = true;
  }
  // If all slots are occupied with fresh entries, drop this update.
  // A full table of fresh entries means we're well-connected — acceptable.
}

// Return the best neighbor for relaying: lowest hopDist, highest RSSI,
// above the RSSI floor. Returns NULL if no qualified neighbor exists.
// Used by huntForPeers() to sort candidates before probing.
const Neighbor *bestNeighbor() {
  const Neighbor *best = nullptr;
  int bestScore = -9999;
  uint32_t now = millis();

  for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
    if (!neighborTable[i].valid)
      continue;
    if (now - neighborTable[i].lastSeenMs > NEIGHBOR_EXPIRY_MS)
      continue;
    // On ESP8266, rssi=0 (no sidecar) — above RSSI_FLOOR, so hopDist carries full weight
    if (neighborTable[i].rssi < RSSI_FLOOR)
      continue;
    if (neighborTable[i].hopDist == HOPDIST_UNKNOWN)
      continue; // unroutable

    int score = neighborScore(neighborTable[i]);
    if (score > bestScore) {
      bestScore = score;
      best = &neighborTable[i];
    }
  }
  return best;
}

// Forward-declared here so printNeighborTable() can use netlogln()
void netlogln(const String& msg);
void netlog(const String& msg);

void printNeighborTable() {
  netlogln("[NBR] --- Neighbor Table ---");
  uint32_t now = millis();
  bool anyValid = false;
  for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
    if (!neighborTable[i].valid)
      continue;
    anyValid = true;
    bool stale = (now - neighborTable[i].lastSeenMs > NEIGHBOR_EXPIRY_MS);
    String line = "[NBR]   " + formatMac(neighborTable[i].mac) +
                  "  RSSI=" + String(neighborTable[i].rssi) + "dBm" +
                  "  hopDist=" + String(neighborTable[i].hopDist) +
                  "  score=" + String(neighborScore(neighborTable[i]));
    if (stale) line += "  [STALE]";
    netlogln(line);
  }
  if (!anyValid) netlogln("[NBR]   (empty)");
  
  #ifdef ESP32
  netlogln("[NBR] promisc_total=" + String(promiscTotal));
  netlogln("[NBR] promisc_action=" + String(promiscAction));
  netlogln("[NBR] promisc_oui=" + String(promiscOui));
  netlogln("[NBR] promisc_cached=" + String(promiscCached));
  #else
  netlogln("[NBR] RSSI sidecar: DISABLED (ESP8266 hw limit)");
  #endif
  
  netlogln("[NBR] -----------------------");
}

// =========================================================================
// Forward Declarations
// =========================================================================
bool transmitPayload(const SurvivorPayload &payload, const uint8_t *targetMac,
                     bool logAttempts);
bool huntForPeers(const SurvivorPayload &payload);
bool routePayload(const SurvivorPayload &payload);
void processRepeatPayload();
bool huntForGateway();
void connectHomeWifi();
void netlogln(const String &msg);
void netlog(const String &msg);
void flushEdgeQueue();

// =========================================================================
// ESP-NOW Transport
// =========================================================================
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
  return esp_now_add_peer(gatewayMacAddress, ESP_NOW_ROLE_COMBO, channel,
                          (uint8_t *)LMK_KEY, 16) == 0;
#endif
}

#ifdef ESP32
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData,
                int len) {
#elif defined(ESP8266)
void OnDataRecv(uint8_t *mac_addr, uint8_t *incomingData, uint8_t len) {
#endif
  // KEEP THIS CALLBACK MINIMAL.
  // Allowed: memcpy, flag set, simple arithmetic, array index.
  // Forbidden: String(), Serial.print(), delay(), esp_now_send(),
  //            any function that allocates heap or blocks.

  if (len == sizeof(AckPayload)) {
    AckPayload ack;
    memcpy(&ack, incomingData, sizeof(AckPayload));
    if (ack.msgType == ACK_MSG_TYPE && ack.nodeId == awaitedAckNode &&
        ack.sequence == awaitedAckSequence) {
      ackReceived = true;
    }
  } else if (len == sizeof(SurvivorPayload) && !pendingRepeat) {

// Protect buffer + flag atomically on ESP32 dual-core
#ifdef ESP32
    portENTER_CRITICAL(&repeatMux);
#endif
    memcpy((void *)&pendingRepeatPayload, incomingData,
           sizeof(SurvivorPayload));
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
  if (!pendingRepeat)
    return;

  // Snapshot and clear the flag atomically
  SurvivorPayload fwdPayload;
  uint8_t senderMac[6];
#ifdef ESP32
  portENTER_CRITICAL(&repeatMux);
#endif
  memcpy(&fwdPayload, (void *)&pendingRepeatPayload, sizeof(SurvivorPayload));
  memcpy(senderMac, pendingRepeatSenderMac, 6);
  pendingRepeat = false;
#ifdef ESP32
  portEXIT_CRITICAL(&repeatMux);
#endif

  // TTL check BEFORE sending ACK
  if (fwdPayload.ttl == 0 || fwdPayload.nodeId == myNodeId)
    return;

  // Always ACK the upstream sender BEFORE duplicate check.
  // A rebooted node reuses seq numbers, so the relay's dupCache may
  // contain stale entries. If we suppress the ACK, the sender retries
  // forever and never establishes the relay path.
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
          tmpPeer.encrypt = true;
          memcpy(tmpPeer.lmk, LMK_KEY, 16);
          esp_now_add_peer(&tmpPeer);
      }
      esp_now_send(senderMac, (uint8_t *)&edgeAck, sizeof(AckPayload));
  #elif defined(ESP8266)
      esp_now_del_peer(senderMac);
      esp_now_add_peer(senderMac, ESP_NOW_ROLE_COMBO, 0, (uint8_t *)LMK_KEY, 16);
      esp_now_send(senderMac, (uint8_t *)&edgeAck, sizeof(AckPayload));
  #endif

  // Update neighbor table — RSSI from promiscuous sidecar, refreshed even for duplicates
  updateNeighborTable(senderMac, lookupRssi(senderMac), fwdPayload.hopDist);

  // Duplicate suppression — drop already-seen (nodeId, seq) pairs
  if (isDuplicate(fwdPayload.nodeId, fwdPayload.sequence)) {
    netlogln("[REPEATER] Duplicate suppressed: Node " +
             String(fwdPayload.nodeId) + " seq=" + String(fwdPayload.sequence));
    return;
  }
  recordSeen(fwdPayload.nodeId, fwdPayload.sequence);

  fwdPayload.ttl--;
  // NOTE: fwdPayload.hopDist is NOT modified — it represents the
  // originating node's distance to GW, preserved for gateway-side logging.

  // Forward toward gateway or save to queue — NO connectHomeWifi() here
  if (fwdPayload.isSosActive) {
    netlogln("[REPEATER] Priority SOS from Node " + String(fwdPayload.nodeId) +
             " (hopDist=" + String(fwdPayload.hopDist) + ") — forwarding");
    bool forwarded = false;
    if (gatewayConnected) {
      uint8_t savedSeq = awaitedAckSequence;
      uint8_t savedNode = awaitedAckNode;
      bool savedAck = ackReceived;
      forwarded = transmitPayload(fwdPayload, gatewayMacAddress, true);
      awaitedAckSequence = savedSeq;
      awaitedAckNode = savedNode;
      ackReceived = savedAck;
    }
    if (!forwarded) {
      if (edgeQueueCount < EDGE_QUEUE_SIZE) {
        edgeQueue[edgeQueueCount++] = fwdPayload;
      }
      netlogln("[REPEATER] GW unavailable. SOS queued for retry.");
    }
  } else {
    netlogln("[REPEATER] Normal payload from Node " +
             String(fwdPayload.nodeId) +
             " (hopDist=" + String(fwdPayload.hopDist) + ") queued");
    if (edgeQueueCount < EDGE_QUEUE_SIZE) {
      edgeQueue[edgeQueueCount++] = fwdPayload;
    }
  }
}

bool transmitPayload(const SurvivorPayload &payload, const uint8_t *targetMac,
                     bool logAttempts) {
  awaitedAckSequence = payload.sequence;
  awaitedAckNode = payload.nodeId;
  ackReceived = false;

  for (int attempt = 0; attempt < TX_BURST_COUNT; attempt++) {
#ifdef ESP32
    esp_err_t sendResult =
        esp_now_send(targetMac, (uint8_t *)&payload, sizeof(SurvivorPayload));
    if (sendResult != ESP_OK) {
      Serial.print("[RADIO] Unicast dispatch failed code=");
      Serial.println((int)sendResult);
      return false;
    }
#elif defined(ESP8266)
    int sendResult = esp_now_send((uint8_t *)targetMac, (uint8_t *)&payload,
                                  sizeof(SurvivorPayload));
    if (sendResult != 0) {
      Serial.print("[RADIO] Unicast dispatch failed code=");
      Serial.println(sendResult);
      return false;
    }
#endif

    unsigned long waitStart = millis();
    while (!ackReceived && (millis() - waitStart) < ACK_WAIT_MS) {
      yield(); // Feeds ESP8266 WDT and allows callbacks
    }

    if (ackReceived)
      return true;

    if (logAttempts) {
      Serial.print("[RADIO] No ACK after burst ");
      Serial.print(attempt + 1);
      Serial.print("/");
      Serial.println(TX_BURST_COUNT);
    }
  }
  return false;
}

// =========================================================================
// Mesh Routing & Discovery
// =========================================================================
void registerEdgePeers(int channel) {
  for (int i = 0; i < numAuthorizedNodes; i++) {
    if (isMacEqual(authorizedEdgeNodes[i], myMac))
      continue;
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
    esp_now_add_peer((uint8_t *)authorizedEdgeNodes[i], ESP_NOW_ROLE_COMBO,
                     channel, (uint8_t *)LMK_KEY, 16);
#endif
  }
}

bool routePayload(const SurvivorPayload &payload) {
  if (gatewayConnected) {
    if (transmitPayload(payload, gatewayMacAddress, true))
      return true;
    gatewayConnected = false;
    netlogln("[RADIO] Gateway lost. Escalating to mesh.");
  }
  return huntForPeers(payload);
}

// =========================================================================
// huntForPeers — Hop-distance vector routing with cooperative yielding.
//
// Priority chain:
//   1. Neighbor table fast path (best-scored candidate, O(5) scan)
//   2. Cached preferred peer (unchanged from V2.2)
//   3. Full channel sweep across authorizedEdgeNodes
//   4. Extended retry on lastKnownChannel (10s window)
//
// After a successful relay, myHopDist is updated:
//   myHopDist = relayPeer.hopDist + 1
// This propagates correct distance information in future outgoing payloads.
// =========================================================================

// Look up a relay peer's hopDist from the neighbor table and update myHopDist.
// Falls back to 2 if the peer is not yet in the table (minimum relay distance).
void updateHopDistFromRelay(const uint8_t* relayMac) {
    uint32_t now = millis();
    for (int i = 0; i < NEIGHBOR_TABLE_SIZE; i++) {
        if (!neighborTable[i].valid) continue;
        if (now - neighborTable[i].lastSeenMs > NEIGHBOR_EXPIRY_MS) continue;
        if (isMacEqual(neighborTable[i].mac, relayMac)) {
            uint8_t newDist = (neighborTable[i].hopDist < MAX_HOPS)
                                  ? neighborTable[i].hopDist + 1
                                  : HOPDIST_UNKNOWN;
            if (newDist != myHopDist) {
                myHopDist = newDist;
                netlogln("[MESH] myHopDist updated to " + String(myHopDist) +
                         " via relay " + formatMac(relayMac));
            }
            return;
        }
    }
    // Peer not in table yet — set floor distance of 2 (at least one relay hop)
    if (myHopDist < 2 || myHopDist == HOPDIST_UNKNOWN) {
        myHopDist = 2;
        netlogln("[MESH] myHopDist set to 2 (relay peer not yet in table)");
    }
}

bool huntForPeers(const SurvivorPayload &payload) {

  // ---- Neighbor table fast path ----
  const Neighbor *best = bestNeighbor();
  if (best != nullptr) {
    int meshCh =
        (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
    registerEdgePeers(meshCh);
    netlogln("[MESH] NBR table: trying best peer " + formatMac(best->mac) +
             " score=" + String(neighborScore(*best)) + " hopDist=" +
             String(best->hopDist) + " RSSI=" + String(best->rssi) + "dBm");

    if (transmitPayload(payload, best->mac, false)) {
      // Update myHopDist: we are one hop further than this relay
      uint8_t newDist =
          (best->hopDist < MAX_HOPS) ? best->hopDist + 1 : HOPDIST_UNKNOWN;
      if (newDist != myHopDist) {
        myHopDist = newDist;
        netlogln("[MESH] myHopDist updated to " + String(myHopDist) +
                 " via relay " + formatMac(best->mac));
      }
      memcpy(preferredPeer, best->mac, 6);
      hasPreferredPeer = true;
      netlogln("[MESH] NBR table peer ACK'd.");
      return true;
    }
    netlogln("[MESH] NBR table peer miss. Falling to sweep.");
  }

  // ---- Cached preferred peer fast path ----
  if (hasPreferredPeer) {
    int meshCh =
        (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
    registerEdgePeers(meshCh);
    netlogln("[MESH] Trying cached peer " + formatMac(preferredPeer));
    if (transmitPayload(payload, preferredPeer, false)) {
      updateHopDistFromRelay(preferredPeer);
      netlogln("[MESH] Cached peer ACK'd.");
      return true;
    }
    hasPreferredPeer = false;
    netlogln("[MESH] Cached peer miss. Full channel sweep starting.");
  }

  // ---- Multi-channel sweep ----
  netlogln("[MESH] Channel sweep across " + String(MESH_CHANNEL_COUNT) +
           " channels...");
  for (int ci = 0; ci < MESH_CHANNEL_COUNT; ci++) {
    int ch = MESH_CHANNELS[ci];
    registerEdgePeers(ch);
    for (int i = 0; i < numAuthorizedNodes; i++) {
      if (isMacEqual(authorizedEdgeNodes[i], myMac))
        continue;

      awaitedAckSequence = payload.sequence;
      awaitedAckNode = payload.nodeId;
      ackReceived = false;

#ifdef ESP32
      esp_now_send(authorizedEdgeNodes[i], (uint8_t *)&payload,
                   sizeof(SurvivorPayload));
#elif defined(ESP8266)
      esp_now_send((uint8_t *)authorizedEdgeNodes[i], (uint8_t *)&payload,
                   sizeof(SurvivorPayload));
#endif

      unsigned long t = millis();
      while (!ackReceived && millis() - t < MESH_PER_PEER_WAIT_MS)
        yield();
      if (ackReceived) {
        lastKnownChannel = ch;
        memcpy(preferredPeer, authorizedEdgeNodes[i], 6);
        hasPreferredPeer = true;
        netlogln("[MESH] Peer found on Ch " + String(ch) + " -> " +
                 formatMac(preferredPeer));
        updateHopDistFromRelay(preferredPeer);
        return true;
      }
    }
  }

  // ---- Extended retry on lastKnownChannel (10s window) ----
  int meshCh =
      (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
  registerEdgePeers(meshCh);
  netlogln("[MESH] Extended retry on Ch " + String(meshCh) + " (10s)...");
  unsigned long deadline = millis() + MESH_TOTAL_TIMEOUT_MS;
  while (millis() < deadline) {
    processRepeatPayload(); // Service repeater buffer during long retry
    for (int i = 0; i < numAuthorizedNodes; i++) {
      if (isMacEqual(authorizedEdgeNodes[i], myMac))
        continue;

      awaitedAckSequence = payload.sequence;
      awaitedAckNode = payload.nodeId;
      ackReceived = false;

#ifdef ESP32
      esp_now_send(authorizedEdgeNodes[i], (uint8_t *)&payload,
                   sizeof(SurvivorPayload));
#elif defined(ESP8266)
      esp_now_send((uint8_t *)authorizedEdgeNodes[i], (uint8_t *)&payload,
                   sizeof(SurvivorPayload));
#endif

      unsigned long t = millis();
      while (!ackReceived && millis() - t < MESH_PER_PEER_WAIT_MS)
        yield();
      if (ackReceived) {
        memcpy(preferredPeer, authorizedEdgeNodes[i], 6);
        hasPreferredPeer = true;
        netlogln("[MESH] Extended retry ACK from " + formatMac(preferredPeer));
        updateHopDistFromRelay(preferredPeer);
        return true;
      }
      if (millis() >= deadline)
        break;
    }
  }

  Serial.println("[MESH] All peers unreachable.");
  netlogln("[MESH] All peers unreachable after full sweep.");
  return false;
}

// =========================================================================
// Gateway Hunt
// =========================================================================
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
      const uint8_t *bssid = WiFi.BSSID(i);
      if (bssid != nullptr)
        copyMac(bssid, discoveredBssid);
      String extractedMac = foundSSID.substring(5);
      if (extractedMac.length() >= 12)
        parseMacString(extractedMac, discoveredStaMac);
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
    targetChannel =
        (lastKnownChannel != 0) ? lastKnownChannel : DEFAULT_MESH_CHANNEL;
    Serial.print("[SYS] WARNING: Gateway Lighthouse not found. Falling back to "
                 "Mesh Channel ");
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

  if (!isMacZero(discoveredBssid))
    copyMac(discoveredBssid, gatewayMacAddress);
  else if (!isMacZero(discoveredStaMac))
    copyMac(discoveredStaMac, gatewayMacAddress);

  if (!ensureGatewayPeer(targetChannel)) {
    Serial.println("[SYS] WARNING: Gateway peer setup failed.");
    return false;
  }
  registerEdgePeers(targetChannel);
  gatewayConnected = true;
  return true;
}

// =========================================================================
// Remote UDP Logger
// =========================================================================
void netlog(const String &msg) {
  Serial.print(msg);
  if (netlogReady) {
    String packet = "[NODE_" + String(myNodeId) + "] " + msg;
    netlogUdp.beginPacket("255.255.255.255", NETLOG_PORT);
    netlogUdp.print(packet);
    netlogUdp.endPacket();
  }
}

void netlogln(const String &msg) { netlog(msg + "\n"); }

void connectHomeWifi() {
  if (netlogReady)
    return;
  delay(200);
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(200);
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    int connectedChannel = WiFi.channel();
    if (lastKnownChannel != 0 && connectedChannel != lastKnownChannel) {
      Serial.print("[NETLOG] WARNING: Router channel ");
      Serial.print(connectedChannel);
      Serial.print(" != ESP-NOW channel ");
      Serial.print(lastKnownChannel);
      Serial.println(". Re-registering peers.");
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
  // Re-enable promiscuous RSSI sidecar — WiFi.begin() disables it
  enablePromiscuousRssi();
}

// =========================================================================
// System Initialization
// =========================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- V.O.I.D. Edge Node Booting (V2.3) ---");

  memset(dupCache, 0, sizeof(dupCache));
  // Clear neighbor table
  memset(neighborTable, 0, sizeof(neighborTable));
  myHopDist = HOPDIST_UNKNOWN;

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
    while (1)
      delay(1000);
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
    registerEdgePeers((lastKnownChannel != 0) ? lastKnownChannel
                                              : DEFAULT_MESH_CHANNEL);
    SurvivorPayload bootProbe;
    bootProbe.nodeId = myNodeId;
    bootProbe.batteryPct = 100;
    bootProbe.cpuLoad = 0;
    bootProbe.isSosActive = true;
    bootProbe.sequence = 0;
    bootProbe.ttl = 2;
    bootProbe.uptimeMs = millis();
    bootProbe.hopDist = HOPDIST_UNKNOWN;
    if (huntForPeers(bootProbe)) {
      netlogln("[SYS] Boot probe ACK'd by peer — mesh path active.");
    } else {
      netlogln(
          "[SYS] No peers at boot. Will retry on first telemetry trigger.");
    }
  } else {
    // Direct gateway connection confirmed at boot — we are 1 hop
    myHopDist = 1;
    netlogln("[MESH] Direct gateway link confirmed. myHopDist=1");
  }

  Serial.println("[SYS] Node Armed. V2.3 Adaptive Telemetry starting.");
  netlogln("[SYS] Node " + String(myNodeId) +
           " armed. hopDist=" + String(myHopDist));
}

// =========================================================================
// Queue Flush
// =========================================================================
void flushEdgeQueue() {
  if (edgeQueueCount == 0)
    return;
  Serial.print("\n[QUEUE] Flushing ");
  Serial.print(edgeQueueCount);
  Serial.println(" saved payloads to Gateway...");

  int originalCount = edgeQueueCount;
  int sentCount = 0;
  int firstFailedIndex = -1;

  for (int i = 0; i < originalCount; i++) {
    processRepeatPayload();
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

  if (firstFailedIndex < 0)
    firstFailedIndex = 0;
  int unsentCount = originalCount - firstFailedIndex;
  for (int i = 0; i < unsentCount; i++) {
    edgeQueue[i] = edgeQueue[firstFailedIndex + i];
  }
  edgeQueueCount = unsentCount;
  Serial.print("[QUEUE] Preserved ");
  Serial.print(edgeQueueCount);
  Serial.println(" unsent payload(s) in RAM.");
}

// =========================================================================
// Main Execution Loop
// =========================================================================
void loop() {
  // Always service the repeater buffer FIRST, unconditionally
  processRepeatPayload();

  // Flush relay-queued packets during idle time
  if (edgeQueueCount > 0 && gatewayConnected) {
    flushEdgeQueue();
  }

  // Periodic neighbor table dump (every 30s) for diagnostics
  static unsigned long lastNbrDump = 0;
  if (millis() - lastNbrDump > 30000) {
      lastNbrDump = millis();
      printNeighborTable();
  }

  static float mockBattery = 100.0;
  mockBattery -= 0.05;
  if (mockBattery < 0)
    mockBattery = 100.0;

  outgoingTelemetry.nodeId = myNodeId;
  outgoingTelemetry.batteryPct = (uint8_t)mockBattery;
  outgoingTelemetry.cpuLoad = random(10, 30);
  outgoingTelemetry.isSosActive = (random(0, 100) > 97);
  outgoingTelemetry.ttl = 3;
  outgoingTelemetry.uptimeMs = millis();
  outgoingTelemetry.hopDist = myHopDist;

  bool triggerTransmission = false;
  String triggerReason = "";

  if (outgoingTelemetry.isSosActive) {
    triggerTransmission = true;
    triggerReason = "EMERGENCY SOS BURST";
  } else if (lastTxBattery > outgoingTelemetry.batteryPct + 4) {
    triggerTransmission = true;
    triggerReason = "BATTERY DROP EXCEPTION";
  } else if (abs(lastTxCpu - outgoingTelemetry.cpuLoad) > 30) {
    triggerTransmission = true;
    triggerReason = "CPU SPIKE EXCEPTION";
  } else if (millis() - lastTxTime > 80000) {
    triggerTransmission = true;
    triggerReason = "80s HEARTBEAT";
  }

  if (triggerTransmission) {
    outgoingTelemetry.sequence = nextSequence++;
    outgoingTelemetry.uptimeMs = millis();
    outgoingTelemetry.hopDist = myHopDist;

    netlogln("\n--- Telemetry Triggered: " + triggerReason +
             " | hopDist=" + String(myHopDist) + " ---");

    bool sent = routePayload(outgoingTelemetry);
    if (sent) {
      // If we went direct to gateway, we are 1 hop
      if (gatewayConnected) {
        if (myHopDist != 1) {
          myHopDist = 1;
          netlogln("[MESH] Direct GW TX confirmed. myHopDist=1");
        }
      }
      netlogln("[RADIO] ACK seq=" + String(outgoingTelemetry.sequence) +
               " bat=" + String(outgoingTelemetry.batteryPct) + "%" +
               " hopDist=" + String(myHopDist));
      lastTxTime = millis();
      lastTxBattery = outgoingTelemetry.batteryPct;
      lastTxCpu = outgoingTelemetry.cpuLoad;
      lastTxSos = outgoingTelemetry.isSosActive;
      flushEdgeQueue();
    } else {
      netlogln("[RADIO] Delivery FAILED seq=" +
               String(outgoingTelemetry.sequence));
      gatewayConnected = false;

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
        lastTxTime = millis();
        lastTxBattery = outgoingTelemetry.batteryPct;
        lastTxCpu = outgoingTelemetry.cpuLoad;
        lastTxSos = outgoingTelemetry.isSosActive;
      }
    }
  }

  yield();    // Feed WDT during idle
  delay(100); // Responsive loop cadence
}
