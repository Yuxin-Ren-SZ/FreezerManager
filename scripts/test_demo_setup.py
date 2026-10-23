#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Isolated startup regressions for demo-setup.sh (stdlib only)."""

import os
import shlex
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "scripts" / "demo-setup.sh"


class DemoSetupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fmgr-demo-test-")
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.root / "scripts").mkdir()
        shutil.copy2(SCRIPT, self.root / "scripts" / "demo-setup.sh")
        self.server = self.root / "out/build/dev/src/server/freezerd"
        self.cli = self.root / "out/build/dev/src/cli/freezerctl"
        self.db = self.root / "out/demo/freezer.db"
        self.marker = Path(f"{self.db}.demo-seeded")
        self.server.parent.mkdir(parents=True)
        self.cli.parent.mkdir(parents=True)
        web = self.root / "src/web"
        (web / "node_modules/.bin").mkdir(parents=True)
        vite = web / "node_modules/.bin/vite"
        vite.write_text("#!/bin/sh\nexec sleep 30\n")
        vite.chmod(0o755)
        self.cli.write_text("#!/bin/sh\nexit 0\n")
        self.cli.chmod(0o755)
        venv_python = self.root / ".venv/bin/python3"
        venv_python.parent.mkdir(parents=True)
        venv_python.write_text(
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = -c ]; then\n"
            "  case \"${2:-}\" in *'import argon2'*) exit 0;; esac\n"
            "fi\n"
            "if [ \"${1:-}\" = - ]; then\n"
            "  input=$(cat)\n"
            "  case \"$input\" in\n"
            "    *LEGACY_DEMO_SEED_CHECK*) printf '%s\\n' \"$input\" | "
            + shlex.quote(sys.executable)
            + " - \"$2\"; exit $?;;\n"
            "    *'UPDATE users SET totp_secret_enc'*) exit 0;;\n"
            "  esac\n"
            "fi\n"
            "case \"${1:-}\" in */seed_demo.py) [ \"${DEMO_SEED_FAIL:-0}\" != 1 ]; exit;; esac\n"
            "exec "
            + shlex.quote(sys.executable)
            + " \"$@\"\n"
        )
        venv_python.chmod(0o755)
        self.server.write_text(
            "#!/bin/sh\n"
            "echo server-start >> \"$DEMO_STUB_EVENTS\"\n"
            "echo $$ > \"$DEMO_SERVER_PIDFILE\"\n"
            "touch \"$FMGR_DB_PATH\"\n"
            "exec sleep 30\n"
        )
        self.server.chmod(0o755)
        self._stub(
            "curl",
            "#!/bin/sh\n"
            "echo curl >> \"$DEMO_STUB_EVENTS\"\n"
            "if [ \"${DEMO_KILL_SERVER_ON_CURL:-0}\" = 1 ] && [ -f \"$DEMO_SERVER_PIDFILE\" ]; then\n"
            "  kill \"$(cat \"$DEMO_SERVER_PIDFILE\")\"\n"
            "fi\n"
            "case \"$*\" in */api/v1/health*) echo '{\"status\":\"ok\"}';; esac\n"
            "exit 0\n",
        )
        self._stub("sleep", "#!/bin/sh\nif [ \"${1:-}\" = 30 ]; then exec /bin/sleep 30; fi\nexit 0\n")
        self._stub("seq", "#!/bin/sh\necho 1\n")
        self._stub("node", "#!/bin/sh\necho v22.0.0\n")
        self._stub("npm", "#!/bin/sh\nexit 0\n")
        self._stub("cat", "#!/bin/sh\nexec /bin/cat \"$@\"\n")
        self._stub("mkdir", "#!/bin/sh\nexec /bin/mkdir \"$@\"\n")
        self._stub("rm", "#!/bin/sh\nexec /bin/rm \"$@\"\n")
        self._stub("touch", "#!/bin/sh\nexec /usr/bin/touch \"$@\"\n")
        self._stub("dirname", "#!/bin/sh\nexec /usr/bin/dirname \"$@\"\n")
        self._stub("env", "#!/bin/sh\nexec /usr/bin/env \"$@\"\n")
        self._stub("mv", "#!/bin/sh\nexec /bin/mv \"$@\"\n")
        self._stub(
            "lsof",
            "#!/bin/sh\n"
            "if [ \"${DEMO_STUB_PORT_BUSY:-}\" = \"${2#*:}\" ]; then\n"
            "  echo busy-listener\n"
            "  exit 0\n"
            "fi\n"
            "exit 1\n",
        )

    def tearDown(self):
        self.temp.cleanup()

    def _stub(self, name, body):
        path = self.bin / name
        path.write_text(body)
        path.chmod(0o755)

    def run_demo(self, *args, **extra_env):
        events = self.root / "events"
        env = {
            **os.environ,
            "PATH": str(self.bin),
            "FMGR_MAIN_CHECKOUT": str(self.root),
            "FMGR_DEMO_DB_PATH": str(self.db),
            "FMGR_FREEZERCTL": str(self.cli),
            "FMGR_LISTEN": "127.0.0.1:51003",
            "FMGR_REST_LISTEN": "127.0.0.1:18113",
            "FMGR_WEB_DEV_PORT": "5213",
            "DEMO_STUB_EVENTS": str(events),
            "DEMO_SERVER_PIDFILE": str(self.root / "server.pid"),
            "DEMO_STUB_SEEDED": "1",
            **extra_env,
        }
        process = subprocess.Popen(
            ["/bin/bash", str(self.root / "scripts/demo-setup.sh"), *args],
            cwd=self.root,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            stdout, stderr = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=3)
        result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
        return result, events.read_text() if events.exists() else ""

    def test_missing_freezerctl_fails_before_reset_or_migration(self):
        self.cli.unlink()
        db = self.root / "out/demo/freezer.db"
        db.parent.mkdir(parents=True, exist_ok=True)
        db.write_text("keep existing data")

        result, events = self.run_demo("--reset", FMGR_FREEZERCTL="")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("freezerctl", result.stderr.lower())
        self.assertEqual(db.read_text(), "keep existing data")
        self.assertNotIn("server-start", events)

    def test_missing_lsof_fails_before_reset_or_migration(self):
        (self.bin / "lsof").unlink()
        db = self.root / "out/demo/freezer.db"
        db.parent.mkdir(parents=True, exist_ok=True)
        db.write_text("keep existing data")

        result, events = self.run_demo("--reset")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lsof", result.stderr.lower())
        self.assertEqual(db.read_text(), "keep existing data")
        self.assertNotIn("server-start", events)

    def test_incomplete_database_refuses_reuse_with_reset_recovery_message(self):
        self.db.parent.mkdir(parents=True, exist_ok=True)
        self.db.write_text("incomplete")

        result, events = self.run_demo()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incomplete", result.stderr.lower())
        self.assertIn("--reset", result.stderr)
        self.assertEqual(self.db.read_text(), "incomplete")
        self.assertNotIn("server-start", events)

    def create_legacy_database(self, neuro_samples=20, onco_samples=12):
        self.db.parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(self.db) as conn:
            conn.executescript(
                "CREATE TABLE users (id TEXT PRIMARY KEY, primary_email TEXT, totp_secret_enc TEXT);"
                "CREATE TABLE labs (id TEXT PRIMARY KEY);"
                "CREATE TABLE samples (id INTEGER PRIMARY KEY, lab_id TEXT);"
            )
            conn.execute(
                "INSERT INTO users VALUES (?, ?, NULL)",
                ("00000000-0000-b001-0000-000000000001", "admin@freezer.local"),
            )
            labs = (
                "00000000-0000-a001-0000-000000000001",
                "00000000-0000-a001-0000-000000000002",
            )
            conn.executemany("INSERT INTO labs VALUES (?)", [(lab,) for lab in labs])
            conn.executemany(
                "INSERT INTO samples (lab_id) VALUES (?)",
                [(labs[0],)] * neuro_samples + [(labs[1],)] * onco_samples,
            )

    def test_partial_reference_only_seed_is_rejected_without_mutation(self):
        self.create_legacy_database(neuro_samples=0, onco_samples=0)

        result, events = self.run_demo()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("seed state is unknown", result.stderr.lower())
        self.assertIn("--reset", result.stderr)
        self.assertFalse(self.marker.exists())
        self.assertNotIn("server-start", events)

    def test_completed_legacy_seed_bootstraps_marker_without_audit_count(self):
        self.create_legacy_database()

        result, _ = self.run_demo()

        self.assertIn(result.returncode, (0, 130), result.stderr)
        self.assertTrue(self.marker.exists())
        self.assertEqual(self.marker.read_text().strip(), "freezermanager-demo-seeded-v1")

    def test_marker_allows_normal_sample_edits_without_reseeding(self):
        self.create_legacy_database(neuro_samples=0, onco_samples=0)
        self.marker.write_text("freezermanager-demo-seeded-v1\n")

        result, _ = self.run_demo()

        self.assertIn(result.returncode, (0, 130), result.stderr)
        self.assertIn("completion marker found", result.stdout)
        self.assertEqual(self.marker.read_text().strip(), "freezermanager-demo-seeded-v1")

    def test_reset_removes_stale_marker_before_a_failed_seed(self):
        self.create_legacy_database()
        self.marker.write_text("freezermanager-demo-seeded-v1\n")

        result, _ = self.run_demo("--reset", DEMO_SEED_FAIL="1")

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.marker.exists())

    def test_occupied_port_is_reported_without_starting_server(self):
        result, events = self.run_demo(DEMO_STUB_PORT_BUSY="18113")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("port 18113 is already in use", result.stderr)
        self.assertNotIn("server-start", events)

    def test_child_exit_is_not_hidden_by_another_process_health_response(self):
        result, events = self.run_demo(DEMO_KILL_SERVER_ON_CURL="1")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exited before", result.stderr.lower())
        self.assertIn("freezerd-migrate.log", result.stderr)
        self.assertIn("curl", events)

    def test_normal_ready_path_starts_both_owned_services(self):
        result, events = self.run_demo()

        self.assertIn(result.returncode, (0, 130), result.stderr)
        self.assertIn("Demo is ready", result.stdout)
        self.assertIn("server-start", events)
        self.assertIn("curl", events)
        self.assertTrue(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
