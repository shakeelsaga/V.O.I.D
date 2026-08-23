# V.O.I.D. Observability Helm Chart

Helm chart that packages the V.O.I.D. observability stack — Mosquitto, Telegraf,
InfluxDB 2.x, and Grafana — into a single deployable unit for Kubernetes.

## Architecture

```
ESP32 Gateway ──► Mosquitto (MQTT) ──► Telegraf ──► InfluxDB 2.x ──► Grafana
                  (LoadBalancer)                     (StatefulSet)    (LoadBalancer)
```

| Component  | Role | Kubernetes Resource |
|------------|------|---------------------|
| Mosquitto  | MQTT broker — ingestion endpoint for the gateway node | Deployment + LoadBalancer Service |
| Telegraf   | Subscribes to `void/telemetry`, parses JSON, writes to InfluxDB | Deployment (no Service) |
| InfluxDB   | Persistent time-series storage with auto-initialised org/bucket | StatefulSet + PVC + ClusterIP Service |
| Grafana    | Dashboard UI with auto-provisioned datasource and panels | Deployment + LoadBalancer Service |

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
| Grafana   | 3000 | `http://<external-ip>:3000` (admin / admin) |
| Mosquitto | 1883 | `tcp://<external-ip>:1883` |

The pre-loaded **V.O.I.D. Survivor Telemetry** dashboard is available under
Dashboards in Grafana immediately after deployment.

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

See [`values.yaml`](values.yaml) for the full reference.

## Uninstall

```bash
helm uninstall void-obs -n void
```

> **Note:** The InfluxDB PVC is retained after uninstall to prevent data loss.
> To fully clean up: `kubectl delete pvc -l app.kubernetes.io/instance=void-obs -n void`
