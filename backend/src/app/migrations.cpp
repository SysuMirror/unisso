#include "core.h"

#include <iostream>

namespace {

void runSql(const std::string& sql, const char* label) {
    if (Db::exec(sql) < 0) {
        std::cerr << "[migrate] " << label << " failed: " << Db::lastError() << std::endl;
    }
}

bool columnExists(const std::string& table, const std::string& column) {
    auto rows = Db::query(
        "SELECT 1 FROM information_schema.columns WHERE table_schema=DATABASE()"
        " AND table_name=" + Db::quote(table) + " AND column_name=" + Db::quote(column) +
        " LIMIT 1");
    return !rows.empty();
}

void addColumn(const std::string& table, const std::string& column,
               const std::string& definition) {
    if (!columnExists(table, column)) {
        runSql("ALTER TABLE " + table + " ADD COLUMN " + column + " " + definition,
               ("add column " + table + "." + column).c_str());
    }
}

}  // namespace

namespace migrations {

void run() {
    addColumn("users", "is_internal", "TINYINT(1) NOT NULL DEFAULT 0");
    addColumn("users", "sse_user_id", "VARCHAR(64) NULL");
    addColumn("users", "is_super_admin", "TINYINT(1) NOT NULL DEFAULT 0");
    addColumn("users", "step_up_enabled", "TINYINT(1) NOT NULL DEFAULT 0");
    addColumn("applications", "organization_id", "VARCHAR(36) NULL");
    // JSON array: IPs a trusted browser has recently authenticated from.
    // Device trust is bound to these, not just to the device cookie.
    addColumn("trusted_devices", "known_ips", "TEXT NULL");
    // 2026-11: second factor is opt-in now. The column default is flipped to 0
    // so INSERTs that omit step_up_enabled start off (admin accounts are
    // seeded with it on in domain::createUser instead).
    runSql("ALTER TABLE users ALTER COLUMN step_up_enabled SET DEFAULT 0",
           "step_up_enabled default off");
    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS migration_flags (
  name VARCHAR(64) NOT NULL PRIMARY KEY,
  applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create migration_flags");
    if (Db::exec("INSERT IGNORE INTO migration_flags (name) VALUES ('step_up_default_off_2026_11')") == 1) {
        // One-time opt-out for non-admin accounts that never chose anything
        // (the switch used to default to on). Re-running this migration later
        // must not undo deliberate opt-ins, hence the flag guard.
        runSql("UPDATE users SET step_up_enabled=0 WHERE step_up_enabled=1 AND is_admin=0",
               "opt out non-admin users from step-up (one time)");
    }

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS organizations (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  name VARCHAR(128) NOT NULL,
  slug VARCHAR(64) NULL UNIQUE,
  description VARCHAR(255) NULL,
  created_by VARCHAR(36) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create organizations");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS organization_members (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  organization_id VARCHAR(36) NOT NULL,
  user_id VARCHAR(36) NOT NULL,
  role VARCHAR(32) NOT NULL DEFAULT 'member',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_org_user (organization_id, user_id),
  KEY idx_org_member_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create organization_members");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS user_groups (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  organization_id VARCHAR(36) NOT NULL,
  name VARCHAR(128) NOT NULL,
  description VARCHAR(255) NULL,
  created_by VARCHAR(36) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_org_group_name (organization_id, name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create user_groups");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS user_group_members (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  group_id VARCHAR(36) NOT NULL,
  user_id VARCHAR(36) NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_group_user (group_id, user_id),
  KEY idx_group_member_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create user_group_members");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS application_organizations (
  application_id VARCHAR(36) NOT NULL,
  organization_id VARCHAR(36) NOT NULL,
  PRIMARY KEY (application_id, organization_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create application_organizations");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS role_assignments (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  subject_type VARCHAR(32) NOT NULL,
  subject_id VARCHAR(64) NOT NULL,
  role VARCHAR(32) NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_subject_role (subject_type, subject_id, role)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create role_assignments");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS sse_identities (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  sse_user_id VARCHAR(64) NOT NULL UNIQUE,
  email VARCHAR(128) NULL,
  name VARCHAR(128) NULL,
  identity VARCHAR(32) NULL,
  is_internal TINYINT(1) NOT NULL DEFAULT 1,
  last_verified_at DATETIME NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create sse_identities");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS account_links (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  canonical_user_id VARCHAR(36) NOT NULL,
  linked_user_id VARCHAR(36) NOT NULL UNIQUE,
  linked_provider VARCHAR(64) NULL,
  linked_subject VARCHAR(128) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create account_links");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS account_peer_links (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  user_a_id VARCHAR(36) NOT NULL,
  user_b_id VARCHAR(36) NOT NULL,
  created_by VARCHAR(36) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_peer_pair (user_a_id,user_b_id),
  KEY idx_peer_b (user_b_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create account_peer_links");

    // Symmetric peer links. Existing master/sub rows become one undirected edge.
    runSql("INSERT IGNORE INTO account_peer_links (id,user_a_id,user_b_id,created_by,created_at) "
           "SELECT UUID(), LEAST(canonical_user_id,linked_user_id), "
           "GREATEST(canonical_user_id,linked_user_id), canonical_user_id, created_at "
           "FROM account_links", "migrate account_links to peer links");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS trusted_devices (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  user_id VARCHAR(36) NOT NULL,
  token_hash VARCHAR(64) NOT NULL,
  user_agent VARCHAR(255) NULL,
  ip VARCHAR(64) NULL,
  known_ips TEXT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_seen_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_device_token (token_hash),
  KEY idx_device_user (user_id, last_seen_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create trusted_devices");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS step_up_device_requests (
  id VARCHAR(64) NOT NULL PRIMARY KEY,
  user_id VARCHAR(36) NOT NULL,
  challenge_hash VARCHAR(64) NOT NULL,
  status VARCHAR(16) NOT NULL DEFAULT 'pending',
  requester_ua VARCHAR(255) NULL,
  requester_ip VARCHAR(64) NULL,
  approver_user_id VARCHAR(36) NULL,
  approver_ua VARCHAR(255) NULL,
  approver_ip VARCHAR(64) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  expires_at DATETIME NOT NULL,
  decided_at DATETIME NULL,
  KEY idx_device_req_user (user_id, status, expires_at),
  KEY idx_device_req_challenge (challenge_hash, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create step_up_device_requests");

    runSql("DELETE FROM step_up_device_requests WHERE expires_at < NOW() - INTERVAL 1 DAY",
           "prune old step-up device requests");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS face_persons (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  provider_person_id VARCHAR(128) NOT NULL,
  external_id VARCHAR(128) NOT NULL,
  created_by VARCHAR(36) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_face_provider (provider_person_id),
  UNIQUE KEY uk_face_external (external_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create face_persons");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS user_faces (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  user_id VARCHAR(36) NOT NULL,
  face_person_id VARCHAR(36) NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_user_face (user_id, face_person_id),
  KEY idx_face_user (face_person_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create user_faces");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS face_audits (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  user_id VARCHAR(36) NULL,
  event VARCHAR(32) NOT NULL,
  provider_person_id VARCHAR(128) NULL,
  score DECIMAL(6,5) NULL,
  ip VARCHAR(64) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_face_audit_user (user_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create face_audits");

    runSql("DELETE FROM trusted_devices WHERE last_seen_at < NOW() - INTERVAL 180 DAY",
           "prune expired trusted devices");
    runSql("DELETE FROM face_audits WHERE created_at < NOW() - INTERVAL 180 DAY",
           "prune old face audits");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS invites (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  code VARCHAR(64) NOT NULL UNIQUE,
  organization_id VARCHAR(36) NULL,
  role VARCHAR(32) NULL,
  created_by VARCHAR(36) NULL,
  used_by VARCHAR(36) NULL,
  used_at DATETIME NULL,
  expires_at DATETIME NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create invites");

    // Users apply to join organizations; super admins and org admins approve.
    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS org_join_requests (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  organization_id VARCHAR(36) NOT NULL,
  user_id VARCHAR(36) NOT NULL,
  message VARCHAR(512) NULL,
  status VARCHAR(16) NOT NULL DEFAULT 'pending',
  reviewed_by VARCHAR(36) NULL,
  review_note VARCHAR(255) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NULL,
  KEY idx_org_join_requests_org (organization_id, status),
  KEY idx_org_join_requests_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create org_join_requests");

    const std::string org = constants::kDefaultOrganizationId;
    runSql("INSERT IGNORE INTO organizations (id,name,slug,description,created_at,updated_at) VALUES (" +
               Db::quote(org) + ",'SSE','sse','默认组织',NOW(),NOW())",
           "seed default organization");
    runSql("INSERT IGNORE INTO role_assignments (id,subject_type,subject_id,role,created_at) VALUES (" +
               Db::quote(util::uuid4()) + ",'sse_user_id','561','super_admin',NOW())",
           "seed super admin 561");

    runSql("INSERT IGNORE INTO organization_members (id,organization_id,user_id,role,created_at) "
           "SELECT UUID()," + Db::quote(org) + ",id,IF(is_admin=1,'admin','member'),NOW() FROM users",
           "migrate users to default organization");
    runSql("UPDATE applications SET organization_id=" + Db::quote(org) +
               " WHERE organization_id IS NULL",
           "migrate applications organization");
    runSql("INSERT IGNORE INTO application_organizations (application_id,organization_id) "
           "SELECT id," + Db::quote(org) + " FROM applications",
           "migrate application_organizations");

    runSql("UPDATE users SET is_super_admin=1, is_admin=1 "
           "WHERE sse_user_id COLLATE utf8mb4_unicode_ci = "
           "_utf8mb4'561' COLLATE utf8mb4_unicode_ci",
           "seed super admin 561 flag");
    runSql("UPDATE users SET is_super_admin=1, is_admin=1 "
           "WHERE id COLLATE utf8mb4_unicode_ci IN ("
           "SELECT subject_id COLLATE utf8mb4_unicode_ci FROM role_assignments "
           "WHERE subject_type COLLATE utf8mb4_unicode_ci = "
           "_utf8mb4'user' COLLATE utf8mb4_unicode_ci "
           "AND role COLLATE utf8mb4_unicode_ci = "
           "_utf8mb4'super_admin' COLLATE utf8mb4_unicode_ci)",
           "sync super admin flags");
    runSql("UPDATE users SET is_admin=1 WHERE is_super_admin=1",
           "ensure super admins are admins");
}

}  // namespace migrations
