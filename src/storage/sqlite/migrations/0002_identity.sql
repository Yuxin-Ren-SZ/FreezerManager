
CREATE TABLE IF NOT EXISTS labs (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL CHECK (length(name) > 0),
  contact TEXT NOT NULL,
  created_at_micros INTEGER NOT NULL,
  settings_json TEXT NOT NULL CHECK (json_valid(settings_json)),
  is_phi_enabled INTEGER NOT NULL CHECK (is_phi_enabled IN (0, 1)),
  archived_at_micros INTEGER
);

CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY,
  primary_email TEXT NOT NULL CHECK (length(primary_email) > 2),
  display_name TEXT NOT NULL CHECK (length(display_name) > 0),
  status TEXT NOT NULL CHECK (status IN ('active', 'disabled')),
  created_at_micros INTEGER NOT NULL,
  auth_bindings_json TEXT NOT NULL CHECK (json_valid(auth_bindings_json)),
  totp_secret_enc TEXT,
  default_lab_id TEXT REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED
);

CREATE UNIQUE INDEX IF NOT EXISTS users_primary_email_lower_unique
  ON users(lower(primary_email));

CREATE TABLE IF NOT EXISTS lab_memberships (
  user_id TEXT NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  lab_id TEXT NOT NULL REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  scope_filters_json TEXT NOT NULL CHECK (json_valid(scope_filters_json)),
  invited_by TEXT REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  joined_at_micros INTEGER NOT NULL,
  revoked_at_micros INTEGER,
  PRIMARY KEY (user_id, lab_id)
);
