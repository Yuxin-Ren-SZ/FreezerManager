# FreezerManager — Service Level Objectives

M3.5 operability · PRD §17.1  
Version: 0.1 (pre-alpha)

## Uptime

| Metric | Target | Measurement |
|--------|--------|-------------|
| Availability | **99.5%** monthly (≤ 3.65 h downtime) | `/health` endpoint polled every 30 s |
| Single-node only | No HA/clustering in M3.5 | Server process liveness |

## Request Latency

All targets measured at the gRPC server (server-side, excluding network).

| Percentile | Unary RPC | Streaming RPC | Measurement |
|------------|-----------|---------------|-------------|
| p50 | < 10 ms | < 50 ms | `fmgr_rpc_latency_seconds` histogram |
| p95 | < 50 ms | < 200 ms | same |
| p99 | < 200 ms | < 500 ms | same |

Burst budget: 5-minute windows may exceed p99 up to 2× the target.

## Error Rate

| Metric | Target | Measurement |
|--------|--------|-------------|
| Authenticated RPC errors | **< 0.1%** of requests | `fmgr_rpc_total{code!="OK"}` / `fmgr_rpc_total` |
| Unauthenticated errors | Excluded from budget | `grpc_code="UNAUTHENTICATED"` filtered out |
| `/health` failures | **< 0.01%** of probes | Independent of RPC budget |

## Backup

| Metric | Target | Measurement |
|--------|--------|-------------|
| RPO (Recovery Point Objective) | **24 hours** | `fmgr_backup_last_success_timestamp_seconds` gauge |
| RTO (Recovery Time Objective) | **1 hour** | Manual measurement (restore drill) |
| Restore drill | Weekly automated | `freezerctl backup verify` in cron |

## Prometheus Metric Reference

Endpoint: `GET /metrics`

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `fmgr_rpc_total` | Counter | `method`, `code` | RPC call count by gRPC method + status |
| `fmgr_rpc_latency_seconds` | Histogram | `method` | Server-side unary latency |
| `fmgr_audit_events_total` | Counter | — | Audit events appended |
| `fmgr_backup_last_success_timestamp_seconds` | Gauge | — | Unix timestamp of last successful backup |
| `fmgr_health_status` | Gauge | `dependency` | 1 = healthy, 0 = failing |

## `/health` Contract

```
GET /health          → 200 OK  {"status":"SERVING"}
GET /health?full=1   → 200 OK  {"status":"SERVING","db":"ok","kms":"ok"}  
                    or 503     {"status":"NOT_SERVING","db":"timeout"}
```

Dependency checks:
- **db**: SQLite/Postgres backend reachable (simple query)
- **kms**: KMS provider configured (SKIP if PHI disabled)

## Alert Thresholds

| Condition | Threshold | Severity |
|-----------|-----------|----------|
| Error rate | > 1% over 5 min | Warning |
| Error rate | > 5% over 5 min | Critical |
| `/health` failing | > 60 s | Critical |
| Backup age | > 30 h | Warning |
| Backup age | > 48 h | Critical |
| Process down | Any | Critical |

## Exclusions

- **Planned maintenance**: SLO excludes announced maintenance windows
- **Force majeure**: Infrastructure failures outside application control
- **Dependency failures**: PostgreSQL outage counted only if app-level timeout < 5 s
