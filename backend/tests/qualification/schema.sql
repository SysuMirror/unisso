-- Minimal fixture for the qualification chain. Never run against a shared DB.
CREATE DATABASE qualification CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE qualification;
CREATE TABLE users (
  id VARCHAR(36) PRIMARY KEY,
  email VARCHAR(255) UNIQUE,
  password_hash VARCHAR(255),
  username VARCHAR(128),
  full_name VARCHAR(128),
  student_id VARCHAR(64),
  avatar VARCHAR(512),
  email_verified TINYINT NOT NULL DEFAULT 0,
  is_active TINYINT NOT NULL DEFAULT 1,
  is_admin TINYINT NOT NULL DEFAULT 0,
  is_internal TINYINT NOT NULL DEFAULT 0,
  is_super_admin TINYINT NOT NULL DEFAULT 0,
  is_developer TINYINT NOT NULL DEFAULT 0,
  sse_user_id VARCHAR(64),
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
  last_login_at DATETIME
);
CREATE TABLE applications (
  id VARCHAR(36) PRIMARY KEY,
  client_id VARCHAR(128) UNIQUE,
  client_secret VARCHAR(255),
  name VARCHAR(128),
  is_active TINYINT DEFAULT 1,
  is_confidential TINYINT DEFAULT 1,
  m2m_enabled TINYINT DEFAULT 1,
  kind VARCHAR(16) DEFAULT 'internal',
  status VARCHAR(16) DEFAULT 'approved',
  redirect_uris TEXT,
  allowed_scopes TEXT
);
CREATE TABLE user_consents (
  id VARCHAR(36) PRIMARY KEY,
  user_id VARCHAR(36),
  application_id VARCHAR(36),
  scopes TEXT,
  is_active TINYINT DEFAULT 1,
  granted_at DATETIME,
  revoked_at DATETIME
);
CREATE TABLE user_identities (
  id VARCHAR(36) PRIMARY KEY,
  user_id VARCHAR(36),
  is_active TINYINT DEFAULT 1,
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE access_tokens (
  id VARCHAR(36) PRIMARY KEY,
  token VARCHAR(128) UNIQUE,
  revoked TINYINT DEFAULT 0
);
-- Skip unrelated one-time seeds and cleanup in this small fixture.
CREATE TABLE migration_flags (
  name VARCHAR(64) PRIMARY KEY,
  applied_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
INSERT INTO migration_flags (name) VALUES
  ('applications_m2m_enabled_2026_11'),
  ('homepage_apps_seed_2026_11'),
  ('redesign_cleanup_2026_11');
