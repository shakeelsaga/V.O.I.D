# V.O.I.D. - Vital Offline Information Dispatch

> A delay-tolerant, multi-hop ESP-NOW mesh network for disaster-scenario survivor telemetry,  
> with hop-distance vector routing, highly available gateway failover, and cloud-native observability.

**Current stable release:** `v2.4.0` &nbsp;|&nbsp; Cloud-native Helm chart deployment, Hop-distance vector routing, passive RSSI peer scoring (ESP32)  
**Hardware targets:** ESP32 (Arduino Core v3.x) · ESP8266  
**Upstream stack:** Mosquitto MQTT → Telegraf → InfluxDB → Grafana on K3s  
**Deployment:** [Kubernetes Helm Chart included (`deploy/void-observability/`)](deploy/void-observability/README.md)  
**License:** MIT

---

## What This Is

V.O.I.D. is a firmware and infrastructure system built around one specific failure scenario: **the communication grid is down**. Cellular towers are destroyed or saturated. Wi-Fi infrastructure is offline. Survivor nodes - carried by people or placed at fixed locations in a disaster zone - need to continuously report their status: battery level, CPU health, SOS distress signals. That data needs to reach a command centre even when no reliable end-to-end network path exists, and even when the gateway itself fails.

The system solves this with four interlocking mechanisms:

1. **Zero-touch provisioning.** The gateway injects its own MAC address into its Wi-Fi beacon SSID. Edge nodes scan and self-configure at boot - no hardcoded addresses, no manual setup.
2. **A self-forming multi-hop ESP-NOW mesh.** Edge nodes relay each other's packets when direct gateway range is unavailable. A passive hop-distance vector tells every node which direction the gateway is, without any probe traffic.
3. **A delay-tolerant buffer.** If the upstream MQTT connection drops, the gateway writes payloads to onboard flash with delta filtering and mid-flush integrity protection. When connectivity restores, the buffer flushes with zero duplicates and zero data loss.
4. **A highly available gateway pair.** Two gateway nodes run an autonomous Active-Passive election. If the primary goes down, the shadow self-promotes within 7 seconds - no human intervention required.

Every subsystem in this repository has been built for and tested on physical hardware. The system is designed to handle real-world failure conditions, including mid-flush network disconnections, router channel reassignments, ESP-NOW state loss during Wi-Fi mode transitions, and hardware-specific constraints like dual-core race conditions and ISR stack limits. It has been debugged and refined at the protocol level to ensure stability in constrained environments.

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [System Components](#system-components)
  - [Edge Node](#edge-node-edge_nodesedge_nodesino)
  - [Gateway Node](#gateway-node-gateway_nodegateway_nodeino)
- [Protocol Reference](#protocol-reference)
  - [SurvivorPayload Wire Format](#survivorpayload-wire-format-v230)
  - [Encryption Model](#encryption-model)
  - [ACK Handshake](#ack-handshake)
  - [Lighthouse Provisioning](#lighthouse-zero-touch-provisioning)
  - [Hop-Distance Vector Routing](#hop-distance-vector-routing-v230)
  - [HA Election Protocol](#ha-election-protocol)
- [Hardware Requirements](#hardware-requirements)
- [Project Setup](#project-setup)
- [Configuration Reference](#configuration-reference)
- [Observability Stack](#observability-stack)
- [Version History](#version-history)
- [Known Limitations](#known-limitations)
- [Roadmap](#roadmap)

---

## Architecture Overview

```
╔═════════════════════════════════════════════════════════════════════════════════════════════╗
║                          DISASTER ZONE                                                      ║
║                                                                                             ║
║  [Node C] ── ESP-NOW ──► [Node B] ── ESP-NOW ──► [Node A]                                   ║
║  out of GW range          repeater                direct range                              ║
║  hopDist = 3              hopDist = 2             hopDist = 1                               ║
║                                │                       │                                    ║
║                         ESP-NOW relay           ESP-NOW unicast                             ║
║                         (encrypted)             (encrypted)                                 ║
║                                └───────────────────────┘                                    ║
║                                                │                                            ║
║                              ┌─────────────────▼──────────────────┐                         ║
║                              │         PRIMARY GATEWAY            │                         ║
║                              │  SSID: VOID_<STAMAC>               │                         ║
║                              │  Mode: WIFI_AP_STA                 │                         ║
║                              │  HA:   STATE_PRIMARY               │                         ║
║                              │  DTN:  LittleFS flash buffer       │                         ║
║                              └─────────────────┬──────────────────┘                         ║
║                                                │  HA heartbeat                              ║
║                              ┌─────────────────▼──────────────────┐                         ║
║                              │         SHADOW GATEWAY             │                         ║
║                              │  Mode: WIFI_STA only               │                         ║
║                              │  HA:   STATE_SHADOW                │                         ║
║                              │  Failover: 6s heartbeat timeout    │                         ║
║                              └────────────────────────────────────┘                         ║
╚═════════════════════════════════════════════════╦═══════════════════════════════════════════╝
                                                  ║  upstream Wi-Fi / LAN
                                                  ▼
                                      ┌───────────────────────┐
                                      │  Mosquitto MQTT :1883 │
                                      └───────────┬───────────┘
                                                  │
                                      ┌───────────▼───────────┐
                                      │  Telegraf → InfluxDB  │
                                      │  Grafana on K3s       │
                                      │  (Command Centre)     │
                                      └───────────────────────┘
```

**Data flow in one sentence:** Edge nodes send 11-byte encrypted `SurvivorPayload` structs via ESP-NOW unicast → the primary gateway ACKs immediately, enqueues, and publishes to MQTT with QoS 1 → if MQTT is unreachable, payloads are written to LittleFS flash and flushed on reconnect → if the primary gateway fails, the shadow detects heartbeat timeout and self-promotes within the election window.

---

## System Components

### Edge Node (`edge_nodes/edge_nodes.ino`)

An autonomous telemetry agent. Requires no static network configuration - discovers the gateway at runtime, adapts to topology changes, and relays packets for other nodes.

#### Boot Sequence

```
1. Read hardware MAC → derive node ID (last byte of MAC)
2. Scan for SSID matching "VOID_*" → extract gateway MAC + Wi-Fi channel
3. Register gateway as encrypted ESP-NOW peer on discovered channel
4. Register all authorizedEdgeNodes as encrypted mesh peers
5. Connect home Wi-Fi for UDP remote logging (non-blocking, best-effort)
6. If gateway not found → immediate boot probe into mesh peers
7. Begin adaptive telemetry loop
```

#### Adaptive RBE Telemetry

The node does not transmit on a fixed interval. It transmits on exception:

| Trigger | Condition | Priority |
|---|---|---|
| SOS burst | `isSosActive == true` | Immediate, bypasses all queuing |
| Battery drop | `lastTxBattery > current + 4%` | High |
| CPU spike | `abs(lastTxCpu - current) > 30` | High |
| Heartbeat | 80 seconds elapsed since last TX | Low (fallback only) |

Under stable conditions a node may transmit as rarely as once per minute, conserving both radio bandwidth and battery.

#### Routing Fallback Chain

```
routePayload()
│
├── gatewayConnected → transmitPayload(gatewayMac)
│       └── ACK received → done. myHopDist = 1.
│
└── gateway miss → huntForPeers()
        │
        ├── Step 1: neighbor table fast-path (V2.3+)
        │     bestNeighbor() → highest score above RSSI_FLOOR
        │     score = rssi + (MAX_HOPS − hopDist) × HOPDIST_WEIGHT
        │     ACK received → done. myHopDist = peer.hopDist + 1.
        │
        ├── Step 2: cached preferredPeer (fast path, no sweep)
        │     ACK received → done.
        │
        ├── Step 3: full channel sweep {11, 6, 1} × authorizedEdgeNodes
        │     400 ms per peer per channel
        │     ACK received → cache as preferredPeer, done.
        │
        └── Step 4: 10s extended retry on lastKnownChannel
              processRepeatPayload() called between rounds (non-blocking)
              ACK received → cache as preferredPeer, done.
              All miss → enqueue to edgeQueue RAM buffer (20 slots)
```

#### Repeater Mode

Every edge node passively listens for `SurvivorPayload` packets from other nodes. On receipt:

1. TTL check - drop if `ttl == 0` or `nodeId == myNodeId`.
2. **ACK immediately** back to the upstream sender - stops their retry burst. ACKs are sent **before** duplicate suppression to prevent a fatal lockout: rebooted nodes reuse sequence numbers, so a stale `dupCache` entry would suppress the ACK and leave the sender retrying forever.
3. Update neighbor table with sender's MAC, RSSI (via promiscuous sidecar on ESP32, `0` sentinel on ESP8266), and `hopDist`.
4. Duplicate suppression - drop if `(nodeId, sequence)` pair is in the 16-entry circular `dupCache`.
5. Decrement TTL and forward: SOS → immediate gateway attempt → queue on failure; non-SOS → enqueue for background flush.

All relay logic runs in `processRepeatPayload()`, called from `loop()` - never from the ISR callback. On ESP8266, `OnDataRecv` runs in the SDK Wi-Fi context with a very small stack (~512 bytes). Calling `String`, `Serial`, `delay`, or `esp_now_send` from within it causes an immediate stack overflow (Exception 9).

---

### Gateway Node (`gateway_node/gateway_node.ino`)

The bridge between the ESP-NOW mesh and the upstream IP network. Runs a three-state HA state machine and a Delay-Tolerant Networking buffer.

#### HA State Machine

| State | Wi-Fi Mode | ESP-NOW | MQTT | Lighthouse SSID |
|---|---|---|---|---|
| `STATE_ELECTION` | `WIFI_STA` | Broadcast pings | Disconnected | Off |
| `STATE_PRIMARY` | `WIFI_AP_STA` | Full mesh + ACK | Connected | `VOID_<STAMAC>` active |
| `STATE_SHADOW` | `WIFI_STA` | Heartbeat listen | Disconnected | Off |

#### DTN Pipeline

When MQTT is unavailable, the gateway applies a **delta filter** before buffering. Only state-changing packets are written to flash:

- Node seen for the first time
- SOS transitions from inactive → active
- Battery swings by more than 5% in either direction

Redundant telemetry is discarded to protect flash write endurance. On reconnect, `flushFlashBuffer()` publishes lines one by one with QoS 1, tracks the last published index, and rewrites the file with only the unpublished remainder. Lines already published are never replayed.

---

## Protocol Reference

### SurvivorPayload Wire Format (V2.3)

```c
typedef struct __attribute__((packed)) SurvivorPayload {
    uint8_t  nodeId;       // offset 0  - last byte of hardware MAC
    uint8_t  batteryPct;   // offset 1  - 0–100
    uint8_t  cpuLoad;      // offset 2  - 0–100
    bool     isSosActive;  // offset 3  - emergency distress flag
    uint32_t uptimeMs;     // offset 4  - millis() at TX time
                           //             (4-byte aligned - do not reorder)
    uint8_t  sequence;     // offset 8  - rolling counter, per-node
    uint8_t  ttl;          // offset 9  - decremented by each relay hop
    uint8_t  hopDist;      // offset 10 - sender's hop distance to gateway
} SurvivorPayload;         // sizeof = 11 bytes
```

> ⚠️ **Wire-format contract:** This struct must be **byte-identical** in both `edge_nodes.ino` and `gateway_node.ino`. The gateway's `OnDataRecv` classifies packets using `len == sizeof(SurvivorPayload)`. A struct mismatch causes all edge payloads to be silently misclassified as HA messages and dropped. Any field addition is a **breaking change** requiring simultaneous reflash of all nodes.

#### AckPayload

```c
typedef struct __attribute__((packed)) AckPayload {
    uint8_t msgType;   // always 0xA1
    uint8_t nodeId;    // echoes the nodeId being acknowledged
    uint8_t sequence;  // echoes the sequence number being acknowledged
} AckPayload;          // sizeof = 3 bytes
```

#### HaPayload (gateway-to-gateway only)

```c
typedef struct __attribute__((packed)) HaPayload {
    uint8_t  msgType;        // 0 = election ping, 1 = heartbeat
    uint8_t  mac[6];         // sender's STA MAC
    uint32_t senderUptimeMs; // millis() - used as uptime tiebreaker
    uint8_t  _pad;           // Disambiguator to ensure sizeof != sizeof(SurvivorPayload)
} HaPayload;                 // sizeof = 12 bytes
```

---

### Encryption Model

```
PMK (Primary Master Key) - 16 bytes
  └── Encrypts the LMK during peer registration.
      Shared across all nodes in the mesh.
      ESP32: esp_now_set_pmk()  |  ESP8266: esp_now_set_kok()

LMK (Local Master Key) - 16 bytes
  └── AES-128 CCM payload encryption on each peer-to-peer link.
      Applied per registered peer.
      ESP32: peerInfo.lmk  |  ESP8266: esp_now_add_peer(..., LMK_KEY, 16)
```

| Traffic type | Encrypted |
|---|---|
| `SurvivorPayload` unicast (edge → gateway, edge → relay) | ✅ Yes |
| `AckPayload` unicast (gateway → edge, relay → edge) | ✅ Yes - critical: relay ACKs must use LMK, not plaintext |
| `HaPayload` broadcast (election pings, heartbeats) | ❌ No - must be receivable by unregistered peer gateways |

---

### ACK Handshake

```
Edge Node                              Gateway (PRIMARY)
    │                                        │
    │─── SurvivorPayload (unicast) ─────────►│
    │                                        │ OnDataRecv ISR:
    │◄── AckPayload (fast, from ISR) ────────│   enqueue + send ACK
    │    ackReceived = true                  │
    │    burst timer stops                   │ processPrimaryTasks():
    │◄── AckPayload (verified, from loop) ───│   send confirmed ACK
    │                                        │   publish to MQTT
```

The **ISR ACK** stops the edge retry burst immediately. The **main-loop ACK** covers cases where the radio dropped the ISR ACK. Double-ACK is idempotent - the edge ignores any ACK whose `sequence != awaitedAckSequence`.

**ACK in non-PRIMARY states:** A shadow or electing gateway still sends an immediate ISR ACK but does not enqueue the payload. This stops the edge burst timer (saves 3 seconds of radio time and avoids a full mesh escalation) while correctly deferring data processing to the primary.

---

### Lighthouse Zero-Touch Provisioning

```
Gateway → STATE_PRIMARY
  └── WiFi.softAP("VOID_AABBCCDDEEFF", channel = <router_channel>)
      SSID encodes the gateway STA MAC as 12 uppercase hex chars.

Edge Node → boot
  └── WiFi.scanNetworks()
       └── for each result, if SSID.startsWith("VOID_"):
              targetChannel   ← WiFi.channel(i)
              discoveredBSSID ← WiFi.BSSID(i)       // AP MAC
              extractedSTA    ← SSID.substring(5)   // STA MAC (12 hex chars)
              WiFi.setChannel(targetChannel)
              esp_now_add_peer(gatewayMac, LMK_KEY, targetChannel)
              gatewayConnected = true
```

No hardcoded gateway address. No hardcoded channel. If the gateway is offline at boot, the edge falls back to `lastKnownChannel` (RAM-cached) and probes mesh peers immediately.

---

### Hop-Distance Vector Routing (V2.3)

A passive distributed Bellman-Ford mechanism. The gateway is the implicit root with `hopDist = 0`. Every packet carries the sender's current estimated distance to the gateway. Nodes learn their neighbours' distances from regular traffic - no probe packets are ever generated.

#### How distance propagates

```
Gateway root                 → hopDist = 0  (implicit)
Node A, direct GW link       → myHopDist = 1 (set on first confirmed direct TX)
Node B, relays through A     → hears A's packets with hopDist=1
                               → records A as neighbor with hopDist=1
                               → after relay TX confirmed: myHopDist = A.hopDist + 1 = 2
Node C, relays through B     → myHopDist = 3
```

#### Neighbor table (per edge node)

```c
struct Neighbor {
    uint8_t  mac[6];
    int8_t   rssi;         // last known signal strength (dBm)
    uint8_t  hopDist;      // their reported distance to gateway
    uint32_t lastSeenMs;   // expires after NEIGHBOR_EXPIRY_MS (2 minutes)
    bool     valid;
};
Neighbor neighborTable[NEIGHBOR_TABLE_SIZE]; // 5 entries
```

#### Peer scoring

```
score = rssi + (MAX_HOPS − hopDist) × HOPDIST_WEIGHT

RSSI_FLOOR = −80 dBm  → peers below this are disqualified entirely
```

Example: Peer A at −65 dBm, hopDist=1: score = −65 + (10−1)×10 = **+25**  
Example: Peer B at −55 dBm, hopDist=4: score = −55 + (10−4)×10 = **+5**  
Peer A is selected despite weaker signal - it is much closer to the gateway.

`HOPDIST_UNKNOWN (255)` scores as −9999 and is never selected. It appears as `"hops": 255` in MQTT - filterable in Grafana.

---

### HA Election Protocol

```
Both gateways boot → STATE_ELECTION
  Broadcast HA_MSG_ELECTION_PING every 500 ms
  Random window: 3500–4500 ms

  If peer heartbeat heard during window  →  demoteToShadow()  [Incumbent Rule]
  If window expires uncontested          →  promoteToPrimary()
    ├── WiFi.mode(WIFI_AP_STA)
    ├── softAP("VOID_<MAC>", meshChannel)
    ├── configureEspNow(meshChannel)      ← full ESP-NOW teardown + rebuild
    ├── sendHaMessage(HA_MSG_HEARTBEAT)   ← collapses late-joiner elections
    └── reconnectMqtt()

Shadow monitors heartbeat every 6000 ms
  Timeout → startElection()
```

**Tiebreak order (preemption disabled):**
1. Uptime delta - node alive longer wins if delta > 500 ms
2. MAC address - lexicographically lower MAC wins on tie

**ESP-NOW state preservation:** Every Wi-Fi mode transition calls `configureEspNow(channel)`, which fully rebuilds the ESP-NOW peer registry. ESP32 loses all peer state when switching between `WIFI_STA` and `WIFI_AP_STA` - rebuilding is required, not optional.

---

## Hardware Requirements

### Minimum - 1 gateway + 1 edge node

| Role | Module | Notes |
|---|---|---|
| Gateway | ESP32 (any variant) | AP+STA dual-mode required. ESP32-WROOM-32 verified. |
| Edge Node | ESP32 or ESP8266 | NodeMCU ESP8266 and ESP32-DevKitC both verified. |

### Recommended - full HA + multi-hop setup

| Qty | Role | Module |
|---|---|---|
| 2 | Gateway (primary + shadow) | ESP32 - must share the same upstream Wi-Fi network |
| 2+ | Edge Node | ESP32 or ESP8266 - one per survivor location |
| 1 | MQTT broker host | Any Linux host on the LAN (Raspberry Pi, VM, K3s node) |

### Power note

Wi-Fi sleep modes are explicitly disabled on all devices (`WIFI_NONE_SLEEP` / `WiFi.setSleep(false)`). This is required - sleep mode interactions with ESP-NOW cause intermittent receive failures on both architectures. Plan for continuous active draw:

| Module | Typical continuous draw |
|---|---|
| ESP32 (Wi-Fi active, no sleep) | 80–160 mA |
| ESP8266 (Wi-Fi active, no sleep) | 70–100 mA |

---

## Project Setup

> **Note on Infrastructure:** V.O.I.D. requires an upstream MQTT broker and time-series database. If you have a Kubernetes cluster (like K3s), you can skip manual infrastructure setup and deploy the entire stack in one command using the included Helm chart. See the [**Deployment Guide**](deploy/void-observability/README.md).

### Prerequisites

- Arduino IDE 2.x or PlatformIO
- ESP32 Arduino Core v3.x (`arduino-esp32` ≥ 3.0.0)
- ESP8266 Arduino Core 3.x (`esp8266` ≥ 3.1.0)
- [256dpi/arduino-mqtt](https://github.com/256dpi/arduino-mqtt) library (gateway only)
- Mosquitto MQTT broker reachable on your LAN

### Step 1 - Clone

```bash
git clone https://github.com/shakeelsaga/V.O.I.D.git
cd V.O.I.D
```

### Step 2 - Create secrets files

```bash
cp gateway_node/example_secrets.h gateway_node/secrets.h
cp edge_nodes/example_secrets.h   edge_nodes/secrets.h
```

Edit both files:

```c
#define SECRET_WIFI_SSID      "your_router_ssid"
#define SECRET_WIFI_PASS      "your_router_password"
#define SECRET_MQTT_BROKER_IP "192.168.x.x"    // gateway only

// Generate with: python3 -c "import os; print(', '.join(f'0x{b:02X}' for b in os.urandom(16)))"
// PMK and LMK must be identical across ALL nodes in the mesh.
// DO NOT use the example values from the repository.
static const uint8_t PMK_KEY[16] = { 0x??, ... };
static const uint8_t LMK_KEY[16] = { 0x??, ... };
```

### Step 3 - Create node registry files

```bash
cp gateway_node/example_node_registry.h gateway_node/node_registry.h
cp edge_nodes/example_node_registry.h   edge_nodes/node_registry.h
```

Edit both with your physical edge node MACs:

```c
static const uint8_t authorizedEdgeNodes[][6] = {
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},  // Node 1
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},  // Node 2
};
```

To read a node's MAC, flash it and open Serial Monitor at 115200 baud:
```
[SYS] Raw Hardware MAC: AA:BB:CC:DD:EE:FF
[SYS] Hardware-Derived Node ID: 255
```

### Step 4 - Flash

**Gateway nodes** - flash both identically, they self-elect:
- Open `gateway_node/gateway_node.ino`, select ESP32 board, upload.

**Edge nodes:**
- Open `edge_nodes/edge_nodes.ino`, select ESP32 or ESP8266 board, upload.

> All nodes in the mesh must run the **same firmware version** simultaneously. `SurvivorPayload` is a packed binary struct matched by `sizeof()` - a version mismatch causes silent packet misclassification.

### Step 5 - Verify boot output

Open Serial Monitor at **115200 baud** on each device.

**Primary gateway:**
```
--- V.O.I.D. Gateway Initializing (V2.3) ---
[SYS] Flash Storage Mounted.
[SYS] Gateway STA MAC: AA:BB:CC:DD:EE:FF
[SYS] Upstream Assigned Channel: 6
[HA] Entering ELECTION mode for 3847ms...
[HA] *** CROWNED PRIMARY GATEWAY ***
[SYS] Lighthouse Beacon Active: VOID_AABBCCDDEEFF
[MQTT] Attempting connection... Established.
```

**Shadow gateway:**
```
[HA] Entering ELECTION mode for 4201ms...
[HA] Active Primary detected. Yielding election.
```

**Edge node - direct gateway link:**
```
--- V.O.I.D. Edge Node Booting (V2.3) ---
[SYS] Hardware-Derived Node ID: 255
[SYS] Gateway acquired on Channel 6 | AP MAC: ... | STA MAC: ...
[HDV] Direct gateway link confirmed. myHopDist=1
[SYS] Node Armed. V2.3 Adaptive Telemetry starting.
```

**Edge node - relay path:**
```
[SYS] WARNING: Gateway Lighthouse not found. Falling back to Mesh Channel 6
[MESH] NBR table: trying best peer AABBCCDDEEFF score=25 hopDist=1 RSSI=-65dBm
[MESH] NBR table peer ACK'd.
[HDV] myHopDist updated to 2 via relay AABBCCDDEEFF
```

---

## Configuration Reference

### Edge Node (`edge_nodes.ino`)

| Constant | Default | Description |
|---|---|---|
| `ACK_WAIT_MS` | `750` | Milliseconds to wait for ACK per burst attempt |
| `TX_BURST_COUNT` | `4` | Retransmit attempts before declaring delivery failed |
| `MESH_TOTAL_TIMEOUT_MS` | `10000` | Total extended retry window across all peers |
| `MESH_PER_PEER_WAIT_MS` | `400` | Per-peer ACK wait during channel sweep |
| `MESH_CHANNELS[]` | `{11, 6, 1}` | Wi-Fi channels swept during peer discovery |
| `DUP_CACHE_SIZE` | `16` | Duplicate suppression circular buffer depth |
| `EDGE_QUEUE_SIZE` | `20` | RAM survival queue capacity (payloads) |
| `RSSI_FLOOR` | `-80` | dBm - peers below this are disqualified as relay candidates |
| `MAX_HOPS` | `10` | Maximum plausible hop count (used in peer score formula) |
| `HOPDIST_WEIGHT` | `10` | Score bonus per hop closer to gateway |
| `HOPDIST_UNKNOWN` | `255` | Sentinel - node has not yet confirmed a path to gateway |
| `NEIGHBOR_TABLE_SIZE` | `5` | Passive neighbor table capacity |
| `NEIGHBOR_EXPIRY_MS` | `120000` | Milliseconds before a neighbor entry is evicted as stale |
| `NETLOG_PORT` | `4210` | UDP port for remote serial logging |

### Gateway (`gateway_node.ino`)

| Constant | Default | Description |
|---|---|---|
| `HA_ELECTION_PING_INTERVAL_MS` | `500` | Frequency of election broadcast pings |
| `HA_HEARTBEAT_INTERVAL_MS` | `1000` | Primary heartbeat broadcast cadence |
| `HA_HEARTBEAT_TIMEOUT_MS` | `6000` | Shadow failover trigger - time since last heartbeat |
| `HA_ELECTION_WINDOW_MIN_MS` | `3500` | Minimum randomised election window |
| `HA_ELECTION_WINDOW_MAX_MS` | `4500` | Maximum randomised election window |
| `HA_UPTIME_TIE_MARGIN_MS` | `500` | Uptime delta margin before MAC tiebreak applies |
| `HA_PREEMPTION_ENABLED` | `false` | If true, higher-MAC node displaces incumbent primary |
| `QUEUE_SIZE` | `50` | ESP-NOW hardware RX queue depth (ISR-safe circular buffer) |
| `RAM_BUFFER_SIZE` | `50` | DTN RAM buffer capacity before flash flush |
| `MAX_NODES` | `256` | Maximum trackable node IDs |

---

## Observability Stack

![V.O.I.D. Dashboard 2.0](assets/V.O.I.D%20Dashboard%202.0.png)

Sur> **Note:** A complete Kubernetes Helm chart to deploy this stack is now included. The "Dashboard 2.0" configuration shown above comes pre-loaded and built-in automatically. See [`deploy/void-observability/README.md`](deploy/void-observability/README.md) for full deployment instructions.

### MQTT payload (V2.3)

Published to topic `void/telemetry`:

```json
{
  "node_id":    255,
  "battery":    87,
  "cpu":        14,
  "sos_alert":  0,
  "seq":        42,
  "uptime_ms":  183920,
  "hops":       2
}
```

`hops` values: `1` = direct delivery · `2+` = relay path · `255` = distance not yet confirmed

### Telegraf configuration

```toml
[[inputs.mqtt_consumer]]
  servers     = ["tcp://localhost:1883"]
  topics      = ["void/telemetry"]
  data_format = "json"
  tag_keys    = ["node_id"]
```

### Recommended Grafana panels

| Panel type | Field | Purpose |
|---|---|---|
| Gauge | `battery` grouped by `node_id` | Per-node battery level |
| Stat + threshold | `sos_alert` last 30s max, threshold > 0 → red | Emergency state |
| Time series | `hops` grouped by `node_id` | Real-time topology depth |
| Time series | `battery` grouped by `node_id` | Battery drain over time |
| Bar gauge | `cpu` grouped by `node_id` | CPU load comparison |
| Stat | `count(seq)` per 1 min | Message throughput |

---

## Version History

See [CHANGELOG.md](CHANGELOG.md) for full per-commit detail.

| Version | Date | Summary |
|---|---|---|
| `v2.3.0` | 2026-05-11 | Hop-distance vector routing, probabilistic gossip gate, passive neighbor table, RSSI-composite peer scoring (ESP32), ESP32 promiscuous RSSI sidecar, ESP8266 baseband limitation workaround, ACK encryption fix, duplicate suppression lockout fix, `HaPayload` `_pad` byte disambiguator, `hops` in MQTT output, raw experiment data + diagnostics shipped in-repo |
| `v2.2.0` | 2026-04-26 | 12 critical bug fixes: mesh ACK miss, ESP32 race condition, WDT overflow, MQTT keepalive starvation, mid-flush duplicate protection, shadow ACK, RAM overflow guard |
| `v2.0.0` | 2026-04-25 | HA Active-Passive gateway election, multi-hop relay routing, encrypted unicast 2-way handshake, `ttl` and `uptimeMs` in payload |
| `v1.1.0` | 2026-04-10 | E2E ESP-NOW encryption (PMK/LMK), adaptive RBE telemetry, dynamic hardware node IDs, central node registry, 256dpi MQTT library |
| `v1.0.0` | 2026-03-18 | Lighthouse zero-touch provisioning, LittleFS DTN pipeline, hardware-agnostic dual-target firmware, Grafana/InfluxDB/Telegraf/K3s stack |

> **Release note:** Formal GitHub releases begin at `v2.2.0`. Prior versions are represented by dated commits and are documented in CHANGELOG.md for research reproducibility.

---

## Known Limitations

Documented for research reproducibility and academic honesty.

- **Simulated sensor metrics.** `batteryPct` and `cpuLoad` are generated by a software simulation loop. The routing, buffering, and observability pipeline is fully real - the sensor inputs are synthetic. Physical sensor binding is planned for v3.0.
- **Static node registry.** Authorised nodes must be compiled into `node_registry.h`. Dynamic onboarding without reflashing is not supported.
- **Single Wi-Fi channel constraint.** ESP-NOW and the gateway AP must share the same channel as the upstream router. Channel reassignment by the router breaks all ESP-NOW links until nodes re-scan. The firmware detects and corrects this but there is a disruption window.
- **Indoor testing only.** All experiments conducted indoors at ~5 m inter-node distance. Outdoor RF propagation and interference were not characterised.
- **No formal security audit.** The PMK/LMK architecture follows Espressif's ESP-NOW security model. No formal cryptographic analysis against a defined threat model has been performed.
- **Scalability untested beyond 3 nodes.** Logic supports up to 256 node IDs, but practical testing used 2 edge nodes and 1 active gateway (3 physical devices total).
- **ESP8266 promiscuous mode incompatible with ESP-NOW.** Hardware testing confirmed that enabling promiscuous mode on ESP8266 intercepts all packets at the baseband level, completely severing ESP-NOW receive callbacks and crashing the Station interface. The RSSI sidecar is therefore disabled on ESP8266 (`#if 0` guarded). Neighbor table entries from ESP8266 nodes carry `rssi = 0` (a safe sentinel above `RSSI_FLOOR`), causing peer scoring to degrade gracefully to pure `hopDist` ordering. ESP32 is unaffected - promiscuous mode coexists with ESP-NOW.

---

## Citation

If you use V.O.I.D. in academic work, please cite:

```bibtex
@misc{void2026,
  author       = {Mohammed Shakeel},
  title        = {{V.O.I.D.}: Vital Offline Information Dispatch ---
                  A Delay-Tolerant Multi-Hop {ESP-NOW} Mesh for
                  Disaster-Scenario Survivor Telemetry},
  year         = {2026},
  howpublished = {\url{https://github.com/shakeelsaga/V.O.I.D}},
  note         = {Version 2.3.0}
}
```

*A preprint describing the system architecture and experimental evaluation is available. Raw experiment logs and processed data are included in `V.O.I.D_Experiments/`.*

---

## License

MIT License. See [LICENSE](LICENSE) for full text.