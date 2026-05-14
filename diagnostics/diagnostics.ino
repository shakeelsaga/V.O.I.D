// =============================================================================
// V.O.I.D. — RSSI Promiscuous Diagnostic Sketch
// =============================================================================
// Standalone. No node_registry.h, no secrets.h.
// Purpose: Determine exactly why promiscuous RSSI sidecar returns 0.
//
// WHAT THIS DOES:
//   1. Inits WiFi STA + ESP-NOW
//   2. Enables promiscuous mode with full return-code logging
//   3. Every 10s: sends a broadcast ESP-NOW packet (self-stimulus)
//   4. Every 5s:  dumps all diagnostic counters to Serial
//
// HOW TO USE:
//   1. Fill in DIAG_WIFI_SSID / DIAG_WIFI_PASS below (or leave blank —
//      WiFi connection is optional; promiscuous sidecar test works without it)
//   2. Flash to your ESP32 edge node
//   3. Open Serial Monitor at 115200
//   4. Let run for 60s minimum
//   5. Copy the entire serial output and share it
//
// INTERPRETING THE OUTPUT (share this with Claude):
//   [PROMISC-INIT] lines  → whether promiscuous mode actually activated
//   [TICK] lines          → counter dump every 5s
//     total=0             → callback never fires (mode not active or power save)
//     total>0, mgmt=0     → WIFI_PKT_MGMT filter blocking frames
//     action=0            → frame[0] != 0xD0 (wrong action subtype byte)
//     cat=0               → byte 24 != 0x7F (wrong category offset)
//     oui=0               → OUI mismatch at bytes 25-27
//     cached=0            → MAC comparison failing in updateRssiCache
//   [SELF-TX] lines       → broadcast ESP-NOW sent to self as stimulus
//   [RECV] lines          → ESP-NOW callback received a packet
//   [LOOKUP] lines        → RSSI lookup result for received packet
// =============================================================================

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
#endif

// ---- FILL IN YOUR WIFI CREDS (optional — leave blank to skip WiFi) --------
#define DIAG_WIFI_SSID  "Loading..."
#define DIAG_WIFI_PASS  "stillbuffering"
// ---------------------------------------------------------------------------

// ---- Diagnostic counters (all incremented inside ISR — volatile) ----------
volatile uint32_t promiscTotal    = 0;   // callback fired at all
volatile uint32_t promiscMgmt     = 0;   // passed WIFI_PKT_MGMT check
volatile uint32_t promiscAction   = 0;   // passed frame[0] == 0xD0
volatile uint32_t promiscCategory = 0;   // passed frame[24] == 0x7F
volatile uint32_t promiscOui      = 0;   // passed Espressif OUI check
volatile uint32_t promiscCached   = 0;   // updateRssiCache() called

volatile uint32_t espnowRecvCount = 0;   // ESP-NOW OnDataRecv fired
volatile uint32_t selfTxCount     = 0;   // self-stimulus sends

// ---- RSSI cache (same as production) --------------------------------------
#define RSSI_CACHE_SIZE 8
struct RssiEntry { uint8_t mac[6]; int8_t rssi; };
volatile RssiEntry rssiCache[RSSI_CACHE_SIZE];
volatile int rssiCacheIdx = 0;

void updateRssiCache(const uint8_t* mac, int8_t rssi) {
    int idx = rssiCacheIdx % RSSI_CACHE_SIZE;
    memcpy((void*)rssiCache[idx].mac, mac, 6);
    rssiCache[idx].rssi = rssi;
    rssiCacheIdx++;
    promiscCached++;
}

int8_t lookupRssi(const uint8_t* mac) {
    for (int i = 0; i < RSSI_CACHE_SIZE; i++) {
        if (memcmp((void*)rssiCache[i].mac, mac, 6) == 0)
            return rssiCache[i].rssi;
    }
    return 0;
}

// ---- Raw byte dump helper (prints first N bytes of frame as hex) ----------
// Safe to call only from loop() context, not ISR.
// We capture a snapshot of the last seen frame for diagnostic printing.
volatile bool   hasLastFrame  = false;
volatile uint8_t lastFrame[64];    // first 64 bytes of last MGMT frame
volatile int    lastFrameLen = 0;
volatile int8_t lastRssi     = 0;
#define DIAG_FRAME_DUMP 1   // set to 0 to disable raw frame snapshots


#ifdef ESP32
// ---- Promiscuous callback --------------------------------------------------
// IRAM_ATTR: runs in ISR context.
// Serial.printf NOT safe here — use counters, print from loop().
void IRAM_ATTR promiscuous_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    promiscTotal++;

    if (type != WIFI_PKT_MGMT) return;
    promiscMgmt++;

    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = ppkt->payload;
    int frameLen = ppkt->rx_ctrl.sig_len;

    // Minimum: 24-byte 802.11 header + 1 byte category + 3 byte OUI = 28
    if (frameLen < 28) return;

    // Frame Control byte 0 = 0xD0 for action frame (type=0 mgmt, subtype=1101)
    if (frame[0] != 0xD0) return;
    promiscAction++;

    // Byte 24: category = 0x7F (vendor-specific action)
    if (frame[24] != 0x7F) return;
    promiscCategory++;

    // Bytes 25-27: Espressif OUI
    static const uint8_t ESPRESSIF_OUI[] = {0x18, 0xFE, 0x34};
    if (memcmp(&frame[25], ESPRESSIF_OUI, 3) != 0) return;
    promiscOui++;

    // addr2 = transmitter MAC = bytes 10-15
    updateRssiCache(&frame[10], (int8_t)ppkt->rx_ctrl.rssi);
}

// Second callback variant that also snapshots the raw frame bytes.
// Registered INSTEAD of promiscuous_rx_cb when DIAG_FRAME_DUMP is enabled.
void IRAM_ATTR promiscuous_rx_cb_full(void* buf, wifi_promiscuous_pkt_type_t type) {
    promiscTotal++;

    if (type != WIFI_PKT_MGMT) return;
    promiscMgmt++;

    const wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = ppkt->payload;
    int frameLen = (int)ppkt->rx_ctrl.sig_len;

    // Snapshot first MGMT frame for raw dump (only first time)
    if (!hasLastFrame && frameLen > 0) {
        int copyLen = (frameLen < 64) ? frameLen : 64;
        memcpy((void*)lastFrame, frame, copyLen);
        lastFrameLen = copyLen;
        lastRssi     = (int8_t)ppkt->rx_ctrl.rssi;
        hasLastFrame = true;
    }

    if (frameLen < 28) return;
    if (frame[0] != 0xD0) return;
    promiscAction++;

    if (frame[24] != 0x7F) return;
    promiscCategory++;

    static const uint8_t ESPRESSIF_OUI[] = {0x18, 0xFE, 0x34};
    if (memcmp(&frame[25], ESPRESSIF_OUI, 3) != 0) return;
    promiscOui++;

    updateRssiCache(&frame[10], (int8_t)ppkt->rx_ctrl.rssi);
}

// ---- enablePromiscuousRssi with full return-code logging ------------------
void enablePromiscuousRssi() {
    // Kill power save — WiFi.begin() resets this to WIFI_PS_MIN_MODEM
    esp_err_t rPS = esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };

    esp_err_t rCB  = esp_wifi_set_promiscuous_rx_cb(
                         DIAG_FRAME_DUMP ? promiscuous_rx_cb_full
                                        : promiscuous_rx_cb);
    esp_err_t rFLT = esp_wifi_set_promiscuous_filter(&filter);
    esp_err_t rEN  = esp_wifi_set_promiscuous(true);

    bool active = false;
    esp_wifi_get_promiscuous(&active);

    Serial.printf("\n[PROMISC-INIT] ps_none=%d  cb=%d  filter=%d  enable=%d\n",
                  rPS, rCB, rFLT, rEN);
    Serial.printf("[PROMISC-INIT] Active after enable: %s\n",
                  active ? "YES ✓" : "NO ✗ — THIS IS THE BUG");

    if (!active) {
        Serial.println("[PROMISC-INIT] ERROR: promiscuous mode refused.");
        Serial.println("[PROMISC-INIT] Likely cause: WiFi stack not in correct state.");
        Serial.println("[PROMISC-INIT] Trying esp_wifi_start() then re-enable...");
        esp_wifi_start();
        delay(100);
        rEN = esp_wifi_set_promiscuous(true);
        esp_wifi_get_promiscuous(&active);
        Serial.printf("[PROMISC-INIT] Retry enable=%d active=%s\n",
                      rEN, active ? "YES ✓" : "STILL NO ✗");
    }
}
#elif defined(ESP8266)

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

// ---- ESP8266 Promiscuous Callback ------------------------------------------
void ICACHE_FLASH_ATTR promiscuous_rx_cb_esp8266(uint8_t *buf, uint16_t len) {
    promiscTotal++;

    // On ESP8266, buf points to a sniffer_buf2 structure for management frames
    if (len < sizeof(struct RxControl)) return;
    
    struct sniffer_buf2 *sniffer = (struct sniffer_buf2 *)buf;
    uint8_t *frame = sniffer->buf;
    
    // We don't have a reliable type check filter in ESP8266 like WIFI_PKT_MGMT,
    // so we assume it's passed or manually filter by frame[0]
    promiscMgmt++;

    // Snapshot first MGMT frame for raw dump (only first time)
    if (!hasLastFrame) {
        int copyLen = (len < 64 + sizeof(struct RxControl)) ? (len - sizeof(struct RxControl)) : 64;
        if (copyLen > 0) {
            memcpy((void*)lastFrame, frame, copyLen);
            lastFrameLen = copyLen;
            lastRssi     = sniffer->rx_ctrl.rssi;
            hasLastFrame = true;
        }
    }

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
}

void enablePromiscuousRssi() {
    wifi_promiscuous_enable(0);
    wifi_set_promiscuous_rx_cb(promiscuous_rx_cb_esp8266);
    wifi_promiscuous_enable(1);
    Serial.println("[PROMISC-INIT] ESP8266 Promiscuous mode enabled.");
}
#endif

// ---- ESP-NOW receive callback (minimal) -----------------------------------
uint8_t myMac[6] = {0};

#ifdef ESP32
void OnDataRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    espnowRecvCount++;
}
#elif defined(ESP8266)
void OnDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
    espnowRecvCount++;
}
#endif

// ---- Self-stimulus: send a broadcast ESP-NOW packet -----------------------
// This gives the promiscuous sidecar something to intercept even without
// a second device present. ESP-NOW self-sends appear on the air as real
// 802.11 action frames.
uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct __attribute__((packed)) {
    uint8_t seq;
    uint8_t payload[4];
} DiagPacket;

void selfStimulus() {
    DiagPacket pkt;
    pkt.seq = (uint8_t)(selfTxCount & 0xFF);
    memset(pkt.payload, 0xAB, sizeof(pkt.payload));

#ifdef ESP32
    esp_err_t r = esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
#elif defined(ESP8266)
    int r = esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
#endif
    selfTxCount++;
    Serial.printf("[SELF-TX #%lu] seq=%d result=%d\n",
                  (unsigned long)selfTxCount, pkt.seq, r);
}

// ---- Counter dump ---------------------------------------------------------
void dumpCounters() {
    Serial.println("\n[TICK] ============ Promiscuous Counter Dump ============");
    Serial.printf("[TICK] promiscTotal    = %lu\n", (unsigned long)promiscTotal);
    Serial.printf("[TICK] promiscMgmt     = %lu  (type==WIFI_PKT_MGMT passed)\n",
                  (unsigned long)promiscMgmt);
    Serial.printf("[TICK] promiscAction   = %lu  (frame[0]==0xD0 passed)\n",
                  (unsigned long)promiscAction);
    Serial.printf("[TICK] promiscCategory = %lu  (frame[24]==0x7F passed)\n",
                  (unsigned long)promiscCategory);
    Serial.printf("[TICK] promiscOui      = %lu  (Espressif OUI passed)\n",
                  (unsigned long)promiscOui);
    Serial.printf("[TICK] promiscCached   = %lu  (RSSI written to cache)\n",
                  (unsigned long)promiscCached);
    Serial.printf("[TICK] espnowRecvCount = %lu  (OnDataRecv fired)\n",
                  (unsigned long)espnowRecvCount);
    Serial.printf("[TICK] selfTxCount     = %lu  (self-stimulus sends)\n",
                  (unsigned long)selfTxCount);
    Serial.printf("[TICK] rssiCacheIdx    = %d\n", rssiCacheIdx);

    // If any ESP-NOW received, do a lookup to test cache hit
    if (espnowRecvCount > 0) {
        // myMac is unlikely to be in cache, but shows the lookup path works
        int8_t r = lookupRssi(myMac);
        Serial.printf("[TICK] lookupRssi(myMac) = %d  (expected 0 unless self-tx cached)\n", r);
    }

    Serial.println("[TICK] =======================================================");
}

#ifdef ESP32
// ---- Raw frame dump (from loop — safe to Serial.printf) ------------------
void dumpLastFrame() {
    if (!hasLastFrame) {
        Serial.println("[FRAME] No MGMT frame captured yet.");
        return;
    }
    hasLastFrame = false;  // reset so next unique frame gets captured

    Serial.printf("[FRAME] First MGMT frame captured (%d bytes), RSSI=%d dBm\n",
                  lastFrameLen, (int)lastRssi);
    Serial.print("[FRAME] Raw hex: ");
    for (int i = 0; i < lastFrameLen && i < 64; i++) {
        Serial.printf("%02X ", (uint8_t)lastFrame[i]);
        if ((i + 1) % 16 == 0) Serial.print("\n               ");
    }
    Serial.println();

    if (lastFrameLen >= 28) {
        Serial.printf("[FRAME] frame[0]    = 0x%02X  (expect 0xD0 for action frame)\n",
                      (uint8_t)lastFrame[0]);
        Serial.printf("[FRAME] frame[1]    = 0x%02X\n", (uint8_t)lastFrame[1]);
        Serial.printf("[FRAME] addr1 [4-9] = %02X:%02X:%02X:%02X:%02X:%02X  (dst)\n",
                      lastFrame[4],  lastFrame[5],  lastFrame[6],
                      lastFrame[7],  lastFrame[8],  lastFrame[9]);
        Serial.printf("[FRAME] addr2[10-15]= %02X:%02X:%02X:%02X:%02X:%02X  (src)\n",
                      lastFrame[10], lastFrame[11], lastFrame[12],
                      lastFrame[13], lastFrame[14], lastFrame[15]);
        Serial.printf("[FRAME] addr3[16-21]= %02X:%02X:%02X:%02X:%02X:%02X  (bssid)\n",
                      lastFrame[16], lastFrame[17], lastFrame[18],
                      lastFrame[19], lastFrame[20], lastFrame[21]);
        Serial.printf("[FRAME] frame[24]   = 0x%02X  (expect 0x7F for vendor-specific)\n",
                      (uint8_t)lastFrame[24]);
        Serial.printf("[FRAME] frame[25-27]= %02X %02X %02X  (expect 18 FE 34 = Espressif)\n",
                      lastFrame[25], lastFrame[26], lastFrame[27]);
    } else {
        Serial.printf("[FRAME] Frame too short (%d bytes) — cannot decode offsets\n",
                      lastFrameLen);
    }
}
#elif defined(ESP8266)
void dumpLastFrame() {
    if (!hasLastFrame) {
        Serial.println("[FRAME] No MGMT frame captured yet.");
        return;
    }
    hasLastFrame = false;  

    Serial.printf("[FRAME] First MGMT frame captured (%d bytes), RSSI=%d dBm\n",
                  lastFrameLen, (int)lastRssi);
    Serial.print("[FRAME] Raw hex: ");
    for (int i = 0; i < lastFrameLen && i < 64; i++) {
        Serial.printf("%02X ", (uint8_t)lastFrame[i]);
        if ((i + 1) % 16 == 0) Serial.print("\n               ");
    }
    Serial.println();

    if (lastFrameLen >= 28) {
        Serial.printf("[FRAME] frame[0]    = 0x%02X  (expect 0xD0 for action frame)\n",
                      (uint8_t)lastFrame[0]);
        Serial.printf("[FRAME] addr2[10-15]= %02X:%02X:%02X:%02X:%02X:%02X  (src)\n",
                      lastFrame[10], lastFrame[11], lastFrame[12],
                      lastFrame[13], lastFrame[14], lastFrame[15]);
        Serial.printf("[FRAME] frame[24]   = 0x%02X  (expect 0x7F for vendor-specific)\n",
                      (uint8_t)lastFrame[24]);
        Serial.printf("[FRAME] frame[25-27]= %02X %02X %02X  (expect 18 FE 34 = Espressif)\n",
                      lastFrame[25], lastFrame[26], lastFrame[27]);
    } else {
        Serial.printf("[FRAME] Frame too short (%d bytes) — cannot decode offsets\n",
                      lastFrameLen);
    }
}
#endif

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== V.O.I.D. RSSI Promiscuous Diagnostic ===");
    Serial.println("    Flash, open Serial Monitor @ 115200");
    Serial.println("    Let run 60s minimum, then paste full output\n");

    // Step 1: MAC address
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.macAddress(myMac);
    Serial.printf("[SYS] My MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  myMac[0], myMac[1], myMac[2],
                  myMac[3], myMac[4], myMac[5]);

    // Step 2: ESP-NOW init
#ifdef ESP32
    if (esp_now_init() != ESP_OK) {
        Serial.println("[SYS] FATAL: esp_now_init() failed. Halting.");
        while (1) delay(1000);
    }
    Serial.println("[SYS] ESP-NOW init OK");

    // Register broadcast peer (unencrypted, for self-stimulus)
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, broadcastMac, 6);
    peer.channel  = 0;   // use current channel
    peer.ifidx    = WIFI_IF_STA;
    peer.encrypt  = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[SYS] WARNING: Failed to add broadcast peer.");
    } else {
        Serial.println("[SYS] Broadcast peer registered for self-stimulus.");
    }
#elif defined(ESP8266)
    if (esp_now_init() != 0) {
        Serial.println("[SYS] FATAL: esp_now_init() failed. Halting.");
        while (1) delay(1000);
    }
    Serial.println("[SYS] ESP-NOW init OK");

    if (esp_now_add_peer(broadcastMac, ESP_NOW_ROLE_COMBO, 0, NULL, 0) != 0) {
        Serial.println("[SYS] WARNING: Failed to add broadcast peer.");
    } else {
        Serial.println("[SYS] Broadcast peer registered for self-stimulus.");
    }
#endif

    esp_now_register_recv_cb(OnDataRecv);
    Serial.println("[SYS] ESP-NOW recv callback registered");

    // Step 3: Enable promiscuous RSSI sidecar
    enablePromiscuousRssi();

    // Step 4: Optional WiFi connect (tests that re-enable after begin() works)
    if (strlen(DIAG_WIFI_SSID) > 0) {
        Serial.printf("\n[WIFI] Connecting to '%s'...\n", DIAG_WIFI_SSID);
        WiFi.begin(DIAG_WIFI_SSID, DIAG_WIFI_PASS);
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 30) {
            delay(300);
            Serial.print(".");
            tries++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WIFI] Connected. IP=%s Channel=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.channel());
        } else {
            Serial.println("\n[WIFI] Not connected (OK — sidecar test still valid).");
        }

        // CRITICAL: Re-enable after WiFi.begin() — this is the known failure mode
        Serial.println("[WIFI] Re-enabling promiscuous after WiFi.begin()...");
        enablePromiscuousRssi();
    } else {
        Serial.println("[WIFI] No SSID configured — skipping WiFi connect.");
        Serial.println("[WIFI] Sidecar test proceeds on ESP-NOW channel only.");
    }

    // Initial self-stimulus
    selfStimulus();

    Serial.println("\n[SYS] Diagnostic running. Counters dumped every 5s. Frame dump after first MGMT capture.\n");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    static unsigned long lastTickMs      = 0;
    static unsigned long lastStimulusMs  = 0;
    static unsigned long lastFrameDumpMs = 0;
    static bool          firstFrameShown = false;

    uint32_t now = millis();

    // Every 5s: dump counters
    if (now - lastTickMs > 5000) {
        lastTickMs = now;
        dumpCounters();
    }

    // Every 10s: send self-stimulus ESP-NOW packet
    if (now - lastStimulusMs > 10000) {
        lastStimulusMs = now;
        selfStimulus();
    }

    // As soon as a MGMT frame arrives: dump its raw bytes once
    // (hasLastFrame set in ISR, safe to read and print from loop)
    if (hasLastFrame && !firstFrameShown) {
        firstFrameShown = true;
        dumpLastFrame();
    }
    // Also allow re-dump every 30s for subsequent frames
    if (hasLastFrame && now - lastFrameDumpMs > 30000) {
        lastFrameDumpMs = now;
        firstFrameShown = false;  // allow one more dump
    }

    delay(50);
}
