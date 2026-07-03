<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# gRPC TLS & mTLS (transport security)

`freezerd` serves its gRPC listener over **TLS 1.3 only** (PRD §6). The REST/JSON
gateway has its own TLS config (`FMGR_REST_TLS_CERT` / `FMGR_REST_TLS_KEY`); this
document covers the **gRPC** side.

> **Production requirement.** TLS is mandatory for any non-loopback deployment —
> bearer tokens and (in PHI mode) encrypted-field envelopes travel on this
> channel. Set `FMGR_REQUIRE_TLS=1` in production: `freezerd` then refuses to
> start a plaintext listener if the cert/key paths are missing (fail loud, never
> serve in the clear).

## Environment variables

| Variable | Meaning | Default |
|----------|---------|---------|
| `FMGR_TLS_CERT` | Path to the server certificate (PEM). Empty ⇒ plaintext dev mode. | *(empty)* |
| `FMGR_TLS_KEY` | Path to the server private key (PEM). | *(empty)* |
| `FMGR_REQUIRE_TLS` | `1`/`true` ⇒ abort startup rather than bind plaintext when cert/key are unset. | off |
| `FMGR_MTLS` | Client-certificate policy: `off` \| `request` \| `require`. | `off` |
| `FMGR_TLS_CLIENT_CA` | CA bundle (PEM) used to verify client certs. Required when `FMGR_MTLS=require`. | *(empty)* |
| `FMGR_TLS_RELOAD_SEC` | Poll interval (seconds) for hot-reloading cert/key/CA from disk. | `5` |

### mTLS modes (`FMGR_MTLS`)

- **`off`** — server-auth TLS only; the server never asks for a client cert.
- **`request`** — opportunistic: verify a client cert if one is presented, but do
  not fail the handshake when the client offers none.
- **`require`** — demand a client cert signed by `FMGR_TLS_CLIENT_CA`; reject the
  connection otherwise. Use for machine/instrument clients (PRD §7.1 `MtlsAuthProvider`).

## Certificate rotation

The listener uses gRPC's `FileWatcherCertificateProvider`, which polls the
cert/key/CA files every `FMGR_TLS_RELOAD_SEC` seconds and hot-swaps them for
**new** handshakes **without dropping in-flight connections**. To rotate:

1. Write the new cert/key (and CA, for mTLS) to the same paths.
2. Wait up to `FMGR_TLS_RELOAD_SEC`; new connections use the new material.
3. `SIGHUP` is honored as the documented operator gesture and logged
   (`tls.sighup`), but reload is automatic — no signal is strictly required.

## Example

```sh
# Server-auth TLS, production guard on:
FMGR_REQUIRE_TLS=1 \
FMGR_TLS_CERT=/etc/freezerd/tls/server-cert.pem \
FMGR_TLS_KEY=/etc/freezerd/tls/server-key.pem \
  freezerd

# Mutual TLS for machine clients:
FMGR_REQUIRE_TLS=1 \
FMGR_TLS_CERT=/etc/freezerd/tls/server-cert.pem \
FMGR_TLS_KEY=/etc/freezerd/tls/server-key.pem \
FMGR_MTLS=require \
FMGR_TLS_CLIENT_CA=/etc/freezerd/tls/client-ca.pem \
  freezerd
```

Verify with `grpcurl`:

```sh
grpcurl -cacert ca-cert.pem localhost:50051 list          # succeeds over TLS
grpcurl -plaintext          localhost:50051 list          # fails (handshake)
# mTLS require:
grpcurl -cacert ca-cert.pem -cert client-cert.pem -key client-key.pem \
        localhost:50051 list                               # succeeds
```

Self-signed certificates are fine for dev. Production should use a certificate
from your institutional CA or an internal PKI; the server pins TLS 1.3, so
gRPC/BoringSSL restricts the cipher suites to the TLS 1.3 AEAD set automatically.
