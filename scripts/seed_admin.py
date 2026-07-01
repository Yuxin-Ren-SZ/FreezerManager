#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Seed an admin user into a FreezerManager SQLite database.

Usage: python3 seed_admin.py [db_path] [email] [password]
Default: data/freezer.db, admin@freezer.local, admin
"""

import sqlite3
import sys
import time
from pathlib import Path

from argon2 import PasswordHasher
from argon2.low_level import Type


def apply_migration(conn: sqlite3.Connection, sql_path: Path) -> None:
    """Apply one migration SQL file."""
    sql = sql_path.read_text()
    conn.executescript(sql)


def main() -> None:
    # Default paths
    project_root = Path(__file__).resolve().parent.parent
    db_path = sys.argv[1] if len(sys.argv) > 1 else str(project_root / "data" / "freezer.db")
    email = sys.argv[2] if len(sys.argv) > 2 else "admin@freezer.local"
    password = sys.argv[3] if len(sys.argv) > 3 else "admin"

    db_file = Path(db_path)
    db_file.parent.mkdir(parents=True, exist_ok=True)

    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")

    # Note: migrations are applied by freezerd itself. This script assumes
    # the DB was already migrated by starting the server once with FMGR_DB_PATH.
    # It only inserts the seed data (admin user + lab + membership).

    # Hash password using Argon2id (64 MiB, ops=3 — matches LocalAuthProvider default)
    ph = PasswordHasher(
        time_cost=3,           # opslimit
        memory_cost=65536,     # 64 MiB in KiB
        parallelism=4,
        hash_len=32,
        type=Type.ID,
    )
    password_hash = ph.hash(password)
    print(f"Password hash: {password_hash[:60]}...")

    # UUIDs (same pattern as e2e smoke test)
    uid = "10000000-0000-0000-0000-000000000001"
    lab_id = "20000000-0000-0000-0000-000000000001"
    sysadmin_role_id = "00000000-0000-0000-0000-000000000001"
    now_micros = int(time.time() * 1_000_000)
    zero_micros = 1

    import json

    # Insert lab
    conn.execute(
        """INSERT INTO labs (id, name, contact, created_at_micros, settings_json, is_phi_enabled)
           VALUES (?, ?, ?, ?, ?, 0)""",
        (lab_id, "Default Lab", "admin@freezer.local", zero_micros, json.dumps({})),
    )

    # Insert user
    auth_bindings = json.dumps([{"provider": "local", "hash": password_hash}])
    conn.execute(
        """INSERT INTO users (id, primary_email, display_name, status, created_at_micros, auth_bindings_json)
           VALUES (?, ?, ?, 'active', ?, ?)""",
        (uid, email, "Admin", zero_micros, auth_bindings),
    )

    # Insert lab membership (SystemAdmin)
    conn.execute(
        """INSERT INTO lab_memberships (user_id, lab_id, role_id, scope_filters_json, joined_at_micros)
           VALUES (?, ?, ?, ?, ?)""",
        (uid, lab_id, sysadmin_role_id, json.dumps({}), zero_micros),
    )

    conn.commit()
    conn.close()

    print(f"\n✅ Seeded admin user into {db_path}")
    print(f"   Email:    {email}")
    print(f"   Password: {password}")
    print(f"   User ID:  {uid}")
    print(f"   Lab ID:   {lab_id}")
    print(f"\nStart the server with:")
    print(f"  FMGR_DB_PATH={db_path} ./out/build/dev/src/server/freezerd")


if __name__ == "__main__":
    main()
