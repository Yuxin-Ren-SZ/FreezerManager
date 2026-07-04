
CREATE TABLE IF NOT EXISTS audit_events (
  id               TEXT   PRIMARY KEY,
  at_micros        BIGINT NOT NULL,
  actor_user_id    TEXT   NOT NULL,
  actor_session_id TEXT   NOT NULL,
  lab_id           TEXT,
  action           TEXT   NOT NULL,
  entity_kind      TEXT   NOT NULL,
  entity_id        TEXT,
  before_json      TEXT   NOT NULL DEFAULT '{}',
  after_json       TEXT   NOT NULL DEFAULT '{}',
  request_id       TEXT   NOT NULL,
  prev_hash        TEXT   NOT NULL,
  this_hash        TEXT   NOT NULL UNIQUE
);
CREATE INDEX IF NOT EXISTS audit_events_at_idx          ON audit_events(at_micros);
CREATE INDEX IF NOT EXISTS audit_events_actor_idx       ON audit_events(actor_user_id);
CREATE INDEX IF NOT EXISTS audit_events_entity_kind_idx ON audit_events(entity_kind);
CREATE INDEX IF NOT EXISTS audit_events_lab_idx         ON audit_events(lab_id);

CREATE OR REPLACE FUNCTION fmgr_audit_immutable()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RAISE EXCEPTION 'audit_events is append-only: % are not permitted', TG_OP;
END; $$;

DO $$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_trigger WHERE tgname = 'audit_events_no_update') THEN
    CREATE TRIGGER audit_events_no_update
      BEFORE UPDATE ON audit_events
      FOR EACH ROW EXECUTE FUNCTION fmgr_audit_immutable();
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_trigger WHERE tgname = 'audit_events_no_delete') THEN
    CREATE TRIGGER audit_events_no_delete
      BEFORE DELETE ON audit_events
      FOR EACH ROW EXECUTE FUNCTION fmgr_audit_immutable();
  END IF;
END $$;
