
CREATE TABLE IF NOT EXISTS sessions (
  id                  TEXT   PRIMARY KEY,
  user_id             TEXT   NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,
  token_hash          TEXT   NOT NULL CHECK (length(token_hash) > 0),
  token_prefix        TEXT   NOT NULL CHECK (length(token_prefix) > 0),
  created_at_micros   BIGINT NOT NULL,
  last_seen_at_micros BIGINT NOT NULL,
  ip                  TEXT,
  user_agent          TEXT,
  revoked_at_micros   BIGINT
);
CREATE UNIQUE INDEX IF NOT EXISTS sessions_prefix_active_unique
  ON sessions(token_prefix) WHERE revoked_at_micros IS NULL;
CREATE INDEX IF NOT EXISTS sessions_user_id_idx ON sessions(user_id);
CREATE TABLE IF NOT EXISTS api_tokens (
  id                TEXT   PRIMARY KEY,
  user_id           TEXT   NOT NULL REFERENCES users(id)  DEFERRABLE INITIALLY DEFERRED,
  lab_id            TEXT   REFERENCES labs(id)            DEFERRABLE INITIALLY DEFERRED,
  name              TEXT   NOT NULL CHECK (length(name) > 0),
  scope_json        TEXT   NOT NULL DEFAULT '[]',
  token_hash        TEXT   NOT NULL CHECK (length(token_hash) > 0),
  token_prefix      TEXT   NOT NULL CHECK (length(token_prefix) > 0),
  created_at_micros BIGINT NOT NULL,
  expires_at_micros BIGINT,
  revoked_at_micros BIGINT
);
CREATE UNIQUE INDEX IF NOT EXISTS api_tokens_prefix_active_unique
  ON api_tokens(token_prefix) WHERE revoked_at_micros IS NULL;
CREATE INDEX IF NOT EXISTS api_tokens_user_id_idx ON api_tokens(user_id);
CREATE INDEX IF NOT EXISTS api_tokens_lab_id_idx  ON api_tokens(lab_id);
