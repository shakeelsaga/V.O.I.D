# V.O.I.D (Vital Offline Information Dispatch)

V.O.I.D. is an edge-to-cloud telemetry pipeline and Delay-Tolerant Network (DTN) engineered for emergency disaster scenarios. When standard communication grids fail, the system utilizes low-power microcontrollers to autonomously form a survival mesh network, buffering telemetry and bridging it into a cloud-native Kubernetes observability stack upon infrastructure restoration.

## Core Architecture

* **Hardware Agnostic:** Firmware compiles and executes dynamically across both ESP32 (Core v3.x compliant) and ESP8266 architectures.
* **Lighthouse Auto-Discovery:** Implements Zero-Touch Provisioning via SSID injection. Edge nodes actively scan for the Gateway's dynamic MAC address and Wi-Fi channel, bypassing static network configurations.
* **Delay-Tolerant Networking (DTN):** Utilizes LittleFS on the Gateway to buffer ESP-NOW payloads into local flash storage during upstream network outages, preventing data loss.
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

## Current State and Version 1.0 Limitations

This repository represents Version 1.0 of the V.O.I.D. infrastructure. The current release focuses strictly on establishing the routing logic, auto-discovery protocols, and DTN architecture.

**Notes on v1.0:**
* **Simulated Hardware Metrics:** For this initial deployment, the `batteryPct` and `cpuLoad` metrics are generated via a randomized software simulation loop to stress-test the data pipeline. 
* **Future Hardware Integration:** Version 2.0 will bind these variables to physical hardware sensors (e.g., analog voltage dividers for LiPo batteries).
