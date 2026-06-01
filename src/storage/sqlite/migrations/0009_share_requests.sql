-- SPDX-License-Identifier: AGPL-3.0-or-later
-- D8: Cross-lab share-request workflow.
--
-- State machine: pending -> approved | rejected | revoked.
-- Approval requires all three roles: source_admin, target_admin, system_admin.
-- share_request_approvals is append-only; no UPDATE or DELETE paths exist.
-- source_lab_id != target_lab_id enforced by CHECK.
-- All foreign keys are DEFERRABLE so inserts within a transaction can arrive in any order.
-- No ON DELETE CASCADE anywhere — tombstone propagation is application-level.

CREATE TABLE IF NOT EXISTS share_requests (
  id                TEXT    PRIMARY KEY,
  source_lab_id     TEXT    NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  target_lab_id     TEXT    NOT NULL REFERENCES labs(id)  DEFERRABLE INITIALLY DEFERRED,
  requested_by      TEXT    NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  scope_json        TEXT    NOT NULL DEFAULT '{}',
  status            TEXT    NOT NULL DEFAULT 'pending'
    CHECK (status IN ('pending', 'approved', 'rejected', 'revoked')),
  created_at_micros  INTEGER NOT NULL,
  decided_at_micros  INTEGER,
  CHECK (source_lab_id != target_lab_id)
);

CREATE INDEX IF NOT EXISTS share_requests_source_lab_idx ON share_requests(source_lab_id);
CREATE INDEX IF NOT EXISTS share_requests_target_lab_idx ON share_requests(target_lab_id);
CREATE INDEX IF NOT EXISTS share_requests_status_idx     ON share_requests(status);

CREATE TABLE IF NOT EXISTS share_request_approvals (
  share_request_id  TEXT    NOT NULL REFERENCES share_requests(id) DEFERRABLE INITIALLY DEFERRED,
  approver_role     TEXT    NOT NULL
    CHECK (approver_role IN ('source_admin', 'target_admin', 'system_admin')),
  approver_user_id  TEXT    NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  decided_at_micros INTEGER NOT NULL,
  note              TEXT,
  PRIMARY KEY (share_request_id, approver_role)
);

CREATE INDEX IF NOT EXISTS share_request_approvals_request_idx
  ON share_request_approvals(share_request_id);
