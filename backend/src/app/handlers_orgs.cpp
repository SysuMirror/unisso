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

// Trim to at most 120 UTF-8 code points without cutting a multi-byte char.
std::string clampMessage(const std::string& raw) {
    std::string s = util::trim(raw);
    size_t bytes = 0, codepoints = 0;
    while (bytes < s.size() && codepoints < 120) {
        unsigned char c = static_cast<unsigned char>(s[bytes]);
        size_t len = 1;
        if ((c >> 5) == 0x6) len = 2;
        else if ((c >> 4) == 0xE) len = 3;
        else if ((c >> 3) == 0x1E) len = 4;
        if (bytes + len > s.size()) break;
        bytes += len;
        ++codepoints;
    }
    s.resize(bytes);
    return s;
}

JsonValue joinRequestJson(const std::map<std::string, std::string>& row) {
    auto pick = [&row](const char* key) {
        auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    std::map<std::string, JsonValue> org;
    org["id"] = JsonValue(pick("organization_id"));
    org["name"] = JsonValue(pick("org_name"));
    org["slug"] = JsonValue(pick("org_slug"));
    std::map<std::string, JsonValue> applicant;
    applicant["id"] = JsonValue(pick("user_id"));
    applicant["email"] = JsonValue(pick("email"));
    applicant["username"] = JsonValue(pick("username"));
    applicant["full_name"] = JsonValue(pick("full_name"));
    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(pick("id"));
    obj["organization"] = JsonValue(org);
    obj["user"] = JsonValue(applicant);
    obj["message"] = JsonValue(pick("message"));
    obj["status"] = JsonValue(pick("status"));
    obj["created_at"] = JsonValue(pick("created_at"));
    obj["review_note"] = JsonValue(pick("review_note"));
    obj["reviewed_at"] = JsonValue(pick("reviewed_at"));
    obj["reviewer_name"] = JsonValue(pick("reviewer_name"));
    return JsonValue(obj);
}

std::string joinRequestSql(const std::string& where) {
    return "SELECT jr.id,jr.organization_id,jr.user_id,jr.message,jr.status,jr.created_at,"
           "jr.review_note,jr.updated_at AS reviewed_at,"
           "rv.full_name AS reviewer_name,rv.username AS reviewer_username,"
           "o.name AS org_name,o.slug AS org_slug,u.email,u.username,u.full_name "
           "FROM org_join_requests jr "
           "JOIN organizations o ON o.id=jr.organization_id "
           "JOIN users u ON u.id=jr.user_id "
           "LEFT JOIN users rv ON rv.id=jr.reviewed_by "
           "WHERE " + where + " ";
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
    // Everyone browses every organization so they can apply to join one;
    // non-members get an empty role. request_status is the caller's latest
    // join request for that org ('' when none).
    std::string sql =
        "SELECT o.id,o.name,o.slug,o.description,o.created_at,"
        "COALESCE(om.role,'') AS role,"
        "(SELECT COUNT(*) FROM organization_members m WHERE m.organization_id=o.id) AS member_count,"
        "COALESCE((SELECT jr.status FROM org_join_requests jr WHERE jr.organization_id=o.id AND jr.user_id=" +
        Db::quote(user.id) + " ORDER BY jr.created_at DESC LIMIT 1),'') AS request_status "
        "FROM organizations o "
        "LEFT JOIN organization_members om ON om.organization_id=o.id AND om.user_id=" +
        Db::quote(user.id) + " ORDER BY o.name LIMIT 200";
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
        obj["joined"] = JsonValue(row.count("role") && !row.at("role").empty());
        obj["request_status"] = JsonValue(row.count("request_status") ? row.at("request_status") : "");
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

// POST /api/orgs/{id}/join-requests — any logged-in user applies to join.
void organizationsJoinApply(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string orgId = http::param(req, "id");
    auto org = Db::query("SELECT id,name FROM organizations WHERE id=" +
                         Db::quote(orgId) + " LIMIT 1");
    if (org.empty()) {
        http::error(res, 404, "not_found", "组织不存在");
        return;
    }
    auto member = Db::query("SELECT id FROM organization_members WHERE organization_id=" +
                            Db::quote(orgId) + " AND user_id=" + Db::quote(user.id) + " LIMIT 1");
    if (!member.empty()) {
        http::error(res, 409, "already_member", "你已经是该组织成员，无需申请");
        return;
    }
    auto pending = Db::query("SELECT id FROM org_join_requests WHERE organization_id=" +
                             Db::quote(orgId) + " AND user_id=" + Db::quote(user.id) +
                             " AND status='pending' LIMIT 1");
    if (!pending.empty()) {
        http::error(res, 409, "already_pending", "已提交过申请，正在等待审批");
        return;
    }
    std::string message = clampMessage(http::input(req, "message"));
    if (Db::exec("INSERT INTO org_join_requests (id,organization_id,user_id,message,status,created_at) "
                 "VALUES (" + Db::quote(util::uuid4()) + "," + Db::quote(orgId) + "," +
                 Db::quote(user.id) + "," + Db::nullable(message) + ",'pending',NOW())") < 0) {
        http::error(res, 500, "apply_failed", "申请提交失败，请稍后重试");
        return;
    }
    auto rows = Db::query(joinRequestSql("jr.user_id=" + Db::quote(user.id) +
                                         " AND jr.organization_id=" + Db::quote(orgId) +
                                         " ORDER BY jr.created_at DESC LIMIT 1"));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    if (!rows.empty()) obj["request"] = joinRequestJson(rows.front());
    http::json(res, 201, JsonValue(obj));
}

// GET /api/org-join-requests/mine — the caller's own applications.
void orgJoinRequestsMine(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto rows = Db::query(joinRequestSql("jr.user_id=" + Db::quote(user.id)) +
                          "ORDER BY jr.created_at DESC LIMIT 50");
    std::vector<JsonValue> items;
    for (const auto& row : rows) items.push_back(joinRequestJson(row));
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                             {"ok", JsonValue(true)}, {"requests", JsonValue(items)}}));
}

// GET /api/org-join-requests — pending applications the caller may approve
// (super admins see all; org admins see their orgs' requests).
void orgJoinRequestsPending(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::vector<JsonValue> items;
    if (user.isSuperAdmin || user.isAdmin) {
        auto rows = Db::query(joinRequestSql("jr.status='pending'") +
                              "ORDER BY jr.created_at ASC LIMIT 200");
        for (const auto& row : rows) items.push_back(joinRequestJson(row));
    } else {
        auto mine = Db::query("SELECT 1 FROM organization_members WHERE user_id=" +
                              Db::quote(user.id) + " AND role='admin' LIMIT 1");
        if (!mine.empty()) {
            auto rows = Db::query(
                joinRequestSql("jr.status='pending' AND jr.organization_id IN ("
                               "SELECT organization_id FROM organization_members WHERE user_id=" +
                               Db::quote(user.id) + " AND role='admin')") +
                "ORDER BY jr.created_at ASC LIMIT 200");
            for (const auto& row : rows) items.push_back(joinRequestJson(row));
        }
    }
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                             {"ok", JsonValue(true)}, {"requests", JsonValue(items)},
                             {"can_review", JsonValue(!items.empty())}}));
}

// POST /api/org-join-requests/{id}/review — approve or reject. Approvers are
// super admins or admins of the request's organization.
void orgJoinRequestReview(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = http::param(req, "id");
    auto rows = Db::query(joinRequestSql("jr.id=" + Db::quote(id) + " LIMIT 1"));
    if (rows.empty()) {
        http::error(res, 404, "not_found", "申请不存在");
        return;
    }
    const auto& request = rows.front();
    std::string orgId = request.at("organization_id");
    if (!canAdminOrg(user, orgId)) {
        http::error(res, 403, "forbidden", "需要该组织的管理员权限");
        return;
    }
    if (request.count("status") && request.at("status") != "pending") {
        http::error(res, 409, "already_reviewed", "该申请已被处理");
        return;
    }
    std::string action = util::lower(util::trim(http::input(req, "action")));
    bool approve = action == "approve" || action == "accept";
    bool reject = action == "reject" || action == "deny";
    if (!approve && !reject) {
        http::error(res, 400, "invalid_action", "action 只能是 approve 或 reject");
        return;
    }
    std::string note = clampMessage(http::input(req, "note"));
    if (approve) {
        auto target = repo::findUserById(request.at("user_id"));
        if (!target || !target->isActive) {
            http::error(res, 409, "user_unavailable", "申请人账号不存在或已被禁用");
            return;
        }
        Db::exec("INSERT IGNORE INTO organization_members (id,organization_id,user_id,role,created_at) "
                 "VALUES (" + Db::quote(util::uuid4()) + "," + Db::quote(orgId) + "," +
                 Db::quote(target->id) + ",'member',NOW())");
    }
    Db::exec("UPDATE org_join_requests SET status=" + Db::quote(approve ? "approved" : "rejected") +
             ",reviewed_by=" + Db::quote(user.id) + ",review_note=" + Db::nullable(note) +
             ",updated_at=NOW() WHERE id=" + Db::quote(id));
    auto updated = Db::query(joinRequestSql("jr.id=" + Db::quote(id) + " LIMIT 1"));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    if (!updated.empty()) obj["request"] = joinRequestJson(updated.front());
    http::ok(res, JsonValue(obj));
}

}  // namespace handlers
