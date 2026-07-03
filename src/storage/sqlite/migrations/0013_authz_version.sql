
-- Authorization epoch for cache invalidation. Bumped in-transaction whenever a
-- user's effective permissions change (membership/role/grant). Auth providers
-- key their resolved-permission cache on this value so a downgrade takes effect
-- on the next request rather than waiting for the cache TTL. Existing rows start
-- at 0; any later authz change increments it.
ALTER TABLE users ADD COLUMN authz_version INTEGER NOT NULL DEFAULT 0;
