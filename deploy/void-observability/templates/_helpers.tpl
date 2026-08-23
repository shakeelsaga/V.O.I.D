{{/*
Generate a fullname: <release-name>-<chart-name>, truncated to 63 chars.
Used as the base name for all resources.
*/}}
{{- define "void-observability.fullname" -}}
{{- printf "%s-%s" .Release.Name .Chart.Name | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
Common labels applied to every resource.
*/}}
{{- define "void-observability.labels" -}}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version }}
{{- end -}}

{{/*
Selector labels — the stable subset used in Service selectors and
Deployment/StatefulSet matchLabels. These must NOT change between upgrades.
*/}}
{{- define "void-observability.selectorLabels" -}}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}
