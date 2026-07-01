#!/usr/bin/env python3
"""Comprehensive demo data seeder for FreezerManager.

Generates a rich demo database with:
- 2 labs (Neuroscience Lab, Oncology Lab)
- 5 users with different roles (SystemAdmin, 2x LabAdmin, Member, ReadOnly)
- Freezer hierarchy with compartments, shelves, racks
- Multiple box types (9x9 cryobox, 10x10 cryobox)
- Container types (cryovial, tube, microplate)
- Item type taxonomy (CSF, Plasma, Serum, DNA, RNA, Cell Pellet, Tissue)
- 32 samples: 20 Neuro (CSF patients + aliquots + DNA/RNA), 12 Onco (tumor biopsies + aliquots)
- Custom field definitions (patient_id, collection_date, diagnosis, etc.)

Usage:
    # 1. Start freezerd once to run migrations on a fresh DB:
    #    FMGR_DB_PATH=data/freezer.db ./out/build/dev/src/server/freezerd
    #    (kill it after it starts)

    # 2. Seed:
    #    python3 scripts/seed_demo.py

    # 3. Restart server:
    #    FMGR_DB_PATH=data/freezer.db ./out/build/dev/src/server/freezerd

The DB path defaults to data/freezer.db.
Set FREEZER_DB_PATH env var to override.
"""

import json
import os
import sqlite3
import time
from pathlib import Path
from typing import Optional

from argon2 import PasswordHasher
from argon2.low_level import Type

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

# ── Helpers ────────────────────────────────────────────────────────────────


def now_us() -> int:
    return int(time.time() * 1_000_000)


def hash_password(plaintext: str) -> str:
    ph = PasswordHasher(
        time_cost=3,
        memory_cost=65536,
        parallelism=4,
        hash_len=32,
        type=Type.ID,
    )
    return ph.hash(plaintext)


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
         json.dumps({"dept": "Neuroscience", "building": "GBSF"})),
        (LAB_ONCO_ID, "Oncology Lab", "onco-admin@ucdavis.edu",
         json.dumps({"dept": "Oncology", "building": "UCDMC"})),
    ]
    for lab_id, name, contact, settings in labs:
        conn.execute(
            "INSERT INTO labs (id, name, contact, created_at_micros,"
            " settings_json, is_phi_enabled)"
            " VALUES (?, ?, ?, ?, ?, 0)",
            (lab_id, name, contact, now, settings),
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
    print("  5 users, 2 labs, 6 memberships")


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
    result: dict[str, str] = {}
    counter = 1
    for name, size_class, material, sku in cts:
        for lab_id in lab_ids:
            ctid = uid("d001", f"{counter:012d}")
            conn.execute(
                "INSERT INTO container_types (id, lab_id, name, size_class,"
                " outer_dimensions_json, material, supplier_sku, created_at_micros)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (ctid, lab_id, name, size_class, EMPTY_JSON, material, sku, now),
            )
            if lab_id == lab_ids[0]:
                result[size_class] = ctid
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

    box_templates = [
        ("9x9 Cryobox", "cryovial_2ml", 9, 9),
        ("10x10 Cryobox", "cryovial_2ml", 10, 10),
    ]

    counter = 1
    for name, size_class, rows, cols in box_templates:
        result[size_class] = {}
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
            result[size_class][lab_id] = btid
            counter += 1
    print(f"  {len(box_templates)} box types x {len(lab_ids)} labs")
    return result


# ── Step 6: Boxes ──────────────────────────────────────────────────────────


def seed_boxes(conn, box_types, hierarchy):
    now = now_us()
    result = {LAB_NEURO_ID: [], LAB_ONCO_ID: []}

    neuro_containers = hierarchy[LAB_NEURO_ID]["containers"]
    onco_containers = hierarchy[LAB_ONCO_ID]["containers"]

    # containers[] order: root(0), shelf1(1), shelf2(2), rack1(3), rack2(4), rack3(5)
    neuro_boxes = [
        ("CSF Bank - Box 1", "cryovial_2ml", neuro_containers[3]),
        ("Plasma/Serum - Box 1", "cryovial_2ml", neuro_containers[4]),
        ("DNA/RNA - Box 1", "cryovial_2ml", neuro_containers[5]),
    ]

    counter = 1
    for label, size_class, container_id in neuro_boxes:
        bt_id = box_types[size_class][LAB_NEURO_ID]
        box_id = uid("a201", f"{counter:012d}")
        conn.execute(
            "INSERT INTO boxes (id, lab_id, box_type_id, storage_container_id,"
            " label, created_at_micros) VALUES (?, ?, ?, ?, ?, ?)",
            (box_id, LAB_NEURO_ID, bt_id, container_id, label, now),
        )
        result[LAB_NEURO_ID].append(box_id)
        counter += 1

    # Onco
    bt_id = box_types["cryovial_2ml"][LAB_ONCO_ID]
    box_id = uid("a201", f"{counter:012d}")
    conn.execute(
        "INSERT INTO boxes (id, lab_id, box_type_id, storage_container_id,"
        " label, created_at_micros) VALUES (?, ?, ?, ?, ?, ?)",
        (box_id, LAB_ONCO_ID, bt_id, onco_containers[2], "Tumor Biopsy - Box 1", now),
    )
    result[LAB_ONCO_ID].append(box_id)

    print("  4 boxes placed")
    return result


# ── Step 7: Custom field definitions ───────────────────────────────────────


def seed_custom_field_defs(conn, item_types):
    now = now_us()

    fields = [
        (LAB_NEURO_ID, "CSF", "patient_id", "Patient ID", "string", 1),
        (LAB_NEURO_ID, "CSF", "collection_date", "Collection Date", "date", 1),
        (LAB_NEURO_ID, "CSF", "collection_site", "Collection Site", "string", 0),
        (LAB_NEURO_ID, "CSF", "diagnosis", "Diagnosis", "string", 0),
        (LAB_NEURO_ID, "CSF", "volume_ml", "Volume (mL)", "float", 0),
        (LAB_NEURO_ID, "Plasma", "patient_id", "Patient ID", "string", 1),
        (LAB_NEURO_ID, "Plasma", "collection_date", "Collection Date", "date", 1),
        (LAB_NEURO_ID, "Plasma", "diagnosis", "Diagnosis", "string", 0),
        (LAB_NEURO_ID, "Serum", "patient_id", "Patient ID", "string", 1),
        (LAB_NEURO_ID, "Serum", "collection_date", "Collection Date", "date", 1),
        (LAB_NEURO_ID, "DNA", "patient_id", "Patient ID", "string", 1),
        (LAB_NEURO_ID, "DNA", "extraction_date", "Extraction Date", "date", 0),
        (LAB_NEURO_ID, "DNA", "concentration_ng_ul", "Conc (ng/uL)", "float", 0),
        (LAB_NEURO_ID, "RNA", "patient_id", "Patient ID", "string", 1),
        (LAB_NEURO_ID, "RNA", "rin", "RIN", "float", 0),
        (LAB_ONCO_ID, "Tumor Biopsy", "patient_id", "Patient ID", "string", 1),
        (LAB_ONCO_ID, "Tumor Biopsy", "tumor_type", "Tumor Type", "string", 0),
        (LAB_ONCO_ID, "Tumor Biopsy", "biopsy_date", "Biopsy Date", "date", 1),
        (LAB_ONCO_ID, "Tumor Biopsy", "grade", "Tumor Grade", "string", 0),
        (LAB_ONCO_ID, "PBMC", "patient_id", "Patient ID", "string", 1),
        (LAB_ONCO_ID, "PBMC", "collection_date", "Collection Date", "date", 1),
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
    for lab_id, it_name, key, label, data_type, required in fields:
        it_id = neuro_type_map.get(it_name) if lab_id == LAB_NEURO_ID else onco_type_map.get(it_name)
        if it_id is None:
            continue
        cfd_id = uid("a301", f"{counter:012d}")
        conn.execute(
            "INSERT INTO custom_field_definitions"
            " (id, lab_id, scope_kind, item_type_id, key, label, data_type,"
            "  required, validation_json, indexed, is_phi, created_at_micros)"
            " VALUES (?, ?, 'sample', ?, ?, ?, ?, ?, ?, 0, 0, ?)",
            (cfd_id, lab_id, it_id, key, label, data_type, required,
             EMPTY_JSON, now),
        )
        counter += 1
    print(f"  {counter - 1} custom field definitions")


# ── Step 8: Samples ────────────────────────────────────────────────────────


def seed_samples(conn, item_types, boxes, container_types):
    now = now_us()
    sample_counter = 1

    neuro_type_map: dict[str, str] = {}
    onco_type_map: dict[str, str] = {}
    cur = conn.execute("SELECT id, lab_id, name FROM item_types")
    for row in cur.fetchall():
        tid, lid, name = row[0], row[1], row[2]
        if lid == LAB_NEURO_ID:
            neuro_type_map[name] = tid
        elif lid == LAB_ONCO_ID:
            onco_type_map[name] = tid

    cryovial_ct_id = container_types["cryovial_2ml"]

    def insert_sample(sample_id, lab_id, it_id, name, box_id, position,
                      volume_ul, parent_id, custom_fields, barcode=None):
        nonlocal sample_counter
        created_by = USER_SYSADMIN_ID
        cf_json = json.dumps(custom_fields) if custom_fields else EMPTY_JSON
        conn.execute(
            "INSERT INTO samples"
            " (id, lab_id, item_type_id, name, barcode, container_type_id,"
            "  box_id, position_label, volume_value, volume_unit,"
            "  status, parent_sample_id,"
            "  created_by, created_at_micros,"
            "  last_modified_by, last_modified_at_micros,"
            "  custom_fields_json, phi_fields_enc_json)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'µL',"
            "  'active', ?, ?, ?, ?, ?, ?, '{}')",
            (sample_id, lab_id, it_id, name, barcode, cryovial_ct_id,
             box_id, position, volume_ul, parent_id,
             created_by, now, created_by, now, cf_json),
        )
        sample_counter += 1
        return sample_id

    neuro_box1 = boxes[LAB_NEURO_ID][0]  # CSF Bank - Box 1 (9x9)
    neuro_box2 = boxes[LAB_NEURO_ID][1]  # Plasma/Serum - Box 1
    neuro_box3 = boxes[LAB_NEURO_ID][2]  # DNA/RNA - Box 1 (10x10)

    csf_type = neuro_type_map["CSF"]
    csf_a_type = neuro_type_map["CSF_Aliquot"]
    plasma_type = neuro_type_map["Plasma"]
    p_a_type = neuro_type_map["Plasma_Aliquot"]
    serum_type = neuro_type_map["Serum"]
    dna_type = neuro_type_map["DNA"]
    rna_type = neuro_type_map["RNA"]

    # ── Patient P001: Hydrocephalus ──
    p001 = insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, csf_type,
        "P001-CSF-01", neuro_box1, "A1", 2000, None,
        {"patient_id": "P001", "collection_date": "2025-11-15",
         "collection_site": "Lumbar", "diagnosis": "Hydrocephalus"},
        "P001-CSF-01",
    )
    for i, pos in enumerate(["A2", "A3", "A4"], 1):
        insert_sample(
            uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, csf_a_type,
            f"P001-CSF-01-A{i}", neuro_box1, pos, 500, p001,
            {"patient_id": "P001", "collection_date": "2025-11-15"},
        )

    # ── Patient P002: NPH ──
    p002 = insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, csf_type,
        "P002-CSF-01", neuro_box1, "B1", 1800, None,
        {"patient_id": "P002", "collection_date": "2025-12-03",
         "collection_site": "Lumbar", "diagnosis": "NPH"},
        "P002-CSF-01",
    )
    for i, pos in enumerate(["B2", "B3"], 1):
        insert_sample(
            uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, csf_a_type,
            f"P002-CSF-01-A{i}", neuro_box1, pos, 500, p002,
            {"patient_id": "P002", "collection_date": "2025-12-03"},
        )

    # ── Patient P003: iNPH ──
    insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, csf_type,
        "P003-CSF-01", neuro_box1, "C1", 1500, None,
        {"patient_id": "P003", "collection_date": "2026-01-20",
         "collection_site": "Ventricular", "diagnosis": "iNPH"},
        "P003-CSF-01",
    )
    p003_plasma = insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, plasma_type,
        "P003-Plasma-01", neuro_box2, "A1", 1000, None,
        {"patient_id": "P003", "collection_date": "2026-01-20"},
        "P003-PL-01",
    )
    insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, serum_type,
        "P003-Serum-01", neuro_box2, "A2", 1000, None,
        {"patient_id": "P003", "collection_date": "2026-01-20"},
        "P003-SE-01",
    )
    for i, pos in enumerate(["A3", "A4"], 1):
        insert_sample(
            uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, p_a_type,
            f"P003-Plasma-01-A{i}", neuro_box2, pos, 300, p003_plasma,
            {"patient_id": "P003", "collection_date": "2026-01-20"},
        )

    # ── Patient P004: Control ──
    p004 = insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, csf_type,
        "P004-CSF-01", neuro_box1, "D1", 2200, None,
        {"patient_id": "P004", "collection_date": "2026-03-10",
         "collection_site": "Lumbar", "diagnosis": "Control"},
        "P004-CSF-01",
    )
    for i, pos in enumerate(["D2", "D3"], 1):
        insert_sample(
            uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, csf_a_type,
            f"P004-CSF-01-A{i}", neuro_box1, pos, 500, p004,
            {"patient_id": "P004", "collection_date": "2026-03-10"},
        )

    # ── DNA/RNA ──
    insert_sample(uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, dna_type,
                  "P001-DNA-01", neuro_box3, "A1", 50, None,
                  {"patient_id": "P001", "extraction_date": "2025-11-16",
                   "concentration_ng_ul": 125.0}, "P001-DNA-01")
    insert_sample(uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, rna_type,
                  "P001-RNA-01", neuro_box3, "A2", 30, None,
                  {"patient_id": "P001", "rin": 8.2}, "P001-RNA-01")
    insert_sample(uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, dna_type,
                  "P002-DNA-01", neuro_box3, "B1", 45, None,
                  {"patient_id": "P002", "extraction_date": "2025-12-04",
                   "concentration_ng_ul": 98.0}, "P002-DNA-01")
    insert_sample(uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, dna_type,
                  "P003-DNA-01", neuro_box3, "C1", 55, None,
                  {"patient_id": "P003", "extraction_date": "2026-01-21",
                   "concentration_ng_ul": 210.0}, "P003-DNA-01")
    insert_sample(uid("b101", f"{sample_counter:012d}"), LAB_NEURO_ID, rna_type,
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
    t001 = insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, tumor_type,
        "T001-GBM-01", onco_box1, "A1", 500, None,
        {"patient_id": "T001", "tumor_type": "Glioblastoma",
         "biopsy_date": "2026-01-05", "grade": "IV"},
        "T001-GBM-01",
    )
    for i, pos in enumerate(["A2", "A3", "A4"], 1):
        insert_sample(
            uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, tumor_a_type,
            f"T001-GBM-01-A{i}", onco_box1, pos, 100, t001,
            {"patient_id": "T001", "tumor_type": "Glioblastoma"},
        )
    t001_pbmc = insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, pbmc_type,
        "T001-PBMC-01", onco_box1, "C1", 2000, None,
        {"patient_id": "T001", "collection_date": "2026-01-05"},
        "T001-PBMC-01",
    )
    for i, pos in enumerate(["C2", "C3"], 1):
        insert_sample(
            uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, pbmc_a_type,
            f"T001-PBMC-01-A{i}", onco_box1, pos, 500, t001_pbmc,
            {"patient_id": "T001", "collection_date": "2026-01-05"},
        )
    insert_sample(uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, onco_dna_type,
                  "T001-DNA-01", onco_box1, "D1", 40, None,
                  {"patient_id": "T001", "extraction_date": "2026-01-06",
                   "concentration_ng_ul": 180.0}, "T001-DNA-01")
    insert_sample(uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, onco_rna_type,
                  "T001-RNA-01", onco_box1, "D2", 25, None,
                  {"patient_id": "T001", "rin": 7.8}, "T001-RNA-01")

    # T002: BRCA
    t002 = insert_sample(
        uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, tumor_type,
        "T002-BRCA-01", onco_box1, "B1", 600, None,
        {"patient_id": "T002", "tumor_type": "Breast Adenocarcinoma",
         "biopsy_date": "2026-02-18", "grade": "III"},
        "T002-BRCA-01",
    )
    for i, pos in enumerate(["B2", "B3"], 1):
        insert_sample(
            uid("b101", f"{sample_counter:012d}"), LAB_ONCO_ID, tumor_a_type,
            f"T002-BRCA-01-A{i}", onco_box1, pos, 150, t002,
            {"patient_id": "T002", "tumor_type": "Breast Adenocarcinoma"},
        )

    count = sample_counter - 1
    print(f"  {count} samples (patients + aliquots + DNA/RNA)")


# ── Main ───────────────────────────────────────────────────────────────────


def main():
    print("=" * 60)
    print("FreezerManager Demo Data Seeder")
    print(f"DB: {DB_PATH}")
    print("NOTE: migrations must be applied by freezerd before running this.")
    print("=" * 60)

    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")

    pw_hash = hash_password("admin")

    steps = [
        ("Users & Labs", lambda: seed_users(conn, pw_hash)),
        ("Item Types", lambda: seed_item_types(conn)),
        ("Container Types", lambda: seed_container_types(conn, [LAB_NEURO_ID, LAB_ONCO_ID])),
        ("Freezer Hierarchy", lambda: seed_freezer_hierarchy(conn)),
        ("Box Types", lambda: seed_box_types(conn, [LAB_NEURO_ID, LAB_ONCO_ID])),
        ("Boxes", lambda: seed_boxes(conn, box_types, hierarchy)),
        ("Custom Fields", lambda: seed_custom_field_defs(conn, item_types)),
        ("Samples", lambda: seed_samples(conn, item_types, boxes, container_types)),
    ]

    # Run steps 1-4 first so we can capture their return values
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

    print("\n[8/8] Samples...")
    seed_samples(conn, item_types, boxes, container_types)

    conn.commit()
    conn.close()

    print()
    print("=" * 60)
    print("Demo database seeded successfully!")
    print(f"DB: {DB_PATH}")
    print()
    print("Login credentials (all use password: admin):")
    print("  SystemAdmin:  admin@freezer.local")
    print("  Neuro Admin:  wang@neuro.local")
    print("  Onco Admin:   chen@onco.local")
    print("  Neuro Member: li@neuro.local")
    print("  Read-Only:    zhang@neuro.local")
    print()
    print("Start the server:")
    print(f"  FMGR_DB_PATH={DB_PATH} ./out/build/dev/src/server/freezerd")
    print("=" * 60)


if __name__ == "__main__":
    main()
