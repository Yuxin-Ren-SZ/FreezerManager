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

## Behaviour Changes

Changes that alter what a running deployment accepts or does, as opposed to
adding features. Read this before upgrading across the listed releases.

### Browser sessions (`G0.1`, merged 2026-09-29)

**`?access_token=` is no longer accepted on the SSE routes.** `EventSource`
cannot set headers, so the token used to travel in the URL — where it lands in
proxy and access logs. It is now rejected; clients must authenticate with the
session cookie or an `Authorization` header. If you have a client, script or
proxy rewriting SSE URLs with `access_token`, it stops working at this upgrade.

New, additive: `POST /api/v1/auth/browser/{login,submit-mfa,logout}` set an
`HttpOnly` `fmgr_session` cookie plus a JS-readable `fmgr_csrf`, and any
cookie-authenticated mutating request must echo the CSRF cookie in
`X-CSRF-Token` and send a matching `Origin`. Bearer-token callers are unaffected.

**If the gateway runs behind a reverse proxy, the proxy must preserve the public
`Host`**, or every mutating request returns 403 with nothing in the body to
explain why. `FMGR_WEB_ORIGIN` names an additional accepted origin when it
cannot.

### `session.revoke` is deployment level (`#77`, merged 2026-09-29)

`session.revoke` moved into the global-only permission set. **A lab-scoped role
that had been granted `session.revoke` no longer has it** — the grant is left in
the database and is now inert rather than deleted.

That is deliberate: the permission let a lab administrator of one lab revoke a
session belonging to a user of another lab, because the RPC takes a session id
and no lab. Self-revocation (logging yourself out) never needed the permission
and is unaffected.

If you granted `session.revoke` to a custom lab role, either remove the grant or
move the role's holders to a system-administrator role, which still holds it.

### Additive, for completeness

- `POST /api/v1/auth/login` (the legacy route) now returns `user_id` in its
  response. Additive for scripts; the field was already declared in the proto.
- `sample/update` no longer replaces a sample's encrypted PHI envelope when the
  caller could not have seen it, and a blank PHI value cannot erase a stored one.

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
