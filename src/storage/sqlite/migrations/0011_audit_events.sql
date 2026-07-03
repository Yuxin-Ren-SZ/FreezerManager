
-- Replace the simple audit_events table introduced in 0001_init with the
-- full hash-chained schema required by PRD §7.3.
-- DROP is safe here because the 0001 table was a temporary bootstrap placeholder
-- with no production data.
DROP TABLE IF EXISTS audit_events;

CREATE TABLE audit_events (
  id               TEXT    PRIMARY KEY,
  at_micros        INTEGER NOT NULL,
  actor_user_id    TEXT    NOT NULL,
  actor_session_id TEXT    NOT NULL,
  lab_id           TEXT,
  action           TEXT    NOT NULL,
  entity_kind      TEXT    NOT NULL,
  entity_id        TEXT,
  before_json      TEXT    NOT NULL DEFAULT '{}',
  after_json       TEXT    NOT NULL DEFAULT '{}',
  request_id       TEXT    NOT NULL,
  prev_hash        TEXT    NOT NULL,
  this_hash        TEXT    NOT NULL UNIQUE
);

CREATE INDEX IF NOT EXISTS audit_events_at_idx          ON audit_events(at_micros);
CREATE INDEX IF NOT EXISTS audit_events_actor_idx       ON audit_events(actor_user_id);
CREATE INDEX IF NOT EXISTS audit_events_entity_kind_idx ON audit_events(entity_kind);
CREATE INDEX IF NOT EXISTS audit_events_lab_idx         ON audit_events(lab_id);

-- Immutability triggers: the hash chain must never be altered.
CREATE TRIGGER IF NOT EXISTS audit_events_no_update
  BEFORE UPDATE ON audit_events
  FOR EACH ROW
  BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: updates are not permitted');
  END;

CREATE TRIGGER IF NOT EXISTS audit_events_no_delete
  BEFORE DELETE ON audit_events
  FOR EACH ROW
  BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: deletes are not permitted');
  END;
