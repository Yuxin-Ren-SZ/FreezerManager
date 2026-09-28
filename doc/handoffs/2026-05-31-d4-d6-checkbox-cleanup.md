# Handoff note — 2026-05-31, D4/D6 checkbox cleanup

Ticked D4 outer checkbox (D4.1 and D4.2 were both complete but outer was left
open) and all D6.* checkboxes (ItemType, CustomFieldDefinition, validator engine,
and is_phi flag all implemented in the D6 commit). D6.4 note: the schema column
and type flag are in place; enforcement (routing phi fields through the encryption
layer) is deferred to H3 (PHI/KMS section). No code changes — bookkeeping only.

D9 (Session entity) is the next domain slice. It is a blocker for E1 (IAuthProvider
interface) because the auth layer needs to store and validate opaque server-side
sessions and API tokens. Recommended implementation order: D9 → E5.1 (audit
schema) → E1 → E2 (LocalAuthProvider) → E3 (RBAC middleware).
