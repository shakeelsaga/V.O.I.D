# V.O.I.D (Vital Offline Information Dispatch)
 
V.O.I.D. is an edge-to-cloud telemetry pipeline and Delay-Tolerant Network (DTN) engineered for emergency disaster scenarios. When standard communication grids fail, the system utilizes low-power microcontrollers to autonomously form a survival mesh network, buffering telemetry and bridging it into a cloud-native Kubernetes observability stack upon infrastructure restoration.

## Core Architecture

* **Hardware Agnostic:** Firmware compiles and executes dynamically across both ESP32 (Core v3.x compliant) and ESP8266 architectures.
* **Lighthouse Auto-Discovery:** Implements Zero-Touch Provisioning via SSID injection. Edge nodes actively scan for the Gateway's dynamic MAC address and Wi-Fi channel, bypassing static network configurations.
* **Delay-Tolerant Networking (DTN):** Utilizes LittleFS on the Gateway to buffer ESP-NOW payloads into local flash storage during upstream network outages, preventing data loss. Includes RAM batching, delta-state filtering to protect flash wear, and mid-flush integrity protection to prevent duplicate publishes.
* **Multi-Hop Mesh Routing:** Edge nodes operate as repeaters. When direct gateway range is unavailable, telemetry is forwarded through peer nodes via a multi-channel sweep across {11, 6, 1}, with a preferred-peer cache for fast reconnection and a 16-entry duplicate suppression cache to prevent relay loops.
* **Highly Available (HA) Gateway:** Two gateway nodes participate in an Active-Passive auto-election. The Incumbent Rule (preemption disabled) selects the primary by uptime, with MAC address as a tiebreaker. If the primary fails, the shadow self-promotes within 7 seconds without any manual intervention.
* **End-to-End Encryption:** ESP-NOW links are encrypted using a PMK/LMK two-key hierarchy. Credentials and node MAC whitelists are externalized into `secrets.h` and `node_registry.h`, which are excluded from the public repository.
* **Adaptive RBE Telemetry:** Edge nodes transmit on exception, not on a fixed timer — immediately on SOS, battery drop, or CPU spike, and every 60 seconds as a heartbeat fallback. Unsent payloads are queued in RAM and flushed when a path is restored.
* **Cloud-Native Observability:** Routes telemetry via a Mosquitto MQTT broker into a Telegraf and InfluxDB pipeline, visualized in real-time through a Grafana Command Center hosted on a K3s cluster.

---

## System Workflow & Visual Verification

### 1. Initialization and Zero-Touch Provisioning
The Gateway connects to the upstream network and broadcasts a dynamic Lighthouse beacon. The Edge Node powers on, scans the environment, extracts the MAC address from the Lighthouse signature, and dynamically tunes its radio channel to initiate the connection.

![Gateway Initialization and Upstream Connection](assets/V.O.I.D%20Gateway%20Node%201.png)
> *Gateway Node initializing the Lighthouse beacon, establishing the MQTT connection, and receiving initial edge data.*

![Edge Node Auto-Discovery](assets/V.O.I.D%20Edge%20Node%201.png)
> *Edge Node executing the channel sweep, locking onto the Gateway's dynamic MAC, and transmitting telemetry.*

### 2. Mesh Transmission Protocol
Data is transmitted asynchronously via the ESP-NOW protocol. The hardware provides instant delivery acknowledgments to verify the successful routing of SOS signals.

![Radio Transmission Status](assets/V.O.I.D%20Edge%20Node%202.png)
> *Edge Node logging successful delivery when the Gateway is active, and delivery failure when the Gateway is offline.*

### 3. Delay-Tolerant Networking (DTN)
If the upstream Kubernetes server disconnects, the Gateway intercepts the failure and buffers incoming radio packets into onboard flash storage.

![Server Disconnect and Local Buffering](assets/V.O.I.D%20Gateway%20Node%202.png)
> *The upstream MQTT connection drops. The Gateway detects the failure and buffers incoming edge data into LittleFS.*

![Reconnection and Buffer Flush](assets/V.O.I.D%20Gateway%20Node%203.png)
> *The server is restored. The Gateway instantly re-establishes the connection and flushes the buffered payload sequence to the database.*

### 4. Observability and Command Center
The backend relies on a TIG stack (Telegraf, InfluxDB, Grafana) to provide a fault-tolerant, real-time map of hardware health and survivor telemetry.

![Grafana Command Center - Normal State](assets/V.O.I.D%20Dashboard%201.png)
> *The Grafana dashboard tracking continuous analog metrics (Battery/CPU) with shifting color gradients, currently displaying a stable 'NORMAL' operational state.*

![Grafana Command Center - Emergency State](assets/V.O.I.D%20Dashboard%202.png)
> *The dashboard instantly switching to a critical 'EMERGENCY!' state alert upon receiving an active SOS payload from a decentralized Edge Node.*

---

## Current State
 
**Current stable release: `v2.2.0`** &nbsp;|&nbsp; In development: `v2.3.0` (hop-distance vector routing + RSSI peer scoring)
 
V2.2 is a hardened, fully functional release. All core systems — zero-touch provisioning, DTN buffering, multi-hop relay, HA election, and encrypted unicast handshake — are implemented and verified against real hardware failure scenarios.
 
**Known limitations in v2.2:**
* **Simulated Hardware Metrics:** `batteryPct` and `cpuLoad` are generated via a software simulation loop to stress-test the pipeline. Physical sensor binding (e.g., LiPo voltage dividers) is planned for v3.0.
* **Static Node Registry:** Authorised nodes must be compiled into `node_registry.h`. Dynamic onboarding without reflashing is not yet supported.
* **Indoor Testing Only:** All experiments conducted at short range (~5 m). Outdoor RF propagation characteristics have not been evaluated.

For full version history, see [CHANGELOG.md](CHANGELOG.md).
