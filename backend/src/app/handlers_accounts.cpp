#include "core.h"

namespace handlers {

void accountUpdateProfile(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string fullName = util::trim(http::input(req, "full_name"));
    std::string studentId = util::trim(http::input(req, "student_id"));
    if (fullName.empty() && studentId.empty()) {
        http::error(res, 400, "invalid_input", "没有可更新的字段");
        return;
    }
    if (!fullName.empty() && fullName.size() > 100) {
        http::error(res, 400, "invalid_input", "显示名不能超过 100 个字符");
        return;
    }
    if (!studentId.empty() && studentId.size() > 64) {
        http::error(res, 400, "invalid_input", "学号不能超过 64 个字符");
        return;
    }
    std::vector<std::string> sets;
    if (!fullName.empty()) sets.push_back("full_name=" + Db::nullable(fullName));
    if (!studentId.empty()) sets.push_back("student_id=" + Db::nullable(studentId));
    sets.push_back("updated_at=NOW()");
    if (Db::exec("UPDATE users SET " + util::join(sets, ", ") + " WHERE id=" +
                 Db::quote(user.id)) < 0) {
        http::error(res, 500, "update_failed", "资料更新失败，请稍后重试");
        return;
    }
    auto fresh = repo::findUserById(user.id);
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    if (fresh) obj["user"] = repo::userJson(*fresh, true);
    http::json(res, 200, JsonValue(obj));
}

}  // namespace handlers
