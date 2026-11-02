#include "core.h"
#include "app/logger.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>

namespace {

bool managesOrg(const User& user, const std::string& organizationId) {
    if (user.isAdmin || user.isSuperAdmin) return true;
    if (organizationId.empty()) return false;
    auto rows = Db::query(
        "SELECT role FROM organization_members WHERE organization_id=" +
        Db::quote(organizationId) + " AND user_id=" + Db::quote(user.id) + " LIMIT 1");
    if (rows.empty()) return false;
    return rows.front().at("role") == "admin" || rows.front().at("role") == "developer";
}

bool canManage(const User& user, const Application& app) {
    if (user.isAdmin || user.isSuperAdmin) return true;
    if (!app.ownerId.empty() && app.ownerId == user.id) return true;
    return managesOrg(user, app.organizationId);
}

bool canManageApps(const User& user) {
    if (user.isAdmin || user.isSuperAdmin) return true;
    // Any organization member may register apps under their org (join first,
    // then develop) — see appsCreate. Members can apply to join via
    // org join-requests.
    auto rows = Db::query(
        "SELECT 1 FROM organization_members WHERE user_id=" + Db::quote(user.id) + " LIMIT 1");
    if (!rows.empty()) return true;
    auto owned = Db::query(
        "SELECT 1 FROM applications WHERE owner_id=" + Db::quote(user.id) + " LIMIT 1");
    return !owned.empty();
}

std::string newClientId() {
    return "app_" + util::randomHex(8);
}

std::string newClientSecret() {
    return util::randomHex(16);
}


bool validRedirectUri(const std::string& uri) {
    if (uri.empty() || uri.size() > 512) return false;
    if (uri.find('#') != std::string::npos || uri.find('@') != std::string::npos ||
        uri.find('\\') != std::string::npos || uri.find(' ') != std::string::npos ||
        uri.find('\t') != std::string::npos || uri.find('\n') != std::string::npos) {
        return false;
    }
    auto localHost = [&](const std::string& prefix) {
        if (uri.rfind(prefix, 0) != 0) return false;
        if (uri.size() == prefix.size()) return true;
        char c = uri[prefix.size()];
        return c == '/' || c == ':' || c == '?';
    };
    if (uri.rfind("https://", 0) == 0) {
        std::string rest = uri.substr(8);
        if (rest.empty() || rest[0] == '/' || rest[0] == ':' || rest[0] == '?') return false;
        return true;
    }
    return localHost("http://localhost") || localHost("http://127.0.0.1");
}

bool validScopes(const std::vector<std::string>& scopes) {
    static const std::set<std::string> allowed = {
        "openid", "profile", "profile:username", "profile:name",
        "profile:avatar", "profile:student_id", "email", "roles"};
    if (scopes.empty()) return false;
    for (const auto& scope : scopes) {
        if (allowed.find(scope) == allowed.end()) return false;
    }
    return true;
}

bool normalizeRedirectUris(std::vector<std::string>& uris) {
    for (auto& uri : uris) uri = util::trim(uri);
    uris.erase(std::remove_if(uris.begin(), uris.end(),
                              [](const std::string& v) { return v.empty(); }),
               uris.end());
    if (uris.empty() || uris.size() > 10) return false;
    for (const auto& uri : uris) {
        if (!validRedirectUri(uri)) return false;
    }
    return true;
}

bool imageExtension(const std::string& contentType, const std::string& data, std::string& ext) {
    if (contentType == "image/jpeg" && data.size() > 3 &&
        static_cast<unsigned char>(data[0]) == 0xff &&
        static_cast<unsigned char>(data[1]) == 0xd8) {
        ext = ".jpg";
        return true;
    }
    if (contentType == "image/png" && data.size() > 8 &&
        data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0) {
        ext = ".png";
        return true;
    }
    if (contentType == "image/gif" && data.size() > 6 &&
        (data.compare(0, 6, "GIF87a") == 0 || data.compare(0, 6, "GIF89a") == 0)) {
        ext = ".gif";
        return true;
    }
    if (contentType == "image/webp" && data.size() > 12 &&
        data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0) {
        ext = ".webp";
        return true;
    }
    return false;
}

std::string stringArraySql(const std::vector<std::string>& values) {
    return Db::quote(JsonValue(values).toJson());
}

}  // namespace

namespace handlers {

void appIntegrationSkill(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    if (!canManageApps(user)) {
        http::error(res, 403, "forbidden", "只有应用管理员或开发者可以下载接入文档");
        return;
    }
    std::vector<std::string> candidates;
    if (const char* configured = std::getenv("UNISSO_INTEGRATION_SKILL")) {
        if (configured[0]) candidates.emplace_back(configured);
    }
    candidates.push_back("../.claude/skills/unisso-integration/SKILL.md");
    candidates.push_back(".claude/skills/unisso-integration/SKILL.md");
    candidates.push_back("/home/cloud/deploy/unisso/.claude/skills/unisso-integration/SKILL.md");
    candidates.push_back("/home/cloud/deploy/unisso-v2/.claude/skills/unisso-integration/SKILL.md");
    for (const auto& path : candidates) {
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
        if (content.empty()) continue;
        res.statusCode = 200;
        res.headers["Content-Type"] = "text/markdown; charset=utf-8";
        res.headers["Content-Disposition"] =
            "attachment; filename=\"unisso-integration-SKILL.md\"";
        res.headers["Cache-Control"] = "no-store";
        res.body = content;
        return;
    }
    applog::error("[apps] integration skill file not found");
    http::error(res, 404, "skill_not_found", "接入文档暂不可用，请联系平台管理员");
}

void appsList(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::vector<Application> apps = user.isAdmin
        ? repo::listApplications()
        : repo::listApplicationsForUser(user.id);
    std::vector<JsonValue> items;
    for (const auto& app : apps) items.push_back(repo::applicationJson(app, false));
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                             {"ok", JsonValue(true)}, {"applications", JsonValue(items)}}));
}

void appsCreate(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string name = util::trim(http::input(req, "name"));
    if (name.empty()) {
        http::error(res, 400, "invalid_input", "应用名称不能为空");
        return;
    }
    std::vector<std::string> redirectUris = http::inputArray(req, "redirect_uris");
    if (!normalizeRedirectUris(redirectUris)) {
        http::error(res, 400, "invalid_redirect_uri",
                    "回调地址必须使用 https://，或本地 http://localhost / http://127.0.0.1");
        return;
    }
    std::vector<std::string> allowedScopes = http::inputArray(req, "allowed_scopes");
    if (allowedScopes.empty()) allowedScopes.push_back("openid");
    if (!validScopes(allowedScopes)) {
        http::error(res, 400, "invalid_scope", "包含不支持的权限范围");
        return;
    }

    std::string requestedOrg = util::trim(http::input(req, "organization_id"));
    std::string organizationId;
    if (user.isAdmin || user.isSuperAdmin) {
        organizationId = requestedOrg.empty() ? constants::kDefaultOrganizationId : requestedOrg;
        auto org = Db::query("SELECT id FROM organizations WHERE id=" +
                             Db::quote(organizationId) + " LIMIT 1");
        if (org.empty()) {
            http::error(res, 400, "invalid_organization", "组织不存在");
            return;
        }
    } else {
        auto orgs = Db::query(
            "SELECT organization_id FROM organization_members WHERE user_id=" +
            Db::quote(user.id) +
            " ORDER BY FIELD(role,'admin','developer','member'), created_at");
        if (orgs.empty()) {
            http::error(res, 403, "org_member_required",
                        "请先在「组织」页面申请加入一个组织，通过后即可在该组织下注册应用");
            return;
        }
        bool allowed = false;
        for (const auto& row : orgs) {
            if (requestedOrg.empty() || row.at("organization_id") == requestedOrg) {
                organizationId = row.at("organization_id");
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            http::error(res, 403, "forbidden_organization", "你不是该组织的管理员或开发者");
            return;
        }
    }
    bool confidential = true;
    std::string confidentialInput = http::input(req, "is_confidential");
    if (!confidentialInput.empty()) confidential = confidentialInput != "false" && confidentialInput != "0";

    Application app;
    app.id = util::uuid4();
    app.clientId = newClientId();
    app.name = name;
    app.description = util::trim(http::input(req, "description"));
    app.icon = util::trim(http::input(req, "icon"));
    app.redirectUris = redirectUris;
    app.allowedScopes = allowedScopes;
    app.homepageUrl = util::trim(http::input(req, "homepage_url"));
    app.isActive = true;
    app.isConfidential = confidential;
    app.ownerId = user.id;
    app.organizationId = organizationId;
    app.createdAt = util::nowIso();

    std::string rawSecret;
    std::string secretHash;
    if (confidential) {
        rawSecret = newClientSecret();
        try { secretHash = password::hash(rawSecret); }
        catch (const std::exception&) {
            http::error(res, 500, "create_failed", "无法生成客户端密钥");
            return;
        }
    }

    std::string sql =
        "INSERT INTO applications (id,client_id,client_secret,name,description,icon,"
        "redirect_uris,allowed_scopes,homepage_url,is_active,is_confidential,owner_id,"
        "organization_id,created_at,updated_at) VALUES (" +
        Db::quote(app.id) + "," + Db::quote(app.clientId) + "," + Db::quote(secretHash) + "," +
        Db::quote(app.name) + "," + Db::nullable(app.description) + "," +
        Db::nullable(app.icon) + "," +
        stringArraySql(app.redirectUris) + "," + stringArraySql(app.allowedScopes) + "," +
        Db::nullable(app.homepageUrl) + ",1," + (confidential ? "1" : "0") + "," +
        Db::quote(user.id) + "," + Db::nullable(organizationId) + ",NOW(),NOW())";
    if (Db::exec(sql) < 0) {
        http::error(res, 500, "create_failed", "应用创建失败");
        return;
    }

    std::map<std::string, JsonValue> obj;
    obj["application"] = repo::applicationJson(app, false);
    obj["client_id"] = JsonValue(app.clientId);
    if (confidential) obj["client_secret"] = JsonValue(rawSecret);
    http::ok(res, JsonValue(obj));
}

void appsGet(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto app = repo::findApplicationById(http::param(req, "id"));
    if (!app || !canManage(user, *app)) {
        http::error(res, 404, "not_found", "应用不存在");
        return;
    }
    http::ok(res, repo::applicationJson(*app, false));
}

void appsUpdate(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto app = repo::findApplicationById(http::param(req, "id"));
    if (!app || !canManage(user, *app)) {
        http::error(res, 404, "not_found", "应用不存在");
        return;
    }
    std::string name = util::trim(http::input(req, "name"));
    std::string description = util::trim(http::input(req, "description"));
    std::string homepage = util::trim(http::input(req, "homepage_url"));
    std::string icon = util::trim(http::input(req, "icon"));
    std::vector<std::string> redirectUris = http::inputArray(req, "redirect_uris");
    std::vector<std::string> allowedScopes = http::inputArray(req, "allowed_scopes");
    std::string active = http::input(req, "is_active");

    if (name.empty()) name = app->name;
    if (description.empty()) description = app->description;
    if (homepage.empty()) homepage = app->homepageUrl;
    if (icon.empty()) icon = app->icon;
    if (redirectUris.empty()) redirectUris = app->redirectUris;
    if (allowedScopes.empty()) allowedScopes = app->allowedScopes;
    if (!normalizeRedirectUris(redirectUris)) {
        http::error(res, 400, "invalid_redirect_uri",
                    "回调地址必须使用 https://，或本地 http://localhost / http://127.0.0.1");
        return;
    }
    allowedScopes.erase(std::remove(allowedScopes.begin(), allowedScopes.end(), "groups"),
                        allowedScopes.end());
    if (!validScopes(allowedScopes)) {
        http::error(res, 400, "invalid_scope", "包含不支持的权限范围");
        return;
    }
    bool redirectsChanged = redirectUris != app->redirectUris;
    bool scopesChanged = allowedScopes != app->allowedScopes;
    bool isActive = app->isActive;
    if (!active.empty()) isActive = active != "false" && active != "0";

    std::string sql =
        "UPDATE applications SET name=" + Db::quote(name) +
        ", description=" + Db::nullable(description) +
        ", redirect_uris=" + stringArraySql(redirectUris) +
        ", allowed_scopes=" + stringArraySql(allowedScopes) +
        ", homepage_url=" + Db::nullable(homepage) +
        ", icon=" + Db::nullable(icon) +
        ", is_active=" + (isActive ? "1" : "0") +
        ", updated_at=NOW() WHERE id=" + Db::quote(app->id);
    if (Db::exec(sql) < 0) {
        http::error(res, 500, "update_failed", "应用更新失败");
        return;
    }
    if (redirectsChanged || scopesChanged) {
        // Existing grants were made against the previous URI/scope set.
        Db::exec("DELETE FROM user_consents WHERE application_id=" + Db::quote(app->id));
        Db::exec("UPDATE access_tokens SET revoked=1 WHERE client_id=" + Db::quote(app->clientId));
        Db::exec("UPDATE refresh_tokens SET revoked=1 WHERE client_id=" + Db::quote(app->clientId));
    }
    auto updated = repo::findApplicationById(app->id);
    http::ok(res, repo::applicationJson(updated ? *updated : *app, false));
}

void appsDelete(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto app = repo::findApplicationById(http::param(req, "id"));
    if (!app || !canManage(user, *app)) {
        http::error(res, 404, "not_found", "应用不存在");
        return;
    }
    Db::exec("DELETE FROM user_consents WHERE application_id=" + Db::quote(app->id));
    Db::exec("DELETE FROM applications WHERE id=" + Db::quote(app->id));
    http::ok(res);
}

void appsResetSecret(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto app = repo::findApplicationById(http::param(req, "id"));
    if (!app || !canManage(user, *app)) {
        http::error(res, 404, "not_found", "应用不存在");
        return;
    }
    if (!app->isConfidential) {
        http::error(res, 400, "public_client", "公开客户端没有 secret");
        return;
    }
    std::string rawSecret = newClientSecret();
    std::string secretHash;
    try { secretHash = password::hash(rawSecret); }
    catch (const std::exception&) {
        http::error(res, 500, "reset_failed", "无法生成新密钥");
        return;
    }
    Db::exec("UPDATE applications SET client_secret=" + Db::quote(secretHash) +
             ", updated_at=NOW() WHERE id=" + Db::quote(app->id));
    std::map<std::string, JsonValue> obj;
    obj["client_secret"] = JsonValue(rawSecret);
    http::ok(res, JsonValue(obj));
}

void appsIcon(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto app = repo::findApplicationById(http::param(req, "id"));
    if (!app || !canManage(user, *app)) {
        http::error(res, 404, "not_found", "应用不存在");
        return;
    }
    const UploadedFile* file = req.file("file");
    if (!file || file->data.size() > 2 * 1024 * 1024) {
        http::error(res, 400, "invalid_file", "请上传不超过 2MB 的图片");
        return;
    }
    std::string ext;
    if (!imageExtension(file->contentType, file->data, ext)) {
        http::error(res, 400, "invalid_file", "只支持 JPEG、PNG、GIF、WebP 图片");
        return;
    }
    auto& cfg = AppConfig::get();
    std::string dir = util::pathJoin(cfg.dataRoot, "uploads/icons");
    if (!util::ensureDir(dir)) {
        http::error(res, 500, "storage_unavailable", "无法保存图标");
        return;
    }
    std::string fileName = app->id + ext;
    std::string path = util::pathJoin(dir, fileName);
    if (!util::writeFile(path, file->data)) {
        http::error(res, 500, "storage_unavailable", "无法保存图标");
        return;
    }
    std::string url = "/uploads/icons/" + fileName;
    Db::exec("UPDATE applications SET icon=" + Db::quote(url) +
             ", updated_at=NOW() WHERE id=" + Db::quote(app->id));
    std::map<std::string, JsonValue> obj;
    obj["url"] = JsonValue(url);
    http::ok(res, JsonValue(obj));
}

void consentsList(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::vector<JsonValue> items;
    for (const auto& consent : repo::listConsents(user.id, true)) {
        items.push_back(repo::consentJson(consent));
    }
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                             {"ok", JsonValue(true)}, {"consents", JsonValue(items)}}));
}

void consentsUpdate(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = http::param(req, "id");
    auto rows = Db::query("SELECT * FROM user_consents WHERE id=" + Db::quote(id) +
                          " AND user_id=" + Db::quote(user.id) + " AND is_active=1 LIMIT 1");
    if (rows.empty()) {
        http::error(res, 404, "not_found", "授权记录不存在");
        return;
    }
    std::vector<std::string> scopes = http::inputArray(req, "scopes");
    if (scopes.empty()) {
        http::error(res, 400, "invalid_input", "至少保留一个权限");
        return;
    }
    Db::exec("UPDATE user_consents SET scopes=" + stringArraySql(scopes) +
             " WHERE id=" + Db::quote(id) + " AND user_id=" + Db::quote(user.id));
    http::ok(res);
}

void consentsRevoke(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = http::param(req, "id");
    auto rows = Db::query("SELECT uc.application_id, a.client_id FROM user_consents uc "
                          "JOIN applications a ON a.id=uc.application_id WHERE uc.id=" +
                          Db::quote(id) + " AND uc.user_id=" + Db::quote(user.id) +
                          " AND uc.is_active=1 LIMIT 1");
    if (rows.empty()) {
        http::error(res, 404, "not_found", "授权记录不存在");
        return;
    }
    std::string applicationId = rows.front()["application_id"];
    std::string clientId = rows.front()["client_id"];
    Db::exec("UPDATE user_consents SET is_active=0, revoked_at=NOW() WHERE id=" +
             Db::quote(id) + " AND user_id=" + Db::quote(user.id));
    Db::exec("UPDATE access_tokens SET revoked=1 WHERE user_id=" + Db::quote(user.id) +
             " AND client_id=" + Db::quote(clientId) + " AND revoked=0");
    Db::exec("UPDATE refresh_tokens SET revoked=1 WHERE user_id=" + Db::quote(user.id) +
             " AND client_id=" + Db::quote(clientId) + " AND revoked=0");
    http::ok(res);
}

void adminUsersList(const Request& req, Response& res) {
    User admin;
    if (!authx::requireSuperAdmin(req, res, admin)) return;
    std::vector<JsonValue> items;
    for (const auto& user : repo::listUsers()) items.push_back(repo::userJson(user, true));
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                             {"ok", JsonValue(true)}, {"users", JsonValue(items)}}));
}

void adminUsersCreate(const Request& req, Response& res) {
    User admin;
    if (!authx::requireSuperAdmin(req, res, admin)) return;
    std::string username = util::trim(http::input(req, "username"));
    std::string password = http::input(req, "password");
    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string fullName = util::trim(http::input(req, "full_name"));
    std::string isAdminValue = http::input(req, "is_admin");
    bool isAdmin = isAdminValue == "true" || isAdminValue == "1";
    if (username.size() < 3) {
        http::error(res, 400, "invalid_input", "用户名至少 3 个字符");
        return;
    }
    if (!email.empty()) {
        static const std::regex pattern(R"(^[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}$)");
        if (!std::regex_match(email, pattern)) {
            http::error(res, 400, "invalid_input", "邮箱格式不正确");
            return;
        }
        auto clash = Db::query("SELECT id FROM users WHERE email=" + Db::quote(email) + " LIMIT 1");
        if (!clash.empty()) {
            http::error(res, 409, "email_exists", "该邮箱已存在");
            return;
        }
    } else {
        email = username + "@local.invalid";
    }
    auto clash = Db::query("SELECT id FROM users WHERE username=" + Db::quote(username) + " LIMIT 1");
    if (!clash.empty()) {
        http::error(res, 409, "username_exists", "该用户名已存在");
        return;
    }
    try {
        password::validateStrength(password);
    } catch (const std::exception& e) {
        http::error(res, 400, "invalid_input", e.what());
        return;
    }

    User user;
    user.id = util::uuid4();
    user.email = email;
    user.emailVerified = true;
    user.passwordHash = password::hash(password);
    user.username = username;
    user.fullName = fullName;
    user.isActive = true;
    user.isAdmin = isAdmin;
    if (!repo::createUser(user)) {
        http::error(res, 500, "create_failed", "用户创建失败");
        return;
    }
    auto created = repo::findUserById(user.id);
    http::ok(res, repo::userJson(created ? *created : user, true));
}

void adminUsersUpdate(const Request& req, Response& res) {
    User admin;
    if (!authx::requireSuperAdmin(req, res, admin)) return;
    std::string id = http::param(req, "id");
    auto target = repo::findUserById(id);
    if (!target) {
        http::error(res, 404, "not_found", "用户不存在");
        return;
    }
    std::string active = http::input(req, "is_active");
    std::string isAdminValue = http::input(req, "is_admin");
    std::string isSuperValue = http::input(req, "is_super_admin");
    std::string fullName = util::trim(http::input(req, "full_name"));
    std::string studentId = util::trim(http::input(req, "student_id"));
    std::string username = util::trim(http::input(req, "username"));
    std::string password = http::input(req, "password");

    auto activeSuperAdmins = []() -> long long {
        auto rows = Db::query(
            "SELECT COUNT(*) AS c FROM users WHERE is_super_admin=1 AND is_active=1");
        if (rows.empty()) return 0;
        try { return std::stoll(rows.front().at("c")); } catch (...) { return 0; }
    };
    std::vector<std::string> sets;
    bool disableRequested = !active.empty() && (active == "false" || active == "0");
    if (disableRequested && target->isSuperAdmin && activeSuperAdmins() <= 1) {
        http::error(res, 400, "last_super_admin", "至少需要保留一名可用的超级管理员");
        return;
    }
    if (!active.empty()) sets.push_back("is_active=" + std::string(disableRequested ? "0" : "1"));
    if (!isAdminValue.empty()) sets.push_back("is_admin=" + std::string(isAdminValue == "true" || isAdminValue == "1" ? "1" : "0"));
    if (!isSuperValue.empty()) {
        bool makeSuper = isSuperValue == "true" || isSuperValue == "1";
        if (!makeSuper) {
            if (id == admin.id) {
                http::error(res, 400, "cannot_demote_self", "不能取消自己的超级管理员身份");
                return;
            }
            if (target->isSuperAdmin && target->isActive && activeSuperAdmins() <= 1) {
                http::error(res, 400, "last_super_admin", "至少需要保留一名可用的超级管理员");
                return;
            }
            sets.push_back("is_super_admin=0");
        } else {
            sets.push_back("is_super_admin=1");
        }
    }
    if (!fullName.empty()) sets.push_back("full_name=" + Db::quote(fullName));
    if (!studentId.empty()) sets.push_back("student_id=" + Db::quote(studentId));
    if (!username.empty() && username != target->username) {
        auto clash = Db::query("SELECT id FROM users WHERE username=" + Db::quote(username) + " LIMIT 1");
        if (!clash.empty()) {
            http::error(res, 409, "username_exists", "该用户名已存在");
            return;
        }
        sets.push_back("username=" + Db::quote(username));
    }
    if (!password.empty()) {
        try {
            password::validateStrength(password);
            sets.push_back("password_hash=" + Db::quote(password::hash(password)));
        } catch (const std::exception& e) {
            http::error(res, 400, "invalid_input", e.what());
            return;
        }
    }
    if (sets.empty()) {
        http::ok(res);
        return;
    }
    sets.push_back("updated_at=NOW()");
    std::string sql = "UPDATE users SET " + util::join(sets, ", ") + " WHERE id=" + Db::quote(id);
    if (Db::exec(sql) < 0) {
        http::error(res, 500, "update_failed", "用户更新失败");
        return;
    }
    auto updated = repo::findUserById(id);
    http::ok(res, repo::userJson(updated ? *updated : *target, true));
}

void adminUsersDelete(const Request& req, Response& res) {
    User admin;
    if (!authx::requireSuperAdmin(req, res, admin)) return;
    std::string id = http::param(req, "id");
    if (id == admin.id) {
        http::error(res, 400, "invalid_operation", "不能删除当前登录用户");
        return;
    }
    auto target = repo::findUserById(id);
    if (!target) {
        http::error(res, 404, "not_found", "用户不存在");
        return;
    }
    if (target->isSuperAdmin && target->isActive) {
        auto rows = Db::query(
            "SELECT COUNT(*) AS c FROM users WHERE is_super_admin=1 AND is_active=1");
        long long superCount = 0;
        if (!rows.empty()) {
            try { superCount = std::stoll(rows.front().at("c")); } catch (...) {}
        }
        if (superCount <= 1) {
            http::error(res, 400, "last_super_admin", "至少需要保留一名可用的超级管理员");
            return;
        }
    }
    // Face records: unlink this account; delete the person only when no other
    // account references it.
    auto faceRows = Db::query(
        "SELECT uf.id,uf.face_person_id,fp.provider_person_id,fp.external_id "
        "FROM user_faces uf JOIN face_persons fp ON fp.id=uf.face_person_id "
        "WHERE uf.user_id=" + Db::quote(id));
    for (const auto& row : faceRows) {
        std::string facePersonId = row.at("face_person_id");
        std::string provider = row.count("provider_person_id") ? row.at("provider_person_id") : "";
        std::string external = row.count("external_id") ? row.at("external_id") : "";
        Db::exec("DELETE FROM user_faces WHERE id=" + Db::quote(row.at("id")));
        auto remaining = Db::query("SELECT COUNT(*) AS c FROM user_faces WHERE face_person_id=" +
                                   Db::quote(facePersonId));
        long long left = 0;
        if (!remaining.empty()) {
            try { left = std::stoll(remaining.front().at("c")); } catch (...) {}
        }
        if (left == 0) {
            std::string faceError;
            bool deleted = !external.empty() &&
                           face::deletePersonByExternalId(external, faceError);
            if (!deleted && !provider.empty()) {
                deleted = face::deletePerson(provider, faceError);
            }
            if (!deleted) {
                applog::error("[admin] face delete failed for " +
                              (external.empty() ? provider : external) + ": " + faceError);
            }
            Db::exec("DELETE FROM face_persons WHERE id=" + Db::quote(facePersonId));
        }
    }
    Db::exec("DELETE FROM trusted_devices WHERE user_id=" + Db::quote(id));
    Db::exec("DELETE FROM face_audits WHERE user_id=" + Db::quote(id));
    Db::exec("DELETE FROM account_peer_links WHERE user_a_id=" + Db::quote(id) +
             " OR user_b_id=" + Db::quote(id));
    Db::exec("DELETE FROM account_links WHERE canonical_user_id=" + Db::quote(id) +
             " OR linked_user_id=" + Db::quote(id));
    Db::exec("DELETE FROM user_identities WHERE user_id=" + Db::quote(id));
    Db::exec("DELETE FROM user_consents WHERE user_id=" + Db::quote(id));
    Db::exec("DELETE FROM user_roles WHERE user_id=" + Db::quote(id));
    Db::exec("DELETE FROM users WHERE id=" + Db::quote(id));
    http::ok(res);
}

}  // namespace handlers
