# FreezerManager

Open-core, self-hostable freezer / biospecimen management system for academic
and clinical research labs. Written in C++20, designed for data safety,
security (PHI-aware), extensibility, and multi-user concurrency.

> **Status:** Pre-alpha — design phase. See
> [`doc/PRD.md`](./doc/PRD.md) for the full product requirements & design
> document (v0.1, 2026-05-06).

## Highlights (planned for v1)

- Multi-lab on a single deployment with strict isolation.
- Pluggable storage backend (`IStorageBackend`); reference impls for SQLite
  (dev / small labs) and PostgreSQL (production-recommended).
- gRPC primary protocol + REST/JSON gateway; React/TypeScript web UI; Qt 6
  desktop client; Python client (`freezerctl-py`) for scripting & plotting.
- RBAC with per-resource scopes; pluggable auth (local + TOTP, OIDC, LDAP,
  mTLS).
- Append-only, hash-chained audit log; PHI field-level encryption with
  pluggable KMS.
- TDD throughout: abstract conformance suites for storage, auth, KMS, and
  hardware integrations.

## Documentation

- [`doc/PRD.md`](./doc/PRD.md) — product requirements & design document.

## License

FreezerManager is **dual-licensed**:

- **AGPLv3** for open-source / academic / non-commercial use — see
  [`LICENSE`](./LICENSE).
- **Commercial license** available from the project owner — see
  [`LICENSING.md`](./LICENSING.md).

Contributions require signing the Contributor License Agreement — see
[`CLA.md`](./CLA.md). In short: add `Signed-off-by: Your Name <email>` to
every commit (`git commit -s`).

Contact: Yuxin Ren — `yxren_CN@outlook.com`
