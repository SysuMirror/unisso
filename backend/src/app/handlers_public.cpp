#include "core.h"
#include "app/logger.h"

#include <cstdlib>

namespace {

std::string absolute(const std::string& path) {
    return AppConfig::get().issuer + path;
}

}  // namespace

namespace handlers {

bool loadIntegrationSkillMarkdown(const std::string& kind, std::string& fileName,
                                  std::string& content) {
    // kind == "external" is the third-party (跳转授权) integration guide; every
    // other value keeps the first-party (无感登录) guide, so the super-admin
    // download endpoint keeps its semantics.
    const bool external = kind == "external";
    fileName = external ? "unisso-integration-external-SKILL.md"
                        : "unisso-integration-SKILL.md";
    const std::string skillDir =
        external ? "unisso-integration-external" : "unisso-integration";

    std::vector<std::string> candidates;
    const char* configured = std::getenv(external ? "UNISSO_INTEGRATION_SKILL_EXTERNAL"
                                                  : "UNISSO_INTEGRATION_SKILL");
    if (configured && configured[0]) candidates.emplace_back(configured);
    candidates.push_back("../.claude/skills/" + skillDir + "/SKILL.md");
    candidates.push_back(".claude/skills/" + skillDir + "/SKILL.md");
    candidates.push_back("/home/cloud/deploy/unisso/.claude/skills/" + skillDir + "/SKILL.md");
    candidates.push_back("/home/cloud/deploy/unisso-v2/.claude/skills/" + skillDir + "/SKILL.md");

    // Read on every request: editing the skill file takes effect immediately,
    // no rebuild or restart needed.
    for (const auto& path : candidates) {
        std::string text;
        if (!util::readFile(path, text) || text.empty()) continue;
        content = std::move(text);
        return true;
    }
    return false;
}

void publicDocsSkill(const Request& req, Response& res) {
    // Public endpoint: only the third-party (跳转授权) guide is published; the
    // internal 无感登录 guide stays behind the super-admin download.
    const std::string kind = util::trim(http::query(req, "kind"));
    if (!kind.empty() && kind != "external") {
        http::error(res, 400, "invalid_request", "公开文档只提供外部平台（跳转授权）接入说明");
        return;
    }
    std::string fileName, content;
    if (!loadIntegrationSkillMarkdown("external", fileName, content)) {
        applog::error("[public-docs] integration skill file not found");
        http::error(res, 404, "skill_not_found", "接入文档暂不可用，请稍后再试");
        return;
    }
    res.statusCode = 200;
    res.headers["Content-Type"] = "text/markdown; charset=utf-8";
    // Inline (no Content-Disposition): the docs page renders this body directly.
    res.headers["Cache-Control"] = "no-store";
    res.body = content;
}

void publicDocsMeta(const Request&, Response& res) {
    auto& cfg = AppConfig::get();

    std::map<std::string, JsonValue> endpoints{
        {"authorize", JsonValue(absolute("/authorize"))},
        {"token", JsonValue(absolute("/api/oauth/token"))},
        {"userinfo", JsonValue(absolute("/api/oauth/userinfo"))},
        {"introspect", JsonValue(absolute("/api/oauth/introspect"))},
        {"discovery", JsonValue(absolute("/.well-known/openid-configuration"))},
        {"jwks", JsonValue(absolute("/.well-known/jwks.json"))},
    };

    // The whitelist comes from the same helper the app registry validates
    // against, so the published contract cannot drift from what the server
    // actually accepts.
    std::vector<JsonValue> scopes;
    for (const auto& name : scopes::whitelist()) scopes.push_back(JsonValue(name));

    std::vector<JsonValue> docs;
    {
        std::map<std::string, JsonValue> doc;
        doc["kind"] = JsonValue(std::string("external"));
        doc["title"] = JsonValue(std::string("外部平台跳转授权接入"));
        doc["url"] = JsonValue(std::string("/api/public/docs/skill?kind=external"));
        docs.push_back(JsonValue(doc));
    }

    std::map<std::string, JsonValue> obj{
        {"issuer", JsonValue(cfg.issuer)},
        {"endpoints", JsonValue(endpoints)},
        {"scopes", JsonValue(scopes)},
        {"scope_catalog", scopes::catalog()},
        {"grants", JsonValue(std::vector<std::string>{"authorization_code", "refresh_token"})},
        {"code_challenge_methods", JsonValue(std::vector<std::string>{"S256"})},
        {"docs", JsonValue(docs)},
    };
    http::json(res, 200, JsonValue(obj));
}

}  // namespace handlers
