# V.O.I.D. Observability Helm Chart

Helm chart that packages the V.O.I.D. observability stack — Mosquitto, Telegraf,
InfluxDB 2.x, Grafana, Prometheus, and Alertmanager — into a single deployable
unit for Kubernetes.

## Architecture

```
ESP32 Gateway ──► Mosquitto (MQTT) ──► Telegraf ──► InfluxDB 2.x ──► Grafana
                  (LoadBalancer)                     (StatefulSet)    (LoadBalancer)
                                                          ▲
                                                          │ (metrics scrape)
                                                          │
      node-exporter ──► Prometheus ◄── kube-state-metrics
                       (ClusterIP)
```

| Component  | Role | Kubernetes Resource |
|------------|------|---------------------|
| Mosquitto  | MQTT broker — ingestion endpoint for the gateway node | Deployment + LoadBalancer Service |
| Telegraf   | Subscribes to `void/telemetry`, parses JSON, writes to InfluxDB. Exposes pipeline metrics on `:9273` for Prometheus | Deployment + ClusterIP Service |
| InfluxDB   | Persistent time-series storage with auto-initialised org/bucket | StatefulSet + PVC + ClusterIP Service |
| Grafana    | Dashboard UI with auto-provisioned datasource and panels | Deployment + LoadBalancer Service |
| Prometheus | Infrastructure monitoring and `/metrics` scraping | StatefulSet + PVC + ClusterIP Service |
| Alertmanager | Alert routing and Slack notifications | Deployment + ClusterIP Service |
| node-exporter | Host-level CPU, memory, disk, network metrics | DaemonSet + ClusterIP Service |
| kube-state-metrics | Kubernetes object health (pod status, restarts, PVC usage) | Deployment + ClusterIP Service |

## Prerequisites

- Kubernetes cluster (K3s, Kind, EKS, etc.)
- [Helm 3](https://helm.sh/docs/intro/install/)
- A default StorageClass provisioner (K3s ships with `local-path`)

## Quick Start

```bash
helm install void-obs deploy/void-observability/ -n void --create-namespace
```

Verify all pods are running:

```bash
kubectl get pods -n void
```

## Accessing Services

After deployment, retrieve the external IP assigned by the LoadBalancer:

```bash
kubectl get svc -n void
```

| Service   | Default Port | URL |
|-----------|-------------|-----|
| Grafana    | 3000 | `http://<external-ip>:3000` (admin / admin) |
| Mosquitto  | 1883 | `tcp://<external-ip>:1883` |
| Prometheus | 9090 | `http://<external-ip>:9090` (port-forward) |
| Alertmanager| 9093 | `http://<external-ip>:9093` (port-forward) |

The pre-loaded **V.O.I.D. Survivor Telemetry** and **V.O.I.D. Infrastructure Health**
dashboards are available under Dashboards in Grafana immediately after deployment.

## Configuration

Override defaults at install time with `--set` or a custom values file:

```bash
helm install void-obs deploy/void-observability/ \
  --set influxdb.auth.adminToken=my-secret-token \
  --set influxdb.storage.className=standard \
  -n void --create-namespace
```

| Parameter | Description | Default |
|-----------|-------------|---------|
| `mosquitto.service.type` | Service type for MQTT broker | `LoadBalancer` |
| `mosquitto.service.port` | MQTT broker port | `1883` |
| `influxdb.storage.size` | PVC size for time-series data | `5Gi` |
| `influxdb.storage.className` | StorageClass for PVC | `local-path` |
| `influxdb.auth.adminToken` | InfluxDB admin API token | `void-admin-token` |
| `influxdb.auth.org` | InfluxDB organisation | `void` |
| `influxdb.auth.bucket` | InfluxDB bucket | `void_telemetry` |
| `grafana.service.type` | Service type for Grafana | `LoadBalancer` |
| `grafana.service.port` | Grafana UI port | `3000` |
| `grafana.adminUser` | Grafana admin username | `admin` |
| `grafana.adminPassword` | Grafana admin password | `admin` |
| `prometheus.storage.size` | PVC size for Prometheus time-series data | `2Gi` |
| `alertmanager.slackWebhookUrl`| Slack Webhook URL for alert routing | `""` |

See [`values.yaml`](values.yaml) for the full reference.

## Uninstall

```bash
helm uninstall void-obs -n void
```

> **Note:** The InfluxDB and Prometheus PVCs are retained after uninstall to prevent data loss.
> To fully clean up: `kubectl delete pvc -l app.kubernetes.io/instance=void-obs -n void`
