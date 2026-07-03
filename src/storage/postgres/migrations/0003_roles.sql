
CREATE TABLE IF NOT EXISTS permissions (
  key         TEXT PRIMARY KEY,
  description TEXT NOT NULL CHECK (length(description) > 0)
);
CREATE TABLE IF NOT EXISTS roles (
  id                 TEXT    PRIMARY KEY,
  lab_id             TEXT    REFERENCES labs(id) DEFERRABLE INITIALLY DEFERRED,
  kind               TEXT    NOT NULL CHECK (kind IN
    ('system_admin','lab_admin','member','read_only','api_client','custom')),
  name               TEXT    NOT NULL CHECK (length(name) > 0),
  description        TEXT    NOT NULL,
  is_builtin         BOOLEAN NOT NULL DEFAULT FALSE,
  created_at_micros  BIGINT  NOT NULL,
  archived_at_micros BIGINT
);
CREATE UNIQUE INDEX IF NOT EXISTS roles_builtin_name_unique
  ON roles(name) WHERE lab_id IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS roles_lab_name_unique
  ON roles(lab_id, name) WHERE lab_id IS NOT NULL;
CREATE TABLE IF NOT EXISTS role_permissions (
  role_id        TEXT NOT NULL REFERENCES roles(id)       DEFERRABLE INITIALLY DEFERRED,
  permission_key TEXT NOT NULL REFERENCES permissions(key) DEFERRABLE INITIALLY DEFERRED,
  PRIMARY KEY (role_id, permission_key)
);
DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint WHERE conname = 'lab_memberships_role_id_fk'
  ) THEN
    ALTER TABLE lab_memberships
      ADD CONSTRAINT lab_memberships_role_id_fk
      FOREIGN KEY (role_id) REFERENCES roles(id) DEFERRABLE INITIALLY DEFERRED;
  END IF;
END $$;
INSERT INTO permissions (key, description) VALUES
  ('sample.read','Read sample records.'),
  ('sample.write','Create or modify samples.'),
  ('sample.checkout','Check samples in or out.'),
  ('sample.delete_soft','Tombstone a sample.'),
  ('sample.delete_hard','Permanently delete tombstoned sample (SystemAdmin only).'),
  ('box.configure','Define or modify boxes and box types.'),
  ('freezer.configure','Configure freezers and storage layout.'),
  ('custom_field.define','Define custom field schemas.'),
  ('item_type.define','Define item-type taxonomy.'),
  ('user.invite','Invite or revoke lab members.'),
  ('user.manage_roles','Assign roles to lab members.'),
  ('audit.read','Browse audit log entries.'),
  ('audit.export','Export signed audit log files.'),
  ('backup.run','Trigger a backup or restore-drill.'),
  ('share.request','Open a cross-lab share request.'),
  ('share.approve','Approve or reject a share request.'),
  ('phi.read','Read PHI-tagged custom fields.'),
  ('lab.configure','Modify lab settings.'),
  ('lab.enable_phi','Toggle PHI mode on a lab.'),
  ('key.rotate','Rotate cryptographic keys.'),
  ('session.revoke','Revoke another user''s sessions.')
  ON CONFLICT (key) DO NOTHING;
INSERT INTO roles (id,lab_id,kind,name,description,is_builtin,created_at_micros) VALUES
  ('00000000-0000-0000-0000-000000000001',NULL,'system_admin','SystemAdmin',
   'Deployment-wide administrator (PHI access requires separate grant).',TRUE,0),
  ('00000000-0000-0000-0000-000000000002',NULL,'lab_admin','LabAdmin',
   'Full administrative control of one lab.',TRUE,0),
  ('00000000-0000-0000-0000-000000000003',NULL,'member','Member',
   'Read+write samples within scoped freezers/projects.',TRUE,0),
  ('00000000-0000-0000-0000-000000000004',NULL,'read_only','ReadOnly',
   'Read-only access; cannot check out.',TRUE,0),
  ('00000000-0000-0000-0000-000000000005',NULL,'api_client','ApiClient',
   'Conservative machine-client default; expand via custom roles.',TRUE,0)
  ON CONFLICT (id) DO NOTHING;
INSERT INTO role_permissions (role_id,permission_key) VALUES
  ('00000000-0000-0000-0000-000000000001','sample.read'),
  ('00000000-0000-0000-0000-000000000001','sample.write'),
  ('00000000-0000-0000-0000-000000000001','sample.checkout'),
  ('00000000-0000-0000-0000-000000000001','sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000001','sample.delete_hard'),
  ('00000000-0000-0000-0000-000000000001','box.configure'),
  ('00000000-0000-0000-0000-000000000001','freezer.configure'),
  ('00000000-0000-0000-0000-000000000001','custom_field.define'),
  ('00000000-0000-0000-0000-000000000001','item_type.define'),
  ('00000000-0000-0000-0000-000000000001','user.invite'),
  ('00000000-0000-0000-0000-000000000001','user.manage_roles'),
  ('00000000-0000-0000-0000-000000000001','audit.read'),
  ('00000000-0000-0000-0000-000000000001','audit.export'),
  ('00000000-0000-0000-0000-000000000001','backup.run'),
  ('00000000-0000-0000-0000-000000000001','share.request'),
  ('00000000-0000-0000-0000-000000000001','share.approve'),
  ('00000000-0000-0000-0000-000000000001','lab.configure'),
  ('00000000-0000-0000-0000-000000000001','lab.enable_phi'),
  ('00000000-0000-0000-0000-000000000001','key.rotate'),
  ('00000000-0000-0000-0000-000000000001','session.revoke'),
  ('00000000-0000-0000-0000-000000000002','sample.read'),
  ('00000000-0000-0000-0000-000000000002','sample.write'),
  ('00000000-0000-0000-0000-000000000002','sample.checkout'),
  ('00000000-0000-0000-0000-000000000002','sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000002','box.configure'),
  ('00000000-0000-0000-0000-000000000002','freezer.configure'),
  ('00000000-0000-0000-0000-000000000002','custom_field.define'),
  ('00000000-0000-0000-0000-000000000002','item_type.define'),
  ('00000000-0000-0000-0000-000000000002','user.invite'),
  ('00000000-0000-0000-0000-000000000002','user.manage_roles'),
  ('00000000-0000-0000-0000-000000000002','audit.read'),
  ('00000000-0000-0000-0000-000000000002','audit.export'),
  ('00000000-0000-0000-0000-000000000002','share.request'),
  ('00000000-0000-0000-0000-000000000002','share.approve'),
  ('00000000-0000-0000-0000-000000000002','lab.configure'),
  ('00000000-0000-0000-0000-000000000003','sample.read'),
  ('00000000-0000-0000-0000-000000000003','sample.write'),
  ('00000000-0000-0000-0000-000000000003','sample.checkout'),
  ('00000000-0000-0000-0000-000000000003','sample.delete_soft'),
  ('00000000-0000-0000-0000-000000000003','share.request'),
  ('00000000-0000-0000-0000-000000000004','sample.read'),
  ('00000000-0000-0000-0000-000000000004','audit.read'),
  ('00000000-0000-0000-0000-000000000005','sample.read'),
  ('00000000-0000-0000-0000-000000000005','sample.checkout')
  ON CONFLICT DO NOTHING;
