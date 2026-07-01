# FreezerManager — Upgrade / Migration Runbook

M3.5 operability · PRD §17.1  
Applies to: version N → N+1 upgrades on a single-node Linux deployment

## Pre-Upgrade Checklist

Run on the production host before starting the upgrade.

```bash
# 1. Verify current version is healthy
curl -s http://localhost:8080/health
# Expected: {"status":"SERVING"}

curl -s http://localhost:8080/health?full=1
# Expected: {"status":"SERVING","db":"ok"...}

# 2. Check metrics are green
curl -s http://localhost:8080/metrics | grep fmgr_backup_last_success

# 3. Create a pre-upgrade backup (SQLite)
freezerctl backup create --sqlite /var/lib/freezermanager/fmgr.db \
  --out /backup/pre-upgrade-$(date +%Y%m%d-%H%M).fmgrbak \
  --actor <your-user-uuid>

# 4. Verify the backup
freezerctl backup verify --in /backup/pre-upgrade-*.fmgrbak
# Expected: PASS

# 5. Record current schema version
freezerctl db version --sqlite /var/lib/freezermanager/fmgr.db
```

## Upgrade Procedure

### 1. Stop the Service

```bash
sudo systemctl stop freezermanager
# Wait for graceful shutdown (default 30 s)
sudo systemctl status freezermanager
# Expected: inactive (dead)
```

### 2. Back Up Configuration

```bash
cp /etc/freezermanager/config.yaml /backup/config-$(date +%Y%m%d).yaml
```

### 3. Install New Binary

```bash
# Debian/Ubuntu
sudo dpkg -i freezermanager_<version>_amd64.deb

# Or: replace the binary directly
sudo cp freezermanager /usr/local/bin/
```

### 4. Run Schema Migrations

Migrations run automatically on first start. To preview:

```bash
# SQLite
sudo -u freezermanager freezermanager --migrate-only \
  --sqlite /var/lib/freezermanager/fmgr.db

# PostgreSQL (if used)
sudo -u freezermanager freezermanager --migrate-only \
  --postgres "$FMGR_DATABASE_URL"
```

If migrations fail: **STOP. Do not proceed.** Restore from backup (see Rollback).

### 5. Key Rotation (if KMS Master KEK Changed)

Only needed when the release notes state `BREAKING: master KEK rotation required`.

```bash
# 1. Stage the new KEK alongside the old one
#    (systemd credentials example):
sudo cp /etc/credstore/freezermanager/master_kek.new \
       /etc/credstore/freezermanager/master_kek.prev.$(date +%s)

# 2. Run rotation until all records migrated
freezerctl key rotate --sqlite /var/lib/freezermanager/fmgr.db \
  --actor <admin-uuid>

# Output: scanned N, rewrapped M, current K, failed 0
# Re-run if failed > 0 (fix failures before proceeding)

# 3. Remove the old KEK once all records migrated
```

### 6. Start New Version

```bash
sudo systemctl start freezermanager
sleep 3
sudo systemctl status freezermanager
# Expected: active (running)
```

## Post-Upgrade Verification

```bash
# 1. Health check
curl -s http://localhost:8080/health
# Expected: {"status":"SERVING"}

curl -s http://localhost:8080/health?full=1
# Expected: all dependencies OK

# 2. Sample CRUD smoke test
freezerctl sample list --lab <lab-uuid> --limit 1
# Expected: 1 sample(s) or 0 sample(s) (not an error)

# 3. Audit chain integrity
freezerctl audit verify --sqlite /var/lib/freezermanager/fmgr.db
# Expected: Chain verified (N events)

# 4. Check metrics
curl -s http://localhost:8080/metrics | grep fmgr_
```

## Rollback

If post-upgrade verification fails:

```bash
# 1. Stop new version
sudo systemctl stop freezermanager

# 2. Restore database from pre-upgrade backup
freezerctl backup restore \
  --in /backup/pre-upgrade-<timestamp>.fmgrbak \
  --out /var/lib/freezermanager/fmgr.db \
  --force \
  --actor <admin-uuid>

# 3. Downgrade binary
sudo dpkg -i freezermanager_<old-version>_amd64.deb

# 4. Start old version
sudo systemctl start freezermanager
curl -s http://localhost:8080/health
# Expected: {"status":"SERVING"}
```

## Version Compatibility

FreezerManager follows **semantic versioning**:

| Change | Schema Migration | Key Rotation | Downgrade |
|--------|-----------------|--------------|-----------|
| Patch (x.y.Z) | Never | Never | Safe |
| Minor (x.Y.z) | Additive only | Optional | Safe (new columns ignored) |
| Major (X.y.z) | May restructure | May require | Not supported without rollback |

The release notes for every version state:
- Whether a schema migration runs
- Whether master KEK rotation is required
- Minimum downgrade version supported
