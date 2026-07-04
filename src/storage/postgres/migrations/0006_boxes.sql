
CREATE TABLE IF NOT EXISTS boxes (
  id                   TEXT   PRIMARY KEY,
  lab_id               TEXT   NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  box_type_id          TEXT   NOT NULL REFERENCES box_types(id) DEFERRABLE INITIALLY DEFERRED,
  storage_container_id TEXT   NOT NULL
    REFERENCES storage_containers(id) DEFERRABLE INITIALLY DEFERRED,
  label                TEXT   NOT NULL CHECK (length(label) > 0),
  serial               TEXT,
  barcode              TEXT,
  created_at_micros    BIGINT NOT NULL,
  archived_at_micros   BIGINT
);
CREATE INDEX IF NOT EXISTS boxes_lab_idx              ON boxes(lab_id);
CREATE INDEX IF NOT EXISTS boxes_storage_container_idx ON boxes(storage_container_id);
CREATE INDEX IF NOT EXISTS boxes_box_type_idx         ON boxes(box_type_id);
CREATE UNIQUE INDEX IF NOT EXISTS boxes_lab_label_unique
  ON boxes(lab_id, label) WHERE archived_at_micros IS NULL;
ALTER TABLE boxes ENABLE ROW LEVEL SECURITY;
ALTER TABLE boxes FORCE ROW LEVEL SECURITY;
CREATE POLICY fmgr_boxes_lab ON boxes USING (
  lab_id = ANY(string_to_array(current_setting('app.current_lab_ids', true), ',')));
