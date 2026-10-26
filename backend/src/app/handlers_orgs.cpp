#include "core.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace {

std::string orgRole(const User& user, const std::string& orgId) {
    auto rows = Db::query("SELECT role FROM organization_members WHERE organization_id=" +
                          Db::quote(orgId) + " AND user_id=" + Db::quote(user.id) + " LIMIT 1");
    return rows.empty() ? "" : rows.front().at("role");
}

bool canAdminOrg(const User& user, const std::string& orgId) {
    if (user.isSuperAdmin || user.isAdmin) return true;
    return orgRole(user, orgId) == "admin";
}

bool validRole(const std::string& role) {
    return role == "admin" || role == "developer" || role == "member";
}

long long adminCount(const std::string& orgId) {
    auto rows = Db::query("SELECT COUNT(*) AS c FROM organization_members WHERE organization_id=" +
                          Db::quote(orgId) + " AND role='admin'");
    if (rows.empty()) return 0;
    try { return std::stoll(rows.front().at("c")); } catch (...) { return 0; }
}

JsonValue memberJson(const std::map<std::string, std::string>& row) {
    std::map<std::string, JsonValue> obj;
    auto pick = [&row](const char* key) {
        auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    obj["id"] = JsonValue(pick("id"));
    obj["email"] = JsonValue(pick("email"));
    obj["username"] = JsonValue(pick("username"));
    obj["full_name"] = JsonValue(pick("full_name"));
    obj["role"] = JsonValue(pick("role"));
    obj["is_admin"] = JsonValue(pick("is_admin") == "1");
    obj["is_super_admin"] = JsonValue(pick("is_super_admin") == "1");
    obj["is_active"] = JsonValue(pick("is_active") == "1");
    obj["joined_at"] = JsonValue(pick("joined_at"));
    return JsonValue(obj);
}

std::optional<std::map<std::string, std::string>> memberRow(const std::string& orgId,
                                                            const std::string& userId) {
    auto rows = Db::query(
        "SELECT u.id,u.email,u.username,u.full_name,u.is_admin,u.is_super_admin,u.is_active,"
        "om.role,om.created_at AS joined_at FROM organization_members om "
        "JOIN users u ON u.id=om.user_id WHERE om.organization_id=" + Db::quote(orgId) +
        " AND om.user_id=" + Db::quote(userId) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

std::string slugify(const std::string& name) {
    std::string slug;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            slug.push_back(static_cast<char>(std::tolower(c)));
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.size() > 64) slug.resize(64);
    return slug;
}

}  // namespace

namespace handlers {

void organizationsList(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string sql =
        "SELECT o.id,o.name,o.slug,o.description,o.created_at,"
        "COALESCE(om.role,'') AS role,"
        "(SELECT COUNT(*) FROM organization_members m WHERE m.organization_id=o.id) AS member_count "
        "FROM organizations o ";
    if (user.isSuperAdmin || user.isAdmin) {
        sql += "LEFT JOIN organization_members om ON om.organization_id=o.id AND om.user_id=" +
               Db::quote(user.id) + " ORDER BY o.name LIMIT 200";
    } else {
        sql += "JOIN organization_members om ON om.organization_id=o.id AND om.user_id=" +
               Db::quote(user.id) + " ORDER BY o.name LIMIT 200";
    }
    auto rows = Db::query(sql);
    std::vector<JsonValue> items;
    for (const auto& row : rows) {
        std::map<std::string, JsonValue> obj;
        obj["id"] = JsonValue(row.at("id"));
        obj["name"] = JsonValue(row.at("name"));
        obj["slug"] = JsonValue(row.at("slug"));
        obj["description"] = JsonValue(row.count("description") ? row.at("description") : "");
        obj["role"] = JsonValue(row.count("role") ? row.at("role") : "");
        long long members = 0;
        try { members = std::stoll(row.at("member_count")); } catch (...) {}
        obj["member_count"] = jsonx::num(members);
        obj["can_admin"] = JsonValue(user.isSuperAdmin || user.isAdmin ||
                                     (row.count("role") && row.at("role") == "admin"));
        items.push_back(JsonValue(obj));
    }
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                             {"ok", JsonValue(true)}, {"organizations", JsonValue(items)},
                             {"is_super_admin", JsonValue(user.isSuperAdmin || user.isAdmin)}}));
}

void organizationsCreate(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    if (!user.isSuperAdmin && !user.isAdmin) {
        http::error(res, 403, "super_admin_required", "只有超级管理员可以新建组织");
        return;
    }
    std::string name = util::trim(http::input(req, "name"));
    if (name.empty() || name.size() > 128) {
        http::error(res, 400, "invalid_name", "组织名称不能为空");
        return;
    }
    std::string slug = util::lower(util::trim(http::input(req, "slug")));
    if (slug.empty()) slug = slugify(name);
    static const std::regex slugPattern("^[a-z0-9][a-z0-9-]{1,63}$");
    if (!std::regex_match(slug, slugPattern)) {
        http::error(res, 400, "invalid_slug", "组织标识需为 2-64 位小写字母、数字或连字符");
        return;
    }
    auto clash = Db::query("SELECT id FROM organizations WHERE slug=" + Db::quote(slug) + " LIMIT 1");
    if (!clash.empty()) {
        http::error(res, 409, "slug_taken", "组织标识已被使用");
        return;
    }
    std::string id = util::uuid4();
    std::string description = util::trim(http::input(req, "description"));
    if (Db::exec("INSERT INTO organizations (id,name,slug,description,created_at,updated_at) "
                 "VALUES (" + Db::quote(id) + "," + Db::quote(name) + "," + Db::quote(slug) + "," +
                 Db::nullable(description) + ",NOW(),NOW())") < 0) {
        http::error(res, 500, "create_failed", "组织创建失败");
        return;
    }
    Db::exec("INSERT INTO organization_members (id,organization_id,user_id,role,created_at) "
             "VALUES (" + Db::quote(util::uuid4()) + "," + Db::quote(id) + "," +
             Db::quote(user.id) + ",'admin',NOW())");
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["organization"] = JsonValue(std::map<std::string, JsonValue>{
        {"id", JsonValue(id)}, {"name", JsonValue(name)},
        {"slug", JsonValue(slug)}, {"role", JsonValue("admin")}});
    http::json(res, 201, JsonValue(obj));
}

void organizationsGet(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string orgId = http::param(req, "id");
    if (!canAdminOrg(user, orgId)) {
        http::error(res, 403, "forbidden", "需要组织管理员权限");
        return;
    }
    auto org = Db::query("SELECT id,name,slug,description FROM organizations WHERE id=" +
                         Db::quote(orgId) + " LIMIT 1");
    if (org.empty()) {
        http::error(res, 404, "not_found", "组织不存在");
        return;
    }
    auto rows = Db::query(
        "SELECT u.id,u.email,u.username,u.full_name,u.is_admin,u.is_super_admin,u.is_active,"
        "om.role,om.created_at AS joined_at FROM organization_members om "
        "JOIN users u ON u.id=om.user_id WHERE om.organization_id=" + Db::quote(orgId) +
        " ORDER BY FIELD(om.role,'admin','developer','member'), u.username, u.email LIMIT 1000");
    std::vector<JsonValue> members;
    for (const auto& row : rows) members.push_back(memberJson(row));
    std::map<std::string, JsonValue> orgJson;
    orgJson["id"] = JsonValue(org.front().at("id"));
    orgJson["name"] = JsonValue(org.front().at("name"));
    orgJson["slug"] = JsonValue(org.front().at("slug"));
    orgJson["description"] = JsonValue(org.front().count("description") ? org.front().at("description") : "");
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["organization"] = JsonValue(orgJson);
    obj["members"] = JsonValue(members);
    obj["can_manage_admins"] = JsonValue(true);
    http::json(res, 200, JsonValue(obj));
}

void organizationsAddMember(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string orgId = http::param(req, "id");
    if (!canAdminOrg(user, orgId)) {
        http::error(res, 403, "forbidden", "需要组织管理员权限");
        return;
    }
    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string role = util::lower(util::trim(http::input(req, "role")));
    if (role.empty()) role = "member";
    if (!validRole(role)) {
        http::error(res, 400, "invalid_role", "角色只能是管理员、开发者或成员");
        return;
    }
    auto target = repo::findUserByEmail(email);
    if (!target) {
        http::error(res, 404, "user_not_found", "该邮箱还没有注册账号");
        return;
    }
    auto existing = memberRow(orgId, target->id);
    if (existing) {
        std::string currentRole = existing->at("role");
        if (currentRole == "admin" && role != "admin" && adminCount(orgId) <= 1) {
            http::error(res, 400, "last_admin", "组织至少需要保留一名管理员");
            return;
        }
        Db::exec("UPDATE organization_members SET role=" + Db::quote(role) +
                 " WHERE organization_id=" + Db::quote(orgId) + " AND user_id=" +
                 Db::quote(target->id));
    } else {
        Db::exec("INSERT INTO organization_members (id,organization_id,user_id,role,created_at) "
                 "VALUES (" + Db::quote(util::uuid4()) + "," + Db::quote(orgId) + "," +
                 Db::quote(target->id) + "," + Db::quote(role) + ",NOW())");
    }
    auto row = memberRow(orgId, target->id);
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    if (row) obj["member"] = memberJson(*row);
    http::ok(res, JsonValue(obj));
}

void organizationsSetMember(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string orgId = http::param(req, "id");
    std::string userId = http::param(req, "userId");
    if (!canAdminOrg(user, orgId)) {
        http::error(res, 403, "forbidden", "需要组织管理员权限");
        return;
    }
    std::string role = util::lower(util::trim(http::input(req, "role")));
    if (!validRole(role)) {
        http::error(res, 400, "invalid_role", "角色只能是管理员、开发者或成员");
        return;
    }
    auto current = memberRow(orgId, userId);
    if (!current) {
        http::error(res, 404, "not_found", "该用户不在组织中");
        return;
    }
    if (current->at("role") == "admin" && role != "admin" && adminCount(orgId) <= 1) {
        http::error(res, 400, "last_admin", "组织至少需要保留一名管理员");
        return;
    }
    Db::exec("UPDATE organization_members SET role=" + Db::quote(role) +
             " WHERE organization_id=" + Db::quote(orgId) + " AND user_id=" + Db::quote(userId));
    auto row = memberRow(orgId, userId);
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    if (row) obj["member"] = memberJson(*row);
    http::ok(res, JsonValue(obj));
}

void organizationsRemoveMember(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string orgId = http::param(req, "id");
    std::string userId = http::param(req, "userId");
    if (!canAdminOrg(user, orgId)) {
        http::error(res, 403, "forbidden", "需要组织管理员权限");
        return;
    }
    if (userId == user.id) {
        http::error(res, 400, "cannot_remove_self", "不能移除自己；可先把其他成员设为管理员");
        return;
    }
    auto current = memberRow(orgId, userId);
    if (!current) {
        http::error(res, 404, "not_found", "该用户不在组织中");
        return;
    }
    if (current->at("role") == "admin" && adminCount(orgId) <= 1) {
        http::error(res, 400, "last_admin", "组织至少需要保留一名管理员");
        return;
    }
    Db::exec("DELETE FROM organization_members WHERE organization_id=" + Db::quote(orgId) +
             " AND user_id=" + Db::quote(userId));
    http::ok(res);
}

}  // namespace handlers
