
-- C-1: Persistent login-lockout state. Failed-login counters must survive a
-- process restart, otherwise an attacker bypasses the account lockout by
-- bouncing freezerd. Keyed by lowercased email; not lab-scoped (the lockout
-- check runs pre-authentication, before any lab context exists), so no RLS.
-- One active row per email: a successful login clears the row via
-- cleared_at_micros; the partial unique index below allows a fresh active row
-- on the next failure while cleared rows are retained (mirrors sessions).
CREATE TABLE IF NOT EXISTS login_attempts (
  id                    TEXT    PRIMARY KEY,
  email                 TEXT    NOT NULL CHECK (length(email) > 0),
  failure_count         INTEGER NOT NULL DEFAULT 0,
  locked_until_micros   INTEGER,
  last_activity_micros  INTEGER NOT NULL,
  cleared_at_micros     INTEGER
);

CREATE UNIQUE INDEX IF NOT EXISTS login_attempts_email_active_unique
  ON login_attempts(email) WHERE cleared_at_micros IS NULL;
