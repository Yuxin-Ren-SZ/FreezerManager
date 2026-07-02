# PRD Review: Small-Lab Terminal User Perspective

**Reviewer persona:** Academic postdoc, small lab (8 people), one large -80°C
freezer with diverse sample types (cell lines, brain tissue, plasmid DNA,
bacterial stocks, serum, CSF).  Daily terminal user.  Currently tracking
samples with Excel and `grep`.

**Document reviewed:** `doc/PRD.md` (design baseline 2026-05-06, last synced
2026-06-15).

---

## Features I Like

### Mixed-format box support (§4.2)

This is the killer feature.  A real -80°C freezer doesn't hold identical boxes:
9×9 cryoboxes, 100-well PCR tube boxes, mixed-format Eppendorf boxes (large
slots for 2 mL, small slots for 0.5 mL), 50 mL conical racks.  The
`Position.accepts` + `size_class` compatibility model describes exactly this
reality.  Most commercial software assumes uniform grids; the PRD assumes
heterogeneity.  That is the correct assumption.

### Aliquot lineage with independent lifecycle (§4.4)

Wet-lab reality: I aliquot one tube of CSF into 20 sub-tubes.  Depleting the
parent does not delete the children.  Child samples are independent, and the
lineage edge is preserved across soft-delete.  Exactly what I need.

### Check-out/check-in with volume auto-tracking (§4.4)

Someone takes a tube, uses 50 µL, puts it back with 150 µL remaining.  The
system knows who, when, and how much.  Auto-depletes at zero.  Matches daily
workflow without extra steps.

### Hierarchical item types with field inheritance (§4.3)

`liquid → csf` and `liquid → serum` inherit all custom fields from `liquid`,
then add their own.  I don't re-define shared fields for every sub-type.

### Soft-delete only (§4.4)

I never want to lose data to a mis-click.  Tombstone the row, keep the audit
trail, hard-delete only for `SystemAdmin` and audited.  This is correct.

### SQLite for small labs, upgrade path to PostgreSQL (§5)

Eight people, no DBA, one file.  But if we become a core facility, the same
code switches to PostgreSQL by changing a connection string.  No migration
project, no vendor negotiation.

### CSV import with dry-run validation (§13, M1 complete)

Three years of Excel data need importing.  Dry-run first, read the validation
report, then commit all-or-nothing.  This wizard flow _is_ my onboarding path.

### Barcode scanner focus mode (Qt client, M5)

USB barcode scanner as a keyboard wedge.  A "scan mode" that checks samples
in/out on scan — no mouse, no menus.  The PRD describes this mode.

### Atomic sample moves (§4.2)

Both positions update together or neither does.  No scenario where a sample
vanishes from its old slot without appearing in the new one.

### `freezerctl` CLI

I live in the terminal.  `freezerctl sample list --lab ... --freezer ...`
without opening a GUI.

### Encrypted backups with separate backup key (§8, §14)

Master key compromise does not decrypt historical backups, and vice versa.
The backup KEK is independent.  This is real engineering, not a feature
checklist item.

### Append-only hash-chained audit (§7.3)

When a sample goes missing, I need to know who touched it last — and the chain
must be tamper-proof.  `freezerctl audit verify` proves integrity.

### No per-seat licensing (§18, AGPLv3)

I don't ask my PI to budget $X/user/year.  Academic use is free.

---

## Features Missing From the PRD That I Need

### 1. Single-handed lookup — sub-second sample location

This is the #1 daily operation.  I stand in front of the -80°C with gloves on,
barcode scanner in one hand.  I scan or type a sample name, and the screen
shows immediately:

> ABC123 → Freezer 2, Shelf 3, Rack B, Box cryobox-45, Position A7

No tree navigation, no menus, no clicks.  The PRD describes sample search
infrastructure (full-text + structured filter) but does not specify the UX for
this single most frequent operation.

### 2. Printable box maps / labels

The `ILabelPrinter` interface exists in PRD §12 but the implementation is
deferred.  When I'm at the freezer in gloves, I don't want to touch a computer.
I want a printed sheet showing A1=sample_A, A2=sample_B, … for the box I'm
about to open.  This is the bridge between the database and the physical
freezer.  v1 should support at minimum "export box layout as PDF."

### 3. Label/barcode generation at sample creation

Create a sample → system generates a unique ID + barcode/QR code → I print it
and stick it on the tube.  The PRD has barcode scanning but does not explicitly
describe creation-time label generation.  Creation and labelling should be one
continuous action.

### 4. Freezer occupancy visualization — even 2D heatmaps

PRD §9 mentions "QML for visualization panes (freezer 3D layout, fill
heatmaps)" as a distant goal.  Before 3D, I need: a 2D color-coded grid
showing occupancy per shelf/rack/box.  Green = space available, red = full.
Which shelf has room?  Can box 45 fit one more tube?

### 5. Location-based batch operations

"Move all boxes on Shelf 3 to Shelf 4."  "Export CSV of every sample in Freezer
1, Rack B."  The domain model supports the hierarchy; the PRD does not describe
batch operations keyed by location in UI or CLI.

### 6. Temperature logging — no IoT required

PRD §1.3 defers IoT temperature monitoring past v1, which is acceptable.  But
manual temperature logging should be available: "Freezer 1, 2026-06-28, -79°C,
logged by: Yuxin."  A simple form + chart.  Alert if below threshold.  No
hardware needed.

### 7. Sample expiration / use-by tracking with dashboard warnings

Reagents and samples have expiration dates.  I can add a custom `date` field,
but the system has no built-in concept of expiration with dashboard warnings
("3 samples expire this week").  This is baseline inventory management for a
lab.

### 8. Batch import of instrument output data

Nanodrop, Qubit, and plate readers all produce CSV.  I want to import
concentration/purity data and link it to existing samples — not create new
entities.  The PRD's CSV import (§13) targets entity creation, not measurement
appending.

### 9. Dashboard / home screen

"Active samples: 847, checked out: 23, depleted: 12, 5 boxes <20% free."  The
PRD's infrastructure supports these queries but the UX spec does not define a
dashboard or home screen.

### 10. Batch aliquot creation

Aliquot 24 tubes from one parent into Box 5, positions A1–D6.  Specify parent
ID + count + starting position → system generates children and assigns
positions sequentially.  The parent-child data model supports this; the
workflow is not described.

### 11. Emergency freezer-failure relocation mode

Freezer dies at 10pm on a Saturday.  I need to move everything from Freezer 2
to Freezer 3 _immediately_.  The domain model supports atomic moves, but there
is no emergency-relocation workflow: batch-move, record as an emergency event,
relax position-by-position validation.

### 12. Built-in sample type templates

Item types are completely user-defined — powerful, but a new lab doesn't know
where to start.  Ship optional templates: Cell line (passage #, source,
resistance), DNA (conc, 260/280, vector), Tissue (species, organ, fixation),
Bacteria (strain, plasmid).  Opt-in only.

---

## Features I Don't Like / Design Concerns

### 1. Architecture overkill for a small lab

The PRD designs for: three-signature cross-lab sharing, five built-in roles +
custom roles, PHI field-level encryption with key rotation, hash-chained audit
+ nightly HMAC checkpoints, GFS backup retention.  This is the right
architecture for a hospital biobank or a core facility serving 20 labs.  For a
lab of 8 people with one -80°C freezer, it is massive overkill.

The question is whether this complexity leaks into the daily UX.  If adding a
sample requires configuring RBAC permissions first, I go back to my
spreadsheet.

### 2. Three-signature cross-lab sharing

PRD §4.1: `source_admin + target_admin + system_admin` must all approve a
`ShareRequest`.  If I want to give a tube of cells to the lab next door, I will
not use this workflow — I will hand over the tube and update my Excel.  There
should be a fast-path for informal sharing that still records the transfer.

### 3. No quick-add or draft mode for samples

Everything is transactional, audited, schema-validated.  Data integrity is
good, but there should be a low-friction path.  "I just put 5 tubes in Box 3,
positions A1–A5, all HEK293 passage 5 — I'll fill in details later."  The PRD
describes no draft/incomplete state for samples.

### 4. TOTP required for LabAdmin (§7.1)

I understand the security rationale.  But the lab admin is typically the PI or
a senior postdoc who occasionally approves new members.  Requiring TOTP
out-of-the-box is friction.  Strongly recommend it — don't require it for small
lab deployments.

### 5. PostgreSQL labelled "production-recommended"

For 10 users and 10,000 samples, SQLite with WAL mode is fully adequate.
Calling PostgreSQL "production-recommended" signals to small labs that their
SQLite deployment is second-class.  Rename to "scale-up option for multi-lab /
high-concurrency deployments."

### 6. gRPC required for the Python client

M6 plans gRPC for `freezerctl-py`, with a REST gateway as a secondary bridge.
If I want to `curl https://server/api/v1/samples?lab=...` or write a quick
Python script, gRPC is a barrier.  REST should be first-class; gRPC is the
optimization path for the Qt client.

### 7. AGPLv3 may scare institutional IT

PRD §20 acknowledges this risk.  Some university IT departments have blanket
policies against AGPL.  While AGPL for internal academic deployment is fully
legal, a clear "AGPL and Academic Use FAQ" document would help.

### 8. CLA required from contributors (§18)

If someone in my lab fixes a bug or adds a feature, they must sign a CLA
assigning copyright to the project owner.  For academic lab contexts, this is
unusual and may discourage casual contributions from postdocs who just want to
ship a fix and move on.

### 9. No physical placeholder label on checkout

When a sample is checked out, the system knows who took it.  But if it also
printed a label — "Taken by: Zhang, Date: 2026-06-28, Expected return: 7
days" — to stick in the empty slot, the physical freezer and the database would
stay in sync.  The database has the checkout record; the freezer doesn't.

---

## Summary

The PRD is serious about data safety and domain correctness: hash-chained
audit, atomic moves, mixed-format box geometry, aliquot lineage, encrypted
backups with a separate backup key.  These are engineering decisions backed by
1437 tests running in CI.  Excellent foundation.

However, the PRD's design priorities rank **Usability fifth** — behind data
safety, security, extensibility, and multi-user concurrency.  That ordering
makes sense for a multi-lab biobank or hospital deployment.  For a small
academic lab, usability should rank second or third — ahead of security, because
a system nobody wants to use is secure to the point of irrelevance.

Concretely: if v1 ships without **single-handed search, printable box maps, a
dashboard, and batch aliquot creation**, daily usability is insufficient for
this persona.  I would likely stay on Excel + `grep`.
