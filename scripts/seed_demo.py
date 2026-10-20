#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Comprehensive demo data seeder for FreezerManager.

Generates a rich demo database with:
- 2 labs (Neuroscience Lab with PHI mode on, Oncology Lab)
- 5 users with different roles (SystemAdmin, 2x LabAdmin, Member, ReadOnly)
- Freezer hierarchy with compartments, shelves, racks
- Two box grids (9x9 and 10x10 cryoboxes), both actually used
- Container types (cryovial, tube, microplate)
- Item type taxonomy (CSF, Plasma, Serum, DNA, RNA, Cell Pellet, Tissue)
- 32 samples: 20 Neuro (CSF patients + aliquots + DNA/RNA), 12 Onco (tumor biopsies + aliquots)
- Custom field definitions, one of them PHI-tagged (CSF `diagnosis`), with the
  SystemAdmin role granted `phi.read` so the gate is visible
- An audit trail for every seeded sample, written by the storage/audit path
  (`freezerctl sample import`), not by hand

Usage:
    # 1. Start freezerd once to run migrations on a fresh DB:
    #    FMGR_DB_PATH=data/freezer.db ./out/build/dev/src/server/freezerd
    #    (kill it after it starts)

    # 2. Seed (the freezerctl binary must be built; the demo server needs the
    #    same master key to decrypt the PHI field):
    #    FMGR_MASTER_KEK='<base64 of 32 random bytes>' python3 scripts/seed_demo.py

    # 3. Restart server:
    #    FMGR_MASTER_KEK='...' FMGR_DB_PATH=data/freezer.db \
    #      ./out/build/dev/src/server/freezerd

The DB path defaults to data/freezer.db.
Set FREEZER_DB_PATH env var to override, and FMGR_FREEZERCTL to override the
freezerctl binary path (otherwise the build tree is searched).

WHY THE SAMPLES GO THROUGH freezerctl: writing sample rows with raw SQLite
skipped the audit chain and the PHI envelope, so a demo database had an empty
audit log and plaintext PHI. `sample import` is the same storage write path the
server uses (one transaction per batch, `note_mutation` per row), so every
sample gets a real `sample.insert` audit event and the PHI field lands encrypted.
"""

import csv
import glob
import json
import os
import shutil
import sqlite3
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Optional

# ── Project paths ──────────────────────────────────────────────────────────

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = os.environ.get("FREEZER_DB_PATH", str(PROJECT_ROOT / "data" / "freezer.db"))

# ── UUID helpers ───────────────────────────────────────────────────────────


def uid(hi16: str, lo48: str) -> str:
    """Build a UUID: 00000000-0000-XXXX-0000-XXXXXXXXXXXX.

    hi16 = 4 hex chars (0-9, a-f) for the third group.
    lo48 = 12 hex chars (0-9, a-f) for the last group.
    """
    return f"00000000-0000-{hi16}-0000-{lo48}"


# ── Fixed IDs ──────────────────────────────────────────────────────────────

LAB_NEURO_ID = uid("a001", "000000000001")
LAB_ONCO_ID = uid("a001", "000000000002")

USER_SYSADMIN_ID = uid("b001", "000000000001")
USER_NEURO_ADMIN_ID = uid("b001", "000000000002")
USER_ONCO_ADMIN_ID = uid("b001", "000000000003")
USER_MEMBER_ID = uid("b001", "000000000004")
USER_READONLY_ID = uid("b001", "000000000005")

# Built-in role UUIDs (must match migration 0003_roles.sql)
ROLE_SYSADMIN = "00000000-0000-0000-0000-000000000001"
ROLE_LABADMIN = "00000000-0000-0000-0000-000000000002"
ROLE_MEMBER = "00000000-0000-0000-0000-000000000003"
ROLE_READONLY = "00000000-0000-0000-0000-000000000004"

# JSON constant reused across many inserts
CAPACITY_HINT_JSON = json.dumps({"rows": None, "cols": None, "depth": None})
EMPTY_JSON = json.dumps({})
# The write path stores "no dimensions" as the JSON text `null`
# (dimensions_dump in src/storage/detail/BoxGeometryColumns.h), not `{}` —
# the seed must agree so a seeded database and an API-created one are
# indistinguishable (#138).
NO_DIMENSIONS_JSON = "null"

# Every demo account uses this password (printed at the end). The hash below is
# the one argon2-cffi produces for "admin" with the parameters `hash_password`
# uses; it is embedded so the seed also runs on a python without argon2-cffi
# (CI has none) and so a seeded database is reproducible. A demo credential
# documented in this file's own output is not a secret.
DEMO_PASSWORD = "admin"
DEMO_PASSWORD_HASH = (
    "$argon2id$v=19$m=65536,t=3,p=4$lpIh4H0ORTCO3V5XkO4lDg$"
    "a5uI/ebUhUmw5es+SN6KcJ3GqhVJuBVVDGNknLvvgS8"
)

# The Neuroscience lab is the PHI demo: PHI mode is on there, one of its custom
# fields is tagged PHI, and the built-in SystemAdmin role is granted phi.read so
# the positive and negative accounts are both real demo logins (admin vs. the
# ReadOnly/Member accounts). The values stay synthetic patient research ids.
PHI_LAB_ID = LAB_NEURO_ID
PHI_FIELD_KEY = "diagnosis"

# The columns `freezerctl sample import` reads. `id` is deliberately absent: the
# import path mints it and writes the audit row that names it.
SAMPLE_CSV_COLUMNS = [
    "name",
    "item_type_id",
    "container_type_id",
    "box_id",
    "position_label",
    "volume_value",
    "volume_unit",
    "custom_fields_json",
    "barcode",
    "parent_sample_id",
]

# ── Helpers ────────────────────────────────────────────────────────────────


def now_us() -> int:
    return int(time.time() * 1_000_000)


def hash_password(plaintext: str) -> str:
    try:
        # Imported lazily so the demo hash fallback below works without the
        # optional argon2-cffi package.
        from argon2 import PasswordHasher
        from argon2.low_level import Type
    except ImportError:
        if plaintext != DEMO_PASSWORD:
            raise RuntimeError("argon2-cffi is required to hash a non-demo password") from None
        print("  argon2-cffi not installed; using the precomputed demo password hash")
        return DEMO_PASSWORD_HASH
    ph = PasswordHasher(
        time_cost=3,
        memory_cost=65536,
        parallelism=4,
        hash_len=32,
        type=Type.ID,
    )
    return ph.hash(plaintext)


def find_freezerctl() -> str:
    """Locate the freezerctl binary the audited sample import runs through."""
    override = os.environ.get("FMGR_FREEZERCTL", "")
    if override:
        if not os.path.isfile(override):
            raise SystemExit(f"FMGR_FREEZERCTL={override} is not a file")
        return override
    for build in ("dev", "*"):
        pattern = str(PROJECT_ROOT / "out" / "build" / build / "src" / "cli" / "freezerctl")
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    found = shutil.which("freezerctl")
    if found:
        return found
    raise SystemExit(
        "freezerctl not found: the seed imports samples through its audited write path.\n"
        "Build it first:  cmake --build --preset dev\n"
        "or point FMGR_FREEZERCTL at the binary."
    )


def require_master_kek() -> None:
    if not os.environ.get("FMGR_MASTER_KEK", ""):
        raise SystemExit(
            "FMGR_MASTER_KEK is required: the CSF diagnosis field is PHI, so the seed\n"
            "must encrypt it through the same KMS the demo server will use. Generate a\n"
            "demo key and export the SAME value for the seed and the server:\n"
            "  export FMGR_MASTER_KEK=\"$(openssl rand -base64 32)\""
        )


def run_freezerctl(executable: str, args: list[str]) -> str:
    result = subprocess.run([executable, *args], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"freezerctl {' '.join(args)} failed (exit {result.returncode}):\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


# ── Step 1: Users & Labs ───────────────────────────────────────────────────


def seed_users(conn: sqlite3.Connection, password_hash: str) -> None:
    users = [
        (USER_SYSADMIN_ID, "admin@freezer.local", "System Admin", "active"),
        (USER_NEURO_ADMIN_ID, "wang@neuro.local", "Dr. Wang (Neuro)", "active"),
        (USER_ONCO_ADMIN_ID, "chen@onco.local", "Dr. Chen (Onco)", "active"),
        (USER_MEMBER_ID, "li@neuro.local", "Li Wei (Member)", "active"),
        (USER_READONLY_ID, "zhang@neuro.local", "Zhang Min (ReadOnly)", "active"),
    ]
    auth = json.dumps([{"provider": "local", "hash": password_hash}])
    now = now_us()
    for uid_str, email, name, status in users:
        conn.execute(
            "INSERT INTO users (id, primary_email, display_name, status,"
            " created_at_micros, auth_bindings_json)"
            " VALUES (?, ?, ?, ?, ?, ?)",
            (uid_str, email, name, status, now, auth),
        )

    labs = [
        (LAB_NEURO_ID, "Neuroscience Lab", "neuro-admin@ucdavis.edu",
         json.dumps({"dept": "Neuroscience", "building": "GBSF"}), 1),
        (LAB_ONCO_ID, "Oncology Lab", "onco-admin@ucdavis.edu",
         json.dumps({"dept": "Oncology", "building": "UCDMC"}), 0),
    ]
    for lab_id, name, contact, settings, phi_enabled in labs:
        conn.execute(
            "INSERT INTO labs (id, name, contact, created_at_micros,"
            " settings_json, is_phi_enabled)"
            " VALUES (?, ?, ?, ?, ?, ?)",
            (lab_id, name, contact, now, settings, phi_enabled),
        )

    memberships = [
        (USER_SYSADMIN_ID, LAB_NEURO_ID, ROLE_SYSADMIN),
        (USER_SYSADMIN_ID, LAB_ONCO_ID, ROLE_SYSADMIN),
        (USER_NEURO_ADMIN_ID, LAB_NEURO_ID, ROLE_LABADMIN),
        (USER_ONCO_ADMIN_ID, LAB_ONCO_ID, ROLE_LABADMIN),
        (USER_MEMBER_ID, LAB_NEURO_ID, ROLE_MEMBER),
        (USER_READONLY_ID, LAB_NEURO_ID, ROLE_READONLY),
    ]
    for user_id, lab_id, role_id in memberships:
        conn.execute(
            "INSERT INTO lab_memberships (user_id, lab_id, role_id,"
            " scope_filters_json, joined_at_micros)"
            " VALUES (?, ?, ?, ?, ?)",
            (user_id, lab_id, role_id, EMPTY_JSON, now),
        )

    # phi.read is excluded from every built-in role by default (PRD §3). The demo
    # grants it to SystemAdmin so `admin@freezer.local` is the account that sees
    # the PHI value, while the Member/ReadOnly logins show the gate hiding it.
    conn.execute(
        "INSERT OR IGNORE INTO role_permissions (role_id, permission_key)"
        " VALUES (?, 'phi.read')",
        (ROLE_SYSADMIN,),
    )
    print("  5 users, 2 labs, 6 memberships (Neuro PHI on, SystemAdmin granted phi.read)")


# ── Step 2: Item types ─────────────────────────────────────────────────────


def seed_item_types(conn: sqlite3.Connection) -> dict:
    now = now_us()
    result: dict[str, list[str]] = {LAB_NEURO_ID: [], LAB_ONCO_ID: []}

    neuro_types = [
        ("CSF", None), ("Plasma", None), ("Serum", None),
        ("DNA", None), ("RNA", None), ("Cell Pellet", None), ("Tissue", None),
        ("CSF_Aliquot", "CSF"), ("Plasma_Aliquot", "Plasma"), ("Serum_Aliquot", "Serum"),
    ]
    type_ids: dict[str, str] = {}
    counter = 1
    for name, parent_name in neuro_types:
        tid = uid("c001", f"{counter:012d}")
        parent_id = type_ids.get(parent_name) if parent_name else None
        conn.execute(
            "INSERT INTO item_types (id, lab_id, parent_id, name, created_at_micros)"
            " VALUES (?, ?, ?, ?, ?)",
            (tid, LAB_NEURO_ID, parent_id, name, now),
        )
        type_ids[name] = tid
        result[LAB_NEURO_ID].append(tid)
        counter += 1

    onco_types = [
        ("Tumor Biopsy", None), ("PBMC", None), ("Plasma", None),
        ("DNA", None), ("RNA", None),
        ("Tumor_Aliquot", "Tumor Biopsy"), ("PBMC_Aliquot", "PBMC"),
    ]
    for name, parent_name in onco_types:
        tid = uid("c001", f"{counter:012d}")
        parent_id = type_ids.get(parent_name) if parent_name else None
        conn.execute(
            "INSERT INTO item_types (id, lab_id, parent_id, name, created_at_micros)"
            " VALUES (?, ?, ?, ?, ?)",
            (tid, LAB_ONCO_ID, parent_id, name, now),
        )
        type_ids[name] = tid
        result[LAB_ONCO_ID].append(tid)
        counter += 1

    total = sum(len(v) for v in result.values())
    print(f"  {total} item types across 2 labs")
    return result


# ── Step 3: Container types ────────────────────────────────────────────────


def seed_container_types(conn: sqlite3.Connection, lab_ids: list[str]) -> dict:
    now = now_us()
    cts = [
        ("2 mL Cryovial", "cryovial_2ml", "polypropylene", "CT-2ML"),
        ("50 mL Falcon Tube", "tube_50ml", "polypropylene", "CT-50ML"),
        ("15 mL Falcon Tube", "tube_15ml", "polypropylene", "CT-15ML"),
        ("96-Well Microplate", "microplate_well", "polystyrene", "CT-96WP"),
    ]
    # Per lab: container types belong to a lab, and the audited import path
    # rejects a sample placed with another lab's container type (the raw INSERT
    # this seed used to do did not notice).
    result: dict[str, dict[str, str]] = {lab_id: {} for lab_id in lab_ids}
    counter = 1
    for name, size_class, material, sku in cts:
        for lab_id in lab_ids:
            ctid = uid("d001", f"{counter:012d}")
            conn.execute(
                "INSERT INTO container_types (id, lab_id, name, size_class,"
                " outer_dimensions_json, material, supplier_sku, created_at_micros)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (ctid, lab_id, name, size_class, NO_DIMENSIONS_JSON, material, sku, now),
            )
            result[lab_id][size_class] = ctid
            counter += 1
    print(f"  {len(cts)} container types x {len(lab_ids)} labs")
    return result


# ── Step 4: Freezer hierarchy ──────────────────────────────────────────────


def seed_freezer_hierarchy(conn: sqlite3.Connection) -> dict:
    """Build freezer -> compartment -> shelf -> rack hierarchy."""
    now = now_us()
    result: dict[str, dict] = {}

    # ── Helper to insert a storage container ──
    def insert_container(cid, lab_id, parent_id, kind, name, label, ordering):
        conn.execute(
            "INSERT INTO storage_containers"
            " (id, lab_id, parent_id, kind, name, label, ordering_index,"
            "  capacity_hint_json, created_at_micros)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (cid, lab_id, parent_id, kind, name, label, ordering,
             CAPACITY_HINT_JSON, now),
        )

    # ── Neuroscience Lab Freezer ──
    neuro_freezer_id = uid("e001", "000000000001")
    neuro_root = uid("f001", "000000000001")
    neuro_shelf1 = uid("f001", "000000000002")
    neuro_rack1 = uid("f001", "000000000003")
    neuro_rack2 = uid("f001", "000000000004")
    neuro_shelf2 = uid("f001", "000000000005")
    neuro_rack3 = uid("f001", "000000000006")

    insert_container(neuro_root, LAB_NEURO_ID, None, "compartment",
                     "Neuro Freezer A", "A", 0)
    conn.execute(
        "INSERT INTO freezers (id, lab_id, name, location, model, temp_target_c,"
        " layout_root_id, created_at_micros)"
        " VALUES (?, ?, 'Neuro Freezer A', 'GBSF Room 1201', 'Thermo TSX',"
        " -80.0, ?, ?)",
        (neuro_freezer_id, LAB_NEURO_ID, neuro_root, now),
    )
    insert_container(neuro_shelf1, LAB_NEURO_ID, neuro_root, "shelf",
                     "Shelf 1", "S1", 0)
    insert_container(neuro_rack1, LAB_NEURO_ID, neuro_shelf1, "rack",
                     "Rack 1 (CSF)", "R1", 0)
    insert_container(neuro_rack2, LAB_NEURO_ID, neuro_shelf1, "rack",
                     "Rack 2 (Plasma/Serum)", "R2", 1)
    insert_container(neuro_shelf2, LAB_NEURO_ID, neuro_root, "shelf",
                     "Shelf 2", "S2", 1)
    insert_container(neuro_rack3, LAB_NEURO_ID, neuro_shelf2, "rack",
                     "Rack 3 (DNA/RNA)", "R3", 0)

    result[LAB_NEURO_ID] = {
        "freezer_id": neuro_freezer_id,
        "root_container_id": neuro_root,
        "containers": [neuro_root, neuro_shelf1, neuro_shelf2,
                       neuro_rack1, neuro_rack2, neuro_rack3],
    }

    # ── Oncology Lab Freezer ──
    onco_freezer_id = uid("e001", "000000000002")
    onco_root = uid("f001", "000000000010")
    onco_shelf1 = uid("f001", "000000000011")
    onco_rack1 = uid("f001", "000000000012")

    insert_container(onco_root, LAB_ONCO_ID, None, "compartment",
                     "Onco Freezer B", "B", 0)
    conn.execute(
        "INSERT INTO freezers (id, lab_id, name, location, model, temp_target_c,"
        " layout_root_id, created_at_micros)"
        " VALUES (?, ?, 'Onco Freezer B', 'UCDMC Bldg 2', 'Thermo TSX',"
        " -80.0, ?, ?)",
        (onco_freezer_id, LAB_ONCO_ID, onco_root, now),
    )
    insert_container(onco_shelf1, LAB_ONCO_ID, onco_root, "shelf",
                     "Shelf 1", "S1", 0)
    insert_container(onco_rack1, LAB_ONCO_ID, onco_shelf1, "rack",
                     "Rack 1 (Tumors)", "R1", 0)

    result[LAB_ONCO_ID] = {
        "freezer_id": onco_freezer_id,
        "root_container_id": onco_root,
        "containers": [onco_root, onco_shelf1, onco_rack1],
    }

    print("  2 freezers, 3 shelves, 4 racks")
    return result


# ── Step 5: Box types ──────────────────────────────────────────────────────


def seed_box_types(conn: sqlite3.Connection, lab_ids: list[str]) -> dict:
    now = now_us()
    result: dict[str, dict[str, str]] = {}

    # Keyed by the grid, not by size_class: both templates accept the same
    # cryovial size class, so a size_class key made the 9x9 entry overwrite the
    # 10x10 one and every seeded box came out 10x10 (#143).
    box_templates = [
        ("9x9", "9x9 Cryobox", "cryovial_2ml", 9, 9),
        ("10x10", "10x10 Cryobox", "cryovial_2ml", 10, 10),
    ]

    counter = 1
    for grid, name, size_class, rows, cols in box_templates:
        result[grid] = {}
        for lab_id in lab_ids:
            btid = uid("a101", f"{counter:012d}")
            conn.execute(
                "INSERT INTO box_types (id, lab_id, name, manufacturer, sku,"
                " created_at_micros) VALUES (?, ?, ?, 'Generic', ?, ?)",
                (btid, lab_id, name, f"BT-{counter}", now),
            )
            row_labels = "ABCDEFGHIJ"[:rows]
            for r in range(rows):
                for c in range(cols):
                    label = f"{row_labels[r]}{c + 1}"
                    conn.execute(
                        "INSERT INTO box_type_positions"
                        " (box_type_id, label, row_index, col_index)"
                        " VALUES (?, ?, ?, ?)",
                        (btid, label, r, c),
                    )
                    conn.execute(
                        "INSERT INTO box_type_position_accepts"
                        " (box_type_id, position_label, size_class)"
                        " VALUES (?, ?, ?)",
                        (btid, label, size_class),
                    )
            result[grid][lab_id] = btid
            counter += 1
    print(f"  {len(box_templates)} box types (9x9, 10x10) x {len(lab_ids)} labs")
    return result


# ── Step 6: Boxes ──────────────────────────────────────────────────────────


def seed_boxes(conn, box_types, hierarchy):
    now = now_us()
    result = {LAB_NEURO_ID: [], LAB_ONCO_ID: []}

    neuro_containers = hierarchy[LAB_NEURO_ID]["containers"]
    onco_containers = hierarchy[LAB_ONCO_ID]["containers"]

    # containers[] order: root(0), shelf1(1), shelf2(2), rack1(3), rack2(4), rack3(5)
    # Both grids are represented so the box view can show more than one layout.
    neuro_boxes = [
        ("CSF Bank - Box 1", "9x9", neuro_containers[3]),
        ("Plasma/Serum - Box 1", "10x10", neuro_containers[4]),
        ("DNA/RNA - Box 1", "9x9", neuro_containers[5]),
    ]

    counter = 1
    for label, grid, container_id in neuro_boxes:
        bt_id = box_types[grid][LAB_NEURO_ID]
        box_id = uid("a201", f"{counter:012d}")
        conn.execute(
            "INSERT INTO boxes (id, lab_id, box_type_id, storage_container_id,"
            " label, created_at_micros) VALUES (?, ?, ?, ?, ?, ?)",
            (box_id, LAB_NEURO_ID, bt_id, container_id, label, now),
        )
        result[LAB_NEURO_ID].append(box_id)
        counter += 1

    # Onco
    bt_id = box_types["10x10"][LAB_ONCO_ID]
    box_id = uid("a201", f"{counter:012d}")
    conn.execute(
        "INSERT INTO boxes (id, lab_id, box_type_id, storage_container_id,"
        " label, created_at_micros) VALUES (?, ?, ?, ?, ?, ?)",
        (box_id, LAB_ONCO_ID, bt_id, onco_containers[2], "Tumor Biopsy - Box 1", now),
    )
    result[LAB_ONCO_ID].append(box_id)

    print("  4 boxes placed (2 x 9x9, 2 x 10x10)")
    return result


# ── Step 7: Custom field definitions ───────────────────────────────────────


def seed_custom_field_defs(conn, item_types):
    now = now_us()

    # (lab, item type, key, label, data type, required, is_phi). Exactly one
    # field is PHI: the CSF diagnosis. It is the value the demo's phi.read gate
    # reveals to `admin` and hides from the Member/ReadOnly logins. The values
    # are synthetic trial labels ("Hydrocephalus", "NPH", ...), never a name,
    # MRN or date of birth (AGENTS.md §5).
    fields = [
        (LAB_NEURO_ID, "CSF", "patient_id", "Patient ID", "string", 1, 0),
        (LAB_NEURO_ID, "CSF", "collection_date", "Collection Date", "date", 1, 0),
        (LAB_NEURO_ID, "CSF", "collection_site", "Collection Site", "string", 0, 0),
        (LAB_NEURO_ID, "CSF", "diagnosis", "Diagnosis", "string", 0, 1),
        (LAB_NEURO_ID, "CSF", "volume_ml", "Volume (mL)", "float", 0, 0),
        (LAB_NEURO_ID, "Plasma", "patient_id", "Patient ID", "string", 1, 0),
        (LAB_NEURO_ID, "Plasma", "collection_date", "Collection Date", "date", 1, 0),
        (LAB_NEURO_ID, "Plasma", "diagnosis", "Diagnosis", "string", 0, 0),
        (LAB_NEURO_ID, "Serum", "patient_id", "Patient ID", "string", 1, 0),
        (LAB_NEURO_ID, "Serum", "collection_date", "Collection Date", "date", 1, 0),
        (LAB_NEURO_ID, "DNA", "patient_id", "Patient ID", "string", 1, 0),
        (LAB_NEURO_ID, "DNA", "extraction_date", "Extraction Date", "date", 0, 0),
        (LAB_NEURO_ID, "DNA", "concentration_ng_ul", "Conc (ng/uL)", "float", 0, 0),
        (LAB_NEURO_ID, "RNA", "patient_id", "Patient ID", "string", 1, 0),
        (LAB_NEURO_ID, "RNA", "rin", "RIN", "float", 0, 0),
        (LAB_ONCO_ID, "Tumor Biopsy", "patient_id", "Patient ID", "string", 1, 0),
        (LAB_ONCO_ID, "Tumor Biopsy", "tumor_type", "Tumor Type", "string", 0, 0),
        (LAB_ONCO_ID, "Tumor Biopsy", "biopsy_date", "Biopsy Date", "date", 1, 0),
        (LAB_ONCO_ID, "Tumor Biopsy", "grade", "Tumor Grade", "string", 0, 0),
        (LAB_ONCO_ID, "PBMC", "patient_id", "Patient ID", "string", 1, 0),
        (LAB_ONCO_ID, "PBMC", "collection_date", "Collection Date", "date", 1, 0),
    ]

    neuro_type_map: dict[str, str] = {}
    onco_type_map: dict[str, str] = {}
    cur = conn.execute("SELECT id, lab_id, name FROM item_types")
    for row in cur.fetchall():
        tid, lid, name = row[0], row[1], row[2]
        if lid == LAB_NEURO_ID:
            neuro_type_map[name] = tid
        elif lid == LAB_ONCO_ID:
            onco_type_map[name] = tid

    counter = 1
    for lab_id, it_name, key, label, data_type, required, is_phi in fields:
        it_id = neuro_type_map.get(it_name) if lab_id == LAB_NEURO_ID else onco_type_map.get(it_name)
        if it_id is None:
            continue
        cfd_id = uid("a301", f"{counter:012d}")
        conn.execute(
            "INSERT INTO custom_field_definitions"
            " (id, lab_id, scope_kind, item_type_id, key, label, data_type,"
            "  required, validation_json, indexed, is_phi, created_at_micros)"
            " VALUES (?, ?, 'sample', ?, ?, ?, ?, ?, ?, 0, ?, ?)",
            (cfd_id, lab_id, it_id, key, label, data_type, required,
             EMPTY_JSON, is_phi, now),
        )
        counter += 1
    print(f"  {counter - 1} custom field definitions (1 PHI: CSF {PHI_FIELD_KEY})")


# ── Step 8: Samples ────────────────────────────────────────────────────────


def seed_samples(conn, item_types, boxes, container_types):
    """Import the demo sample graph through freezerctl's audited write path.

    The rows are built here, then written by `freezerctl sample import`: the
    storage path that records one `sample.insert` audit event per sample and
    encrypts PHI-tagged values in the same transaction. Raw SQLite INSERTs
    bypassed both, which is exactly the demo gap #143 closes.

    Two passes: parents/standalone samples first, then their aliquots with the
    parent ids looked up by name — the import path mints ids, so the seed cannot
    name them up front. The direct-SQL sections are committed before the first
    import so the CLI's own connection can take the SQLite write lock.
    """
    freezerctl = find_freezerctl()
    require_master_kek()

    neuro_type_map: dict[str, str] = {}
    onco_type_map: dict[str, str] = {}
    cur = conn.execute("SELECT id, lab_id, name FROM item_types")
    for row in cur.fetchall():
        tid, lid, name = row[0], row[1], row[2]
        if lid == LAB_NEURO_ID:
            neuro_type_map[name] = tid
        elif lid == LAB_ONCO_ID:
            onco_type_map[name] = tid

    rows: list[dict] = []

    def add_sample(lab_id, it_id, name, box_id, position, volume_ul,
                   parent_name, custom_fields, barcode=None):
        rows.append({
            "lab_id": lab_id,
            "name": name,
            "item_type_id": it_id,
            "container_type_id": container_types[lab_id]["cryovial_2ml"],
            "box_id": box_id,
            "position_label": position,
            "volume_value": str(volume_ul),
            "volume_unit": "µL",
            "custom_fields_json": json.dumps(custom_fields) if custom_fields else EMPTY_JSON,
            "barcode": barcode or "",
            "parent_name": parent_name or "",
        })
        return name

    def import_pass(label, pass_rows):
        by_lab: dict[str, list[dict]] = {}
        for row in pass_rows:
            by_lab.setdefault(row["lab_id"], []).append(row)
        for lab_id, lab_rows in by_lab.items():
            payload = [
                {column: row.get(column, "") for column in SAMPLE_CSV_COLUMNS}
                for row in lab_rows
            ]
            with tempfile.NamedTemporaryFile(
                    "w", suffix=".csv", encoding="utf-8", newline="", delete=False) as handle:
                csv_path = handle.name
                writer = csv.DictWriter(handle, fieldnames=SAMPLE_CSV_COLUMNS)
                writer.writeheader()
                writer.writerows(payload)
            try:
                output = run_freezerctl(freezerctl, [
                    "sample", "import", "--sqlite", DB_PATH, "--lab", lab_id,
                    "--actor", USER_SYSADMIN_ID, csv_path])
            finally:
                os.unlink(csv_path)
            imported = next(
                (line for line in output.splitlines() if line.startswith("imported ")),
                f"{len(lab_rows)} sample(s)",
            )
            print(f"  {label}: {imported}")

    neuro_box1 = boxes[LAB_NEURO_ID][0]  # CSF Bank - Box 1 (9x9)
    neuro_box2 = boxes[LAB_NEURO_ID][1]  # Plasma/Serum - Box 1 (10x10)
    neuro_box3 = boxes[LAB_NEURO_ID][2]  # DNA/RNA - Box 1 (9x9)

    csf_type = neuro_type_map["CSF"]
    csf_a_type = neuro_type_map["CSF_Aliquot"]
    plasma_type = neuro_type_map["Plasma"]
    p_a_type = neuro_type_map["Plasma_Aliquot"]
    serum_type = neuro_type_map["Serum"]
    dna_type = neuro_type_map["DNA"]
    rna_type = neuro_type_map["RNA"]

    # ── Patient P001: Hydrocephalus ──
    p001 = add_sample(
        LAB_NEURO_ID, csf_type,
        "P001-CSF-01", neuro_box1, "A1", 2000, None,
        {"patient_id": "P001", "collection_date": "2025-11-15",
         "collection_site": "Lumbar", "diagnosis": "Hydrocephalus"},
        "P001-CSF-01",
    )
    for i, pos in enumerate(["A2", "A3", "A4"], 1):
        add_sample(
            LAB_NEURO_ID, csf_a_type,
            f"P001-CSF-01-A{i}", neuro_box1, pos, 500, p001,
            {"patient_id": "P001", "collection_date": "2025-11-15"},
        )

    # ── Patient P002: NPH ──
    p002 = add_sample(
        LAB_NEURO_ID, csf_type,
        "P002-CSF-01", neuro_box1, "B1", 1800, None,
        {"patient_id": "P002", "collection_date": "2025-12-03",
         "collection_site": "Lumbar", "diagnosis": "NPH"},
        "P002-CSF-01",
    )
    for i, pos in enumerate(["B2", "B3"], 1):
        add_sample(
            LAB_NEURO_ID, csf_a_type,
            f"P002-CSF-01-A{i}", neuro_box1, pos, 500, p002,
            {"patient_id": "P002", "collection_date": "2025-12-03"},
        )

    # ── Patient P003: iNPH ──
    add_sample(
        LAB_NEURO_ID, csf_type,
        "P003-CSF-01", neuro_box1, "C1", 1500, None,
        {"patient_id": "P003", "collection_date": "2026-01-20",
         "collection_site": "Ventricular", "diagnosis": "iNPH"},
        "P003-CSF-01",
    )
    p003_plasma = add_sample(
        LAB_NEURO_ID, plasma_type,
        "P003-Plasma-01", neuro_box2, "A1", 1000, None,
        {"patient_id": "P003", "collection_date": "2026-01-20"},
        "P003-PL-01",
    )
    add_sample(
        LAB_NEURO_ID, serum_type,
        "P003-Serum-01", neuro_box2, "A2", 1000, None,
        {"patient_id": "P003", "collection_date": "2026-01-20"},
        "P003-SE-01",
    )
    for i, pos in enumerate(["A3", "A4"], 1):
        add_sample(
            LAB_NEURO_ID, p_a_type,
            f"P003-Plasma-01-A{i}", neuro_box2, pos, 300, p003_plasma,
            {"patient_id": "P003", "collection_date": "2026-01-20"},
        )

    # ── Patient P004: Control ──
    p004 = add_sample(
        LAB_NEURO_ID, csf_type,
        "P004-CSF-01", neuro_box1, "D1", 2200, None,
        {"patient_id": "P004", "collection_date": "2026-03-10",
         "collection_site": "Lumbar", "diagnosis": "Control"},
        "P004-CSF-01",
    )
    for i, pos in enumerate(["D2", "D3"], 1):
        add_sample(
            LAB_NEURO_ID, csf_a_type,
            f"P004-CSF-01-A{i}", neuro_box1, pos, 500, p004,
            {"patient_id": "P004", "collection_date": "2026-03-10"},
        )

    # ── DNA/RNA ──
    add_sample(LAB_NEURO_ID, dna_type,
                  "P001-DNA-01", neuro_box3, "A1", 50, None,
                  {"patient_id": "P001", "extraction_date": "2025-11-16",
                   "concentration_ng_ul": 125.0}, "P001-DNA-01")
    add_sample(LAB_NEURO_ID, rna_type,
                  "P001-RNA-01", neuro_box3, "A2", 30, None,
                  {"patient_id": "P001", "rin": 8.2}, "P001-RNA-01")
    add_sample(LAB_NEURO_ID, dna_type,
                  "P002-DNA-01", neuro_box3, "B1", 45, None,
                  {"patient_id": "P002", "extraction_date": "2025-12-04",
                   "concentration_ng_ul": 98.0}, "P002-DNA-01")
    add_sample(LAB_NEURO_ID, dna_type,
                  "P003-DNA-01", neuro_box3, "C1", 55, None,
                  {"patient_id": "P003", "extraction_date": "2026-01-21",
                   "concentration_ng_ul": 210.0}, "P003-DNA-01")
    add_sample(LAB_NEURO_ID, rna_type,
                  "P003-RNA-01", neuro_box3, "C2", 28, None,
                  {"patient_id": "P003", "rin": 9.1}, "P003-RNA-01")

    # ── Onco: Tumor biopsies ──
    onco_box1 = boxes[LAB_ONCO_ID][0]
    tumor_type = onco_type_map["Tumor Biopsy"]
    tumor_a_type = onco_type_map["Tumor_Aliquot"]
    pbmc_type = onco_type_map["PBMC"]
    pbmc_a_type = onco_type_map["PBMC_Aliquot"]
    onco_dna_type = onco_type_map["DNA"]
    onco_rna_type = onco_type_map["RNA"]

    # T001: GBM
    t001 = add_sample(
        LAB_ONCO_ID, tumor_type,
        "T001-GBM-01", onco_box1, "A1", 500, None,
        {"patient_id": "T001", "tumor_type": "Glioblastoma",
         "biopsy_date": "2026-01-05", "grade": "IV"},
        "T001-GBM-01",
    )
    for i, pos in enumerate(["A2", "A3", "A4"], 1):
        add_sample(
            LAB_ONCO_ID, tumor_a_type,
            f"T001-GBM-01-A{i}", onco_box1, pos, 100, t001,
            {"patient_id": "T001", "tumor_type": "Glioblastoma",
             "biopsy_date": "2026-01-05"},
        )
    t001_pbmc = add_sample(
        LAB_ONCO_ID, pbmc_type,
        "T001-PBMC-01", onco_box1, "C1", 2000, None,
        {"patient_id": "T001", "collection_date": "2026-01-05"},
        "T001-PBMC-01",
    )
    for i, pos in enumerate(["C2", "C3"], 1):
        add_sample(
            LAB_ONCO_ID, pbmc_a_type,
            f"T001-PBMC-01-A{i}", onco_box1, pos, 500, t001_pbmc,
            {"patient_id": "T001", "collection_date": "2026-01-05"},
        )
    add_sample(LAB_ONCO_ID, onco_dna_type,
                  "T001-DNA-01", onco_box1, "D1", 40, None,
                  {"patient_id": "T001", "extraction_date": "2026-01-06",
                   "concentration_ng_ul": 180.0}, "T001-DNA-01")
    add_sample(LAB_ONCO_ID, onco_rna_type,
                  "T001-RNA-01", onco_box1, "D2", 25, None,
                  {"patient_id": "T001", "rin": 7.8}, "T001-RNA-01")

    # T002: BRCA
    t002 = add_sample(
        LAB_ONCO_ID, tumor_type,
        "T002-BRCA-01", onco_box1, "B1", 600, None,
        {"patient_id": "T002", "tumor_type": "Breast Adenocarcinoma",
         "biopsy_date": "2026-02-18", "grade": "III"},
        "T002-BRCA-01",
    )
    for i, pos in enumerate(["B2", "B3"], 1):
        add_sample(
            LAB_ONCO_ID, tumor_a_type,
            f"T002-BRCA-01-A{i}", onco_box1, pos, 150, t002,
            {"patient_id": "T002", "tumor_type": "Breast Adenocarcinoma",
             "biopsy_date": "2026-02-18"},
        )

    # The direct-SQL half must be durable before freezerctl opens the file.
    conn.commit()

    base_rows = [row for row in rows if not row["parent_name"]]
    child_rows = [row for row in rows if row["parent_name"]]
    import_pass("parents", base_rows)

    ids: dict[tuple[str, str], str] = {}
    for lab_id in (LAB_NEURO_ID, LAB_ONCO_ID):
        for name, sample_id in conn.execute(
                "SELECT name, id FROM samples WHERE lab_id = ?", (lab_id,)):
            ids[(lab_id, name)] = sample_id
    for row in child_rows:
        row["parent_sample_id"] = ids[(row["lab_id"], row["parent_name"])]
    import_pass("aliquots", child_rows)

    count = len(rows)
    print(f"  {count} samples imported through the audited path (parents + aliquots)")
    return count


# ── Main ───────────────────────────────────────────────────────────────────


def main():
    print("=" * 60)
    print("FreezerManager Demo Data Seeder")
    print(f"DB: {DB_PATH}")
    print("NOTE: migrations must be applied by freezerd before running this.")
    print("=" * 60)

    # Fail before writing anything if the audited import path is unavailable:
    # a half-written demo database would be worse than a clean error.
    freezerctl = find_freezerctl()
    require_master_kek()

    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")

    pw_hash = hash_password(DEMO_PASSWORD)

    print("\n[1/8] Users & Labs...")
    seed_users(conn, pw_hash)

    print("\n[2/8] Item Types...")
    item_types = seed_item_types(conn)

    print("\n[3/8] Container Types...")
    container_types = seed_container_types(conn, [LAB_NEURO_ID, LAB_ONCO_ID])

    print("\n[4/8] Freezer Hierarchy...")
    hierarchy = seed_freezer_hierarchy(conn)

    print("\n[5/8] Box Types...")
    box_types = seed_box_types(conn, [LAB_NEURO_ID, LAB_ONCO_ID])

    print("\n[6/8] Boxes...")
    boxes = seed_boxes(conn, box_types, hierarchy)

    print("\n[7/8] Custom Fields...")
    seed_custom_field_defs(conn, item_types)

    print("\n[8/8] Samples (imported through the audited write path)...")
    seed_samples(conn, item_types, boxes, container_types)

    conn.commit()

    sample_count = conn.execute("SELECT count(*) FROM samples").fetchone()[0]
    audit_count = conn.execute("SELECT count(*) FROM audit_events").fetchone()[0]
    phi_count = conn.execute(
        "SELECT count(*) FROM samples WHERE lab_id = ? AND phi_fields_enc_json <> '{}'",
        (PHI_LAB_ID,),
    ).fetchone()[0]
    grid_counts = conn.execute(
        "SELECT bt.name, count(*) FROM boxes b"
        " JOIN box_types bt ON bt.id = b.box_type_id"
        " GROUP BY bt.name ORDER BY bt.name"
    ).fetchall()
    conn.close()

    print("\n[audit] Verifying the hash chain...")
    print("  " + run_freezerctl(freezerctl, ["audit", "verify", "--sqlite", DB_PATH]).strip())

    print()
    print("=" * 60)
    print("Demo database seeded successfully!")
    print(f"DB: {DB_PATH}")
    print(f"  {sample_count} samples, {audit_count} audit events (one insert row per sample)")
    print(f"  PHI: Neuroscience Lab enabled, {phi_count} encrypted sample(s), CSF "
          f"`{PHI_FIELD_KEY}` tagged")
    print(f"  box grids in use: " + ", ".join(f"{name} x{count}" for name, count in grid_counts))
    print()
    print("Login credentials (all use password: admin):")
    print("  SystemAdmin:  admin@freezer.local      (has phi.read: sees diagnosis)")
    print("  Neuro Admin:  wang@neuro.local")
    print("  Onco Admin:   chen@onco.local")
    print("  Neuro Member: li@neuro.local           (no phi.read: diagnosis hidden)")
    print("  Read-Only:    zhang@neuro.local        (no phi.read: diagnosis hidden)")
    print()
    print("Start the server with the same master key you seeded with:")
    print(f"  FMGR_MASTER_KEK='...' FMGR_DB_PATH={DB_PATH} ./out/build/dev/src/server/freezerd")
    print("=" * 60)


if __name__ == "__main__":
    main()
