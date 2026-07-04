
-- Authorization epoch for permission-cache invalidation. Bumped in-transaction
-- whenever a user's effective permissions change (membership/role/grant). Auth
-- providers key their resolved-permission cache on this value so a downgrade
-- takes effect on the next request rather than waiting for the cache TTL.
ALTER TABLE users ADD COLUMN IF NOT EXISTS authz_version BIGINT NOT NULL DEFAULT 0;
