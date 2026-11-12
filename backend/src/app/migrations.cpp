#include "core.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

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

std::string configuredSuperAdminIds() {
    if (const char* env = std::getenv("UNISSO_SSE_SUPER_ADMIN_IDS"); env && *env) return env;
    return "561";
}

// Tables left over from the removed organization/role/face/step-up features.
const char* const kDroppedTables[] = {
    "organizations", "organization_members", "application_organizations",
    "org_join_requests", "user_groups", "user_group_members", "invites",
    "roles", "user_roles", "permissions", "role_permissions", "role_assignments",
    "account_links", "account_peer_links",
    "user_faces", "face_persons", "face_audits",
    "step_up_device_requests", "trusted_devices",
};

}  // namespace

namespace migrations {

void run() {
    addColumn("users", "is_internal", "TINYINT(1) NOT NULL DEFAULT 0");
    addColumn("users", "sse_user_id", "VARCHAR(64) NULL");
    addColumn("users", "is_super_admin", "TINYINT(1) NOT NULL DEFAULT 0");
    addColumn("applications", "m2m_enabled", "TINYINT(1) NOT NULL DEFAULT 0");
    // Two-track access: first-party platforms vs third-party OAuth applications.
    addColumn("applications", "kind", "VARCHAR(16) NOT NULL DEFAULT 'internal'");
    addColumn("applications", "status", "VARCHAR(16) NOT NULL DEFAULT 'approved'");
    addColumn("applications", "reviewed_by", "VARCHAR(36) NULL");
    addColumn("applications", "reviewed_at", "DATETIME NULL");
    addColumn("applications", "review_note", "VARCHAR(512) NULL");

    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS migration_flags (
  name VARCHAR(64) NOT NULL PRIMARY KEY,
  applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create migration_flags");

    // The admin console owns the m2m switch now; the environment allow-list
    // clients were imported once when the column was introduced.
    if (Db::exec("INSERT IGNORE INTO migration_flags (name) VALUES ('applications_m2m_enabled_2026_11')") == 1) {
        runSql("UPDATE applications SET m2m_enabled=1 WHERE client_id IN ("
               "'app_4d5b4228022b4a97','app_d12284c26f111821','app_06af4e375c2bf9e8',"
               "'app_0e8d723ab1f092dd','app_cf99748456d9868b','app_94cf23fcb41d331c',"
               "'app_ddf5640acd86457e','app_2262d080be9e0bab','app_2f85bb569c1deb5f',"
               "'app_c992d5d1472fc541','app_85bf3cb652742920','sseinfra',"
               "'client_ufYz8oTtUc3VqerdW8TpGg') AND kind='internal'",
               "seed m2m enabled applications");
    }

    // Bootstrapped super admins come from UNISSO_SSE_SUPER_ADMIN_IDS; "-1"
    // disables the automatic promotion (the admin console owns the flags).
    std::string superIdList;
    {
        std::stringstream stream(configuredSuperAdminIds());
        std::string item;
        while (std::getline(stream, item, ',')) {
            item = util::trim(item);
            if (item.empty() || item == "-1") continue;
            if (!superIdList.empty()) superIdList += ",";
            superIdList += Db::quote(item);
        }
    }
    if (!superIdList.empty()) {
        runSql("UPDATE users SET is_super_admin=1, is_admin=1 WHERE sse_user_id IN (" +
                   superIdList + ")",
               "seed super admin from UNISSO_SSE_SUPER_ADMIN_IDS");
    }

    // Landing page ecosystem list: the apps shown in the public marquee, owned
    // by the super-admin editor at /admin/homepage.
    runSql(R"SQL(
CREATE TABLE IF NOT EXISTS homepage_apps (
  id VARCHAR(36) NOT NULL PRIMARY KEY,
  name VARCHAR(128) NOT NULL,
  description VARCHAR(512) NULL,
  icon VARCHAR(512) NULL,
  url VARCHAR(512) NOT NULL,
  sort_order INT NOT NULL DEFAULT 0,
  is_visible TINYINT(1) NOT NULL DEFAULT 1,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci)SQL", "create homepage_apps");

    // First boot: start from a curated list of what is already live, so the home
    // page is never empty. Everything here is editable afterwards.
    if (Db::exec("INSERT IGNORE INTO migration_flags (name) VALUES ('homepage_apps_seed_2026_11')") == 1) {
        long long existing = 0;
        {
            auto rows = Db::query("SELECT COUNT(*) AS c FROM homepage_apps");
            if (!rows.empty()) {
                try { existing = std::stoll(rows.front().at("c")); } catch (...) {}
            }
        }
        if (existing == 0) {
            runSql(R"SQL(INSERT INTO homepage_apps
  (id,name,description,icon,url,sort_order,is_visible,created_at,updated_at) VALUES
  (UUID(),'集市主站','校园集市：二手交易、招募、活动与社区','',        'https://ssemarket.cn/',10,1,NOW(),NOW()),
  (UUID(),'集市招聘','集市招聘：岗位发布与投递','https://image.ssemarket.cn/i/images/2026/09/10/cef96c21-95e9-44e8-b84f-ad388fc409c4.png','https://join.ssemarket.cn/',20,1,NOW(),NOW()),
  (UUID(),'IWiki','知识库与文档协作','https://image.ssemarket.cn/i/images/2026/10/26/10a6eb4c-8011-41ed-84dd-68a0210804b8.png','https://iwiki.ssemarket.cn/',30,1,NOW(),NOW()),
  (UUID(),'SSE 云平台','统一算力与部署平台：Pod、GPU、存储','',       'https://cloud.ssemarket.cn/',40,1,NOW(),NOW()),
  (UUID(),'Octop','运维控制台，单点登录直达','',                      'https://octop.ssemarket.cn/',50,1,NOW(),NOW()),
  (UUID(),'图灵科竞协会','图灵科竞协会统一平台','',                    'https://turing.ssemarket.cn/',60,1,NOW(),NOW())
)SQL", "seed homepage apps");
        }
    }

    // One-time removal of everything the 2026-11 redesign retired.
    if (Db::exec("INSERT IGNORE INTO migration_flags (name) VALUES ('redesign_cleanup_2026_11')") == 1) {
        runSql("UPDATE applications SET kind='internal', status='approved', "
               "review_note=NULL WHERE kind IS NULL OR kind='' OR status IS NULL OR status=''",
               "default existing applications to approved internal");
        for (const char* table : kDroppedTables) {
            runSql(std::string("DROP TABLE IF EXISTS ") + table, ("drop " + std::string(table)).c_str());
        }
        runSql("ALTER TABLE users DROP COLUMN step_up_enabled", "drop users.step_up_enabled");
        runSql("ALTER TABLE applications DROP COLUMN organization_id",
               "drop applications.organization_id");
        runSql("UPDATE users SET is_admin=0, is_super_admin=0 "
               "WHERE NOT (is_super_admin=1) AND is_admin=1",
               "align admin flags with super admin");
    }
}

}  // namespace migrations
