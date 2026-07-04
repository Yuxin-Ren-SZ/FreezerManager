
-- E2: Add MFA completion tracking to sessions.
-- Existing rows default to 1 (mfa_complete = true) so that previously
-- created sessions remain valid after the migration.
ALTER TABLE sessions ADD COLUMN mfa_complete INTEGER NOT NULL DEFAULT 1;
