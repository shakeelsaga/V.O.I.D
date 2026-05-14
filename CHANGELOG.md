# Changelog

All notable changes to V.O.I.D. are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).  
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> **Breaking changes** are marked with ⚠️ — they require simultaneous reflash of all nodes in the mesh, because `SurvivorPayload` is a packed binary struct matched by `sizeof()` on both ends. Any field addition or reordering is a wire-format break.

---

## [v2.3.0] - 2026-05-11

### Added - Hop-Distance Vector Routing

- **`hopDist` field in `SurvivorPayload`** ⚠️ Breaking - struct grows from 10 to 11 bytes. All nodes must be reflashed simultaneously.
  - Offset 10, `uint8_t`. Carries the originating node's estimated distance to the gateway in relay hops.
  - Gateway is the implicit root (`hopDist = 0`). Direct edge nodes report `hopDist = 1`. Each relay increments before forwarding.
  - Implements a passive distributed Bellman-Ford: no probe packets, no topology broadcasts. Routing data is piggybacked on regular telemetry.

- **5-entry passive neighbor table** in `edge_nodes.ino`
  - `struct Neighbor { mac[6], rssi, hopDist, lastSeenMs, valid }` - populated entirely from received packets inside `processRepeatPayload()`.
  - Entries expire after 120 seconds (`NEIGHBOR_EXPIRY_MS`). Stale entries are evicted on next write.
  - Zero probe overhead - the table builds itself from traffic that already exists.

- **RSSI-composite peer scoring**
  - Formula: `score = rssi + (MAX_HOPS − hopDist) × HOPDIST_WEIGHT`
  - Higher RSSI (stronger signal) and lower `hopDist` (closer to gateway) both increase score.
  - `RSSI_FLOOR = −80 dBm` - peers below this threshold are disqualified as relay candidates, matching Espressif ESP-WIFI-MESH parent-selection behaviour.
  - `HOPDIST_WEIGHT = 10`, `MAX_HOPS = 10` - tunable constants at the top of `edge_nodes.ino`.

- **Neighbor table fast-path in `huntForPeers()`**
  - Before sweeping the full `authorizedEdgeNodes` list, the router checks the neighbor table for the highest-scored qualified peer and probes it first.
  - On success: `myHopDist` is updated to `best.hopDist + 1` and the peer is written to `preferredPeer` cache.
  - On miss: falls through to the existing cached-peer → channel-sweep → extended-retry chain unchanged.

- **`updateHopDistFromRelay()` helper** - ensures `myHopDist` is updated across all four relay paths (neighbor table, cached peer, channel sweep, extended retry). Falls back to `myHopDist = 2` if the relay peer is not yet in the neighbor table.

- **`myHopDist` global self-estimate** in `edge_nodes.ino`
  - Initialised to `HOPDIST_UNKNOWN (255)` at boot.
  - Set to `1` on first confirmed direct gateway transmission.
  - Updated to `relay.hopDist + 1` on every confirmed relay transmission via any of the four discovery paths.
  - Carried in `hopDist` field of every outgoing `SurvivorPayload`.

- **`"hops"` field in MQTT JSON output** in `gateway_node.ino`
  - `processPrimaryTasks()` now publishes: `{"node_id":…, "battery":…, "cpu":…, "sos_alert":…, "seq":…, "uptime_ms":…, "hops":…}`
  - Value `255` indicates the originating node had not yet confirmed its path distance - filterable in Grafana with a threshold panel.

- **`"hops"` field in DTN flash buffer JSON** in `gateway_node.ino`
  - Hop-count is preserved through outages for post-hoc topology analysis.

- **ESP32 promiscuous RSSI sidecar** - intercepts raw 802.11 vendor-specific action frames in promiscuous mode to extract hardware RSSI from ESP-NOW packets (unavailable via the standard receive callback). Filters by frame type (0xD0 action), category (0x7F vendor-specific), and Espressif OUI (0x18FE34). Cached in an 8-entry ring buffer and looked up by MAC in `processRepeatPayload()`. Re-enabled after every `WiFi.begin()` call (which resets promiscuous state).

- **Periodic neighbor table dump** - `printNeighborTable()` called every 30 seconds from `loop()`, output via `netlogln()` for both serial and UDP visibility. Includes per-entry RSSI, hopDist, composite score, staleness, and platform-conditional promiscuous mode counters.

- **Probabilistic Gossip Gate** in `processRepeatPayload()`
  - Introduced `GOSSIP_PROB = 75`.
  - Non-SOS packets are subjected to a random dice roll: 75% chance to forward, 25% chance to silently drop.
  - Reduces redundant mesh flood traffic (where every relay forwards every packet) by ~25% with negligible impact on Packet Delivery Ratio (PDR), due to the existence of multiple overlapping relay paths.
  - SOS packets bypass the gate and are always forwarded.

- **`HOPDIST_UNKNOWN` (255) constant** - sentinel value distinguishing "path not yet confirmed" from any real hop count.

- **Telemetry thresholds tuned** - battery drop exception widened to 4% (from 2%), heartbeat interval extended to 80s (from 60s), SOS probability threshold raised to 97% (from 95%). Reduces radio chatter on stable networks.

### Fixed - Edge Node (`edge_nodes.ino`)

- **ACK encryption mismatch in `processRepeatPayload()`** - relay ACKs were sent unencrypted (`NULL` key on ESP8266, `encrypt=false` on ESP32) while all other peer registrations used `LMK_KEY`. The originating node, which had the relay registered as encrypted, silently dropped the plaintext ACK. This caused every peer-to-peer relay attempt to fail with "All paths failed" despite both nodes being in range and receiving each other's payloads. Fixed by enforcing `LMK_KEY` encryption on all ACK peer registrations across both architectures.

- **ESP8266 promiscuous mode causes total baseband deafness** - enabling `wifi_promiscuous_enable(1)` on ESP8266 intercepts all incoming packets at the hardware level, completely severing the ESP-NOW receive callback (making the node deaf to all peers) and crashing the Station interface (causing lwIP WDT resets). The RSSI extraction logic was proven to work in isolation (`diagnostics.ino`) but cannot coexist with a functioning ESP-NOW mesh on this hardware. Fixed by wrapping all ESP8266 promiscuous code in `#if 0` and implementing `enablePromiscuousRssi()` as a no-op stub. Neighbor table entries from ESP8266 nodes carry `rssi = 0`, which is above `RSSI_FLOOR (-80)`, causing peer scoring to degrade gracefully to pure `hopDist` ordering.

- **Duplicate suppression blocks rebooted nodes** - the ACK was sent after the duplicate check. A rebooted node reuses sequence numbers from 0, and the relay's `dupCache` may still contain stale `(nodeId, seq)` entries from before the reboot. The relay would suppress the "duplicate" and never send an ACK, leaving the rebooted node retrying forever and unable to establish a relay path. Fixed by moving the ACK send before the duplicate check. Duplicate payloads are still suppressed for forwarding - only the ACK is guaranteed.

- **`myHopDist` stuck at 1 after gateway loss** - only the neighbor table fast-path (path 1) updated `myHopDist`. The other three paths (cached peer, channel sweep, extended retry) returned `true` without ever touching it. A node that found the gateway at boot (`myHopDist = 1`) and later relayed through a peer would permanently report `hops: 1` in MQTT. Fixed by adding `updateHopDistFromRelay()` calls to all four relay paths.

- **ESP8266 peer not registered before relay ACK** - on ESP8266, `esp_now_send()` requires the target to be registered as a peer. The relay ACK was sent without first calling `esp_now_add_peer()`, causing the ACK to be silently dropped. Fixed by adding `esp_now_del_peer()` + `esp_now_add_peer()` before the ACK send on ESP8266.

### Fixed - Gateway (`gateway_node.ino`)

- **`HaPayload` misclassified as `SurvivorPayload`** — `sizeof(HaPayload)` was 11 bytes, identical to `sizeof(SurvivorPayload)`. The gateway's `OnDataRecv` classifies packets by length, so HA election pings and heartbeats were silently processed as edge telemetry. Added a 1-byte `_pad` field to `HaPayload`, bringing its size to **12 bytes** and preventing misclassification. ⚠️ Gateway-only wire-format change — does not affect edge nodes (they never receive `HaPayload`).

### Changed - Repository Structure

- **Experimental data shipped in-repo.** `V.O.I.D_Experiments/raw/` contains the raw serial logs for all four experiments (E1–E4). `V.O.I.D_Experiments/processed/` contains `table1.csv` (summary results) and `latency_data.csv` (per-packet timing data). Venv and local logs remain gitignored.

- **`diagnostics/` added to repository.** Contains `diagnostics.ino` (standalone ESP8266 promiscuous-mode verification sketch) and `diagnostic_results.txt`. This sketch documents the hardware limitation that prevents RSSI extraction on ESP8266.

- **`.gitignore` cleaned up.** Consolidated triplicate `.DS_Store` entries into a single global pattern. Removed stale ignores for `serial_monitor.py`, `/logs`, `/upcoming_changes`. Added `/PrePrint` (preprint drafts) and `/utils` (private tooling) to ignore.

### Removed

- **`infrastructure/void_command_centre_v1.json`** — stale Grafana dashboard export. The observability stack is now documented in the README and the preprint rather than shipped as a versioned JSON file.

---

## [v2.2.0] — 2026-04-26

Stability hardening release. No new features — all changes are bug fixes addressing critical failures discovered during multi-hop relay testing. Twelve distinct bugs fixed across both firmware targets.

### Fixed — Edge Node (`edge_nodes.ino`)

- **Mesh ACK miss in `huntForPeers()` channel sweep** — `awaitedAckNode` was never set before transmission in the sweep loop. `OnDataRecv`'s ACK matching condition (`ack.nodeId == awaitedAckNode`) always evaluated against a stale value, causing every peer probe in the mesh discovery phase to silently fail. Fixed by setting `awaitedAckNode = payload.nodeId` before every `esp_now_send()` call in the sweep. This was the root cause of multi-hop appearing broken.

- **Blocking loop starves repeater buffer** — `huntForPeers()` could block the main thread for up to 13+ seconds (3-channel sweep + 10s extended retry). During this window, `processRepeatPayload()` was never called, so incoming relay packets from other nodes were silently dropped because `pendingRepeat` stayed `true` and `OnDataRecv` refused to overwrite it. Fixed by calling `processRepeatPayload()` inside the extended-retry loop between each peer round.

- **ESP32 dual-core race condition on `pendingRepeat`** — `pendingRepeatPayload` and `pendingRepeatSenderMac` were populated by the WiFi ISR on Core 0 while `processRepeatPayload()` read them on Core 1. The compiler could reorder the two `memcpy` operations relative to the `pendingRepeat = true` flag set, causing the main loop to read a half-written buffer. Fixed by wrapping all ISR buffer operations in `portENTER_CRITICAL` / `portEXIT_CRITICAL` on ESP32. ESP8266 is single-core and unaffected.

- **`processRepeatPayload()` called `connectHomeWifi()` mid-relay** — when a forwarded SOS packet failed to reach the gateway, the repeater path called `connectHomeWifi()`, which calls `WiFi.begin()`. On ESP8266, this can lock the radio to the router's channel, silently breaking all subsequent ESP-NOW sends on the mesh channel. Removed all `connectHomeWifi()` and `huntForGateway()` calls from the repeater path. Failed forwards are now silently queued to `edgeQueue` and flushed in the background.

- **`delay(1)` in ACK wait triggers ESP8266 WDT** — four burst attempts × 750ms = 3000ms is at the ESP8266 software watchdog boundary (~3s). Any overhead tipped it into a WDT reset. Replaced `delay(1)` with `yield()` in all ACK wait loops. `yield()` feeds the WDT and allows ESP-NOW receive callbacks to fire without blocking.

- **Relay queue never flushed from repeater path** — packets accumulated in `edgeQueue` via `processRepeatPayload()` were never dispatched unless the node happened to have its own outgoing telemetry ready (which triggers `flushEdgeQueue()`). A pure repeater node with stable battery and CPU would fill its queue silently and overflow. Fixed by calling `flushEdgeQueue()` unconditionally at the top of `loop()` when `edgeQueueCount > 0 && gatewayConnected`.

- **`connectHomeWifi()` does not verify channel match** — after `huntForGateway()` sets the radio to the ESP-NOW channel, `WiFi.begin()` for the home router could reassign it to a different channel, breaking ESP-NOW. Added a channel verification check: if `WiFi.channel()` after connection differs from `lastKnownChannel`, peers are re-registered on the new channel.

### Fixed — Gateway (`gateway_node.ino`)

- **Shadow/Election gateway drops edge payloads without ACKing** — when a gateway in `STATE_SHADOW` or `STATE_ELECTION` received a `SurvivorPayload`, it silently dropped it and sent no ACK. The edge node then exhausted 4 burst attempts (3 seconds) before declaring the gateway dead and escalating to the mesh. Fixed by sending an immediate `AckPayload` from `OnDataRecv` regardless of HA state. Payloads are still only enqueued when `STATE_PRIMARY` — the ACK only stops the edge burst timer.

- **`mqtt.loop()` only called when connected** — when MQTT was disconnected, the library's internal keepalive and ping state machine never ran, causing the broker to drop the TCP connection faster than the 15-second reconnect rate allowed. Fixed by calling `mqtt.loop()` unconditionally on every `processPrimaryTasks()` tick, regardless of connect state.

- **`shouldYieldToRemotePrimary()` uptime arithmetic wraps unsigned** — `msg.senderUptimeMs - millis()` was computed in `uint32_t` space before casting to `int32_t`. When both values were close and the remote was newer, the subtraction wrapped to a large positive number, inverting the comparison: a newer gateway would incorrectly yield to an older one. Fixed by casting both operands to `int32_t` before subtraction.

- **`flushFlashBuffer()` re-publishes already-sent lines on partial flush** — if MQTT disconnected mid-flush, the function halted correctly but left the entire file intact including already-published lines. On the next reconnect, all lines were re-published, injecting duplicates into InfluxDB. Fixed by collecting all lines into a RAM array, tracking `lastPublished` index, and rewriting the file with only the unsent suffix after each flush attempt.

- **`ramBuffer` has no overflow guard** — if MQTT was continuously unavailable and every payload triggered a critical state change, `ramBufferCount` would increment past `RAM_BUFFER_SIZE` into an out-of-bounds write. Fixed by adding a pre-emptive `flushRamToFlash()` call whenever `ramBufferCount >= RAM_BUFFER_SIZE` before appending a new entry.

---

## [v2.0.0] — 2026-04-25

Major feature release. Introduces multi-hop mesh relay, HA gateway election, and encrypted unicast transport. Two commits are grouped here as they form one coherent feature set that shipped together.

### Added — Multi-Hop Relay Routing

- **Edge-to-Edge relay capability** — any edge node can act as a repeater for out-of-range peers. When a node receives a `SurvivorPayload` with `ttl > 0` and `nodeId != myNodeId`, it ACKs the upstream sender, decrements TTL, and forwards toward the gateway.
- **`ttl` field in `SurvivorPayload`** ⚠️ Breaking — struct changes from 9 to 10 bytes alongside `uptimeMs` addition below. See payload changes.
- **`routePayload()` fallthrough chain** — direct gateway → `huntForPeers()` peer relay, without requiring a gateway re-hunt first.
- **`huntForPeers()` multi-channel sweep** — probes channels `{11, 6, 1}` across all `authorizedEdgeNodes`, bounded to `MESH_PER_PEER_WAIT_MS` (400ms) per peer per channel.
- **Preferred peer RAM cache** — last successful relay peer MAC is cached and tried first on next transmission, avoiding full sweep latency on stable topologies.
- **Round-robin extended retry** — 10-second retry window contacts all peers in rotation (400ms each) rather than blocking on a single peer.
- **Boot probe** — if gateway not found at boot, a probe `SurvivorPayload` with `isSosActive = true` is immediately sent into the mesh rather than waiting for the first telemetry trigger.
- **Deferred repeater processing** — `OnDataRecv` is kept to `memcpy + flag` only. All relay logic executes in `processRepeatPayload()` called from `loop()`. Eliminates ESP8266 Exception 9 stack overflow from calling `String`/`Serial`/`esp_now_send` in the SDK Wi-Fi callback context.
- **Dynamic temporary peer registration** — unregistered sender MACs are added as temporary unencrypted peers before relay ACK is dispatched, preventing silent ACK drops to unknown nodes.
- **`edgeQueue` survival buffer** — payloads that fail all delivery paths are queued in a 20-entry RAM array and flushed on the next successful gateway connection.
- **16-entry duplicate suppression cache** — circular buffer of `{nodeId, sequence}` pairs. Drops re-received packets to prevent infinite relay loops in the mesh.
- **60s heartbeat storm fix** — `lastTxTime` was only updated on delivery success. An unreachable gateway caused the heartbeat trigger to fire continuously. Fixed by updating `lastTxTime` regardless of delivery outcome after a full retry cycle.

### Added — Payload Changes ⚠️ Breaking

- **`uptimeMs` (uint32_t)** added to `SurvivorPayload` at byte offset 4, maintaining 4-byte alignment across both architectures: `nodeId(0), batteryPct(1), cpuLoad(2), isSosActive(3), uptimeMs(4–7), sequence(8), ttl(9)` — 10 bytes total.
- **`seq` and `uptime_ms`** added to gateway MQTT JSON output.

### Added — HA Gateway Election

- **Active-Passive auto-election state machine** — three states: `STATE_ELECTION`, `STATE_PRIMARY`, `STATE_SHADOW`.
- **Incumbent Rule (preemption disabled)** — an established primary is not displaced by a later-arriving higher-MAC node. Tiebreak order: uptime delta (±500ms margin) → MAC lexicographic comparison.
- **Randomised election window** (3.5–4.5 seconds) — prevents simultaneous promotion when multiple gateways boot together.
- **`senderUptimeMs` in `HaPayload`** — heartbeats carry the sender's `millis()` value, used as the uptime tiebreaker in `shouldYieldToRemotePrimary()`.
- **Immediate heartbeat on promotion** — new primary broadcasts `HA_MSG_HEARTBEAT` instantly on promotion to collapse any concurrent late-joiner election.
- **Dynamic ESP-NOW teardown/rebuild** (`configureEspNow(channel)`) — called on every Wi-Fi mode transition. Prevents ESP-NOW peer state loss when switching between `WIFI_STA` and `WIFI_AP_STA`, which caused radio deafness and hardware core panics on ESP32.
- **Wi-Fi sleep mode disabled** (`WIFI_NONE_SLEEP` / `WiFi.setSleep(false)`) — applied across all gateway state transitions. Sleep mode interactions with ESP-NOW caused intermittent receive failures.
- **Split-brain resolution** — if two primaries detect each other's heartbeat, `shouldYieldToRemotePrimary()` deterministically selects one to demote.
- **Shadow failover** — shadow monitors heartbeat with a 6-second timeout. On timeout, it re-enters election and self-promotes if uncontested.

### Fixed — Unicast Transport

- **Broadcast split-brain and packet drops** — migrated ESP-NOW telemetry from unencrypted broadcast to encrypted unicast. Broadcasts across the split AP/STA interface caused packet drops and split-brain conflicts between concurrent gateway receivers.
- **`QueuedPayload` struct** — gateway `rxQueue` now stores sender MAC alongside the payload, enabling accurate `sendAckUnicast()` targeting.
- **`sendAckUnicast()` live-channel peer refresh** — before each ACK, the target peer is deleted and re-added with `WiFi.channel()` (the live channel) rather than the static `meshChannel`. Prevents silent ACK drops caused by router channel reassignment after initial peer registration.
- **MQTT reconnect timeout reduced** — prevents prolonged TCP blocking during primary queue processing.

---

## [v1.1.0] — 2026-04-10

Security, identity, and telemetry refinements. Three commits grouped as one minor version.

### Added

- **E2E ESP-NOW encryption** — PMK (Primary Master Key) and LMK (Local Master Key) key hierarchy. PMK encrypts the LMK exchange during peer registration. LMK encrypts payload traffic on each peer-to-peer link. Hardware-agnostic initialisation: `esp_now_set_pmk()` on ESP32, `esp_now_set_kok()` on ESP8266.
- **Externalised secrets** — Wi-Fi SSID/password, MQTT broker IP, and PMK/LMK keys moved to `secrets.h` (gitignored). `example_secrets.h` templates provided for both targets.
- **Dynamic hardware node IDs** — node ID derived at runtime from the last byte of the hardware MAC address. No manual ID assignment required.
- **Central node registry** (`node_registry.h`) — statically compiled whitelist of authorised edge node MAC addresses. Used by both gateway (for encrypted peer registration) and edge nodes (for mesh peer discovery). `example_node_registry.h` templates provided.
- **Adaptive RBE (Report By Exception) telemetry** — edge nodes transmit on event (SOS, battery drop > 2%, CPU delta > 30%) rather than fixed interval. 60-second heartbeat as fallback.
- **Survival queue** — failed transmissions stored in a RAM queue and retried on next successful gateway contact.
- **Robust MAC parsing** — `parseMacString()` handles both colon-separated and compact hex MAC formats from scan results.
- **256dpi MQTT library migration** — replaced `PubSubClient` with `256dpi/arduino-mqtt`. Enables QoS 1 publish with server ACK verification. Shorter TCP timeout prevents main loop blocking during MQTT reconnect.

---

## [v1.0.0] — 2026-03-18

Initial architecture release. Establishes the core pipeline from ESP-NOW mesh to cloud observability.

### Added

- **Lighthouse zero-touch provisioning** — gateway injects its STA MAC address into its AP SSID (`VOID_<STAMAC>`). Edge nodes scan for this pattern at boot, extract the gateway MAC and Wi-Fi channel, and register the gateway as an ESP-NOW peer without any hardcoded network configuration.
- **LittleFS DTN pipeline** — gateway buffers incoming ESP-NOW payloads to onboard flash during upstream MQTT outages. Payload format: newline-delimited JSON in `/void_buffer.txt`. Flushed to MQTT on reconnection.
- **Hardware-agnostic firmware** — single codebase compiles and executes on both ESP32 (Arduino Core v3.x) and ESP8266 via preprocessor conditionals (`#ifdef ESP32` / `#elif defined(ESP8266)`). Adapts ESP-NOW API, Wi-Fi sleep modes, and callback signatures for each architecture.
- **MQTT → Telegraf → InfluxDB → Grafana pipeline** — telemetry published to `void/telemetry` topic. Telegraf ingests and writes to InfluxDB. Grafana dashboards provide real-time battery/CPU tracking with continuous colour gradients and SOS emergency state switching.
- **K3s deployment** — observability stack hosted on a K3s single-node Kubernetes cluster.
- **MIT License**
- **Repository structure** — `gateway_node/`, `edge_nodes/`, `assets/`, `.gitignore` excluding credentials and local tooling.

---

## [Unreleased — Pre-v1.0.0] — 2026-03-13

- Initial firmware scripts for gateway node and edge node. No provisioning, no DTN, no HA. Direct broadcast transmission only.