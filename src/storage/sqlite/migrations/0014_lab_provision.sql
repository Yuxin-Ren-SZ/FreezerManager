
-- Global-only permission for deployment-level lab provisioning (LabService/CreateLab).
-- Creating a brand-new lab cannot be gated by a per-lab permission because no
-- membership exists yet; it is a SystemAdmin deployment action. Granted to the
-- built-in SystemAdmin role only.
INSERT INTO permissions (key, description) VALUES
  ('lab.provision', 'Provision (create) new labs (SystemAdmin only).');
INSERT INTO role_permissions (role_id, permission_key) VALUES
  ('00000000-0000-0000-0000-000000000001', 'lab.provision');
