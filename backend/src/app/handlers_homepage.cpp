#include "core.h"
#include "app/logger.h"

#include <algorithm>

// Landing page ecosystem ("yatsso 生态"): the apps shown in the scrolling
// marquee on the public home page. The list is content, not security config —
// any logged-out visitor can read it, while only a super admin can edit it.

namespace {

struct HomepageApp {
    std::string id;
    std::string name;
    std::string description;
    std::string icon;
    std::string url;
    long long sortOrder = 0;
    bool isVisible = true;
    std::string updatedAt;
};

std::string cell(const std::map<std::string, std::string>& row, const char* key) {
    auto it = row.find(key);
    return it == row.end() ? std::string() : it->second;
}

HomepageApp rowToApp(const std::map<std::string, std::string>& row) {
    HomepageApp app;
    app.id = cell(row, "id");
    app.name = cell(row, "name");
    app.description = cell(row, "description");
    app.icon = cell(row, "icon");
    app.url = cell(row, "url");
    app.isVisible = cell(row, "is_visible") != "0";
    app.updatedAt = cell(row, "updated_at");
    try { app.sortOrder = std::stoll(cell(row, "sort_order")); } catch (...) {}
    return app;
}

std::vector<HomepageApp> listRows(const std::string& where) {
    std::vector<HomepageApp> apps;
    auto rows = Db::query(
        "SELECT id,name,description,icon,url,sort_order,is_visible,"
        "DATE_FORMAT(updated_at,'%Y-%m-%dT%H:%i:%sZ') AS updated_at"
        " FROM homepage_apps" + where + " ORDER BY sort_order ASC, name ASC");
    for (const auto& row : rows) apps.push_back(rowToApp(row));
    return apps;
}

JsonValue appJson(const HomepageApp& app, bool admin) {
    std::map<std::string, JsonValue> obj{
        {"id", JsonValue(app.id)},
        {"name", JsonValue(app.name)},
        {"description", JsonValue(app.description)},
        {"icon", JsonValue(app.icon)},
        {"url", JsonValue(app.url)},
    };
    if (admin) {
        obj["sort_order"] = jsonx::num(app.sortOrder);
        obj["is_visible"] = JsonValue(app.isVisible);
        obj["updated_at"] = JsonValue(app.updatedAt);
    }
    return JsonValue(obj);
}

void respondList(Response& res, const std::vector<HomepageApp>& apps, bool admin) {
    std::vector<JsonValue> items;
    for (const auto& app : apps) items.push_back(appJson(app, admin));
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                             {"ok", JsonValue(true)}, {"applications", JsonValue(items)}}));
}

// Landing page tiles link out to third-party sites, so they obey the same rule
// as OAuth redirect URIs: https only, plus localhost for local development.
bool validLink(const std::string& url) {
    if (url.empty() || url.size() > 512) return false;
    if (url.find('\n') != std::string::npos || url.find('\t') != std::string::npos ||
        url.find(' ') != std::string::npos || url.find('\\') != std::string::npos) {
        return false;
    }
    if (url.rfind("https://", 0) == 0) {
        std::string rest = url.substr(8);
        return !rest.empty() && rest[0] != '/' && rest[0] != ':' && rest[0] != '?';
    }
    auto localHost = [&](const std::string& prefix) {
        if (url.rfind(prefix, 0) != 0) return false;
        if (url.size() == prefix.size()) return true;
        char c = url[prefix.size()];
        return c == '/' || c == ':' || c == '?';
    };
    return localHost("http://localhost") || localHost("http://127.0.0.1");
}

// Same shape as the icon upload on applications: sniff the format, refuse
// anything that is not a real bitmap.
bool storeIcon(const Request& req, Response& res, const std::string& id,
               const std::string& prefix, std::string& url) {
    const UploadedFile* file = req.file("file");
    if (!file || file->data.size() > 2 * 1024 * 1024) {
        http::error(res, 400, "invalid_file", "请上传不超过 2MB 的图片");
        return false;
    }
    std::string ext;
    if (!images::extension(file->contentType, file->data, ext)) {
        http::error(res, 400, "invalid_file", "只支持 JPEG、PNG、GIF、WebP 图片");
        return false;
    }
    auto& cfg = AppConfig::get();
    std::string dir = util::pathJoin(cfg.dataRoot, "uploads/icons");
    if (!util::ensureDir(dir)) {
        http::error(res, 500, "storage_unavailable", "无法保存图标");
        return false;
    }
    std::string fileName = prefix + id + ext;
    if (!util::writeFile(util::pathJoin(dir, fileName), file->data)) {
        http::error(res, 500, "storage_unavailable", "无法保存图标");
        return false;
    }
    url = "/uploads/icons/" + fileName;
    return true;
}

}  // namespace

namespace handlers {

void homepageAppsPublic(const Request&, Response& res) {
    respondList(res, listRows(" WHERE is_visible=1"), false);
    // Short cache: the marquee is on the busiest public page, but an admin edit
    // still becomes visible almost immediately.
    res.headers["Cache-Control"] = "public, max-age=30";
}

void adminHomepageAppsList(const Request& req, Response& res) {
    User user;
    if (!authx::requireSuperAdmin(req, res, user)) return;
    respondList(res, listRows(""), true);
}

void adminHomepageAppsCreate(const Request& req, Response& res) {
    User user;
    if (!authx::requireSuperAdmin(req, res, user)) return;

    HomepageApp app;
    app.id = util::uuid4();
    app.name = util::trim(http::input(req, "name"));
    app.description = util::trim(http::input(req, "description"));
    app.icon = util::trim(http::input(req, "icon"));
    app.url = util::trim(http::input(req, "url"));

    if (app.name.empty()) {
        http::error(res, 400, "invalid_input", "应用名称不能为空");
        return;
    }
    if (!validLink(app.url)) {
        http::error(res, 400, "invalid_url", "跳转链接必须使用 https://（本地可用 http://localhost）");
        return;
    }

    // New tiles go to the end of the marquee unless a position is given.
    std::string sortInput = util::trim(http::input(req, "sort_order"));
    if (!sortInput.empty()) {
        try { app.sortOrder = std::stoll(sortInput); } catch (...) { app.sortOrder = 0; }
    } else {
        auto rows = Db::query("SELECT COALESCE(MAX(sort_order),0)+10 AS n FROM homepage_apps");
        if (!rows.empty()) {
            try { app.sortOrder = std::stoll(rows.front().at("n")); } catch (...) {}
        }
    }
    std::string visible = util::trim(http::input(req, "is_visible"));
    app.isVisible = visible.empty() ? true : (visible != "false" && visible != "0");

    std::string sql =
        "INSERT INTO homepage_apps (id,name,description,icon,url,sort_order,is_visible,"
        "created_at,updated_at) VALUES (" +
        Db::quote(app.id) + "," + Db::quote(app.name) + "," +
        Db::nullable(app.description) + "," + Db::nullable(app.icon) + "," +
        Db::quote(app.url) + "," + std::to_string(app.sortOrder) + "," +
        (app.isVisible ? "1" : "0") + ",NOW(),NOW())";
    if (Db::exec(sql) < 0) {
        http::error(res, 500, "create_failed", "首页应用添加失败");
        return;
    }
    applog::access("[homepage] add app id=" + app.id + " name=" + app.name +
                   " by=" + user.id);

    auto created = listRows(" WHERE id=" + Db::quote(app.id));
    http::ok(res, created.empty() ? appJson(app, true) : appJson(created.front(), true));
}

void adminHomepageAppsUpdate(const Request& req, Response& res) {
    User user;
    if (!authx::requireSuperAdmin(req, res, user)) return;

    std::string id = http::param(req, "id");
    auto rows = listRows(" WHERE id=" + Db::quote(id));
    if (rows.empty()) {
        http::error(res, 404, "not_found", "首页应用不存在");
        return;
    }
    HomepageApp app = rows.front();

    // Partial update: only fields present in the request are touched.
    std::string name = util::trim(http::input(req, "name"));
    std::string description = util::trim(http::input(req, "description"));
    std::string icon = util::trim(http::input(req, "icon"));
    std::string url = util::trim(http::input(req, "url"));
    std::string sortInput = util::trim(http::input(req, "sort_order"));
    std::string visible = util::trim(http::input(req, "is_visible"));

    if (!name.empty()) app.name = name;
    if (!description.empty()) app.description = description;
    if (!icon.empty()) app.icon = icon;
    if (!url.empty()) app.url = url;
    if (!sortInput.empty()) {
        try { app.sortOrder = std::stoll(sortInput); } catch (...) {}
    }
    if (!visible.empty()) app.isVisible = visible != "false" && visible != "0";

    if (app.name.empty()) {
        http::error(res, 400, "invalid_input", "应用名称不能为空");
        return;
    }
    if (!validLink(app.url)) {
        http::error(res, 400, "invalid_url", "跳转链接必须使用 https://（本地可用 http://localhost）");
        return;
    }

    std::string sql =
        "UPDATE homepage_apps SET name=" + Db::quote(app.name) +
        ", description=" + Db::nullable(app.description) +
        ", icon=" + Db::nullable(app.icon) +
        ", url=" + Db::quote(app.url) +
        ", sort_order=" + std::to_string(app.sortOrder) +
        ", is_visible=" + (app.isVisible ? "1" : "0") +
        ", updated_at=NOW() WHERE id=" + Db::quote(id);
    if (Db::exec(sql) < 0) {
        http::error(res, 500, "update_failed", "首页应用更新失败");
        return;
    }
    auto updated = listRows(" WHERE id=" + Db::quote(id));
    http::ok(res, updated.empty() ? appJson(app, true) : appJson(updated.front(), true));
}

void adminHomepageAppsDelete(const Request& req, Response& res) {
    User user;
    if (!authx::requireSuperAdmin(req, res, user)) return;
    std::string id = http::param(req, "id");
    if (Db::exec("DELETE FROM homepage_apps WHERE id=" + Db::quote(id)) < 0) {
        http::error(res, 500, "delete_failed", "删除失败");
        return;
    }
    applog::access("[homepage] delete app id=" + id + " by=" + user.id);
    http::ok(res, JsonValue(std::map<std::string, JsonValue>{{"ok", JsonValue(true)}}));
}

void adminHomepageAppsIcon(const Request& req, Response& res) {
    User user;
    if (!authx::requireSuperAdmin(req, res, user)) return;
    std::string id = http::param(req, "id");
    auto rows = listRows(" WHERE id=" + Db::quote(id));
    if (rows.empty()) {
        http::error(res, 404, "not_found", "首页应用不存在");
        return;
    }
    std::string url;
    if (!storeIcon(req, res, id, "homepage-", url)) return;
    Db::exec("UPDATE homepage_apps SET icon=" + Db::quote(url) +
             ", updated_at=NOW() WHERE id=" + Db::quote(id));
    std::map<std::string, JsonValue> obj;
    obj["url"] = JsonValue(url);
    http::ok(res, JsonValue(obj));
}

}  // namespace handlers
