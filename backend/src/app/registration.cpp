#include "core.h"

#include <curl/curl.h>

#include <atomic>
#include <mutex>
#include <random>
#include <sstream>

// ===========================================================================
// SMTP (libcurl): works with smtps://465 or smtp://587 + STARTTLS
// ===========================================================================

namespace {

struct UploadContext {
    const std::string* data;
    size_t offset;
};

size_t readCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<UploadContext*>(userdata);
    size_t capacity = size * nitems;
    size_t remaining = ctx->data->size() - ctx->offset;
    size_t amount = std::min(capacity, remaining);
    if (amount > 0) {
        std::memcpy(buffer, ctx->data->data() + ctx->offset, amount);
        ctx->offset += amount;
    }
    return amount;
}

size_t discardCallback(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

std::string wrapBase64(const std::string& raw) {
    std::string encoded = util::base64Encode(raw);
    std::string out;
    for (size_t i = 0; i < encoded.size(); i += 76) {
        out += encoded.substr(i, 76);
        out += "\r\n";
    }
    return out;
}

std::string encodeHeader(const std::string& value) {
    return "=?UTF-8?B?" + util::base64Encode(value) + "?=";
}

std::once_flag curlOnce;

}  // namespace

namespace mail {

bool send(const std::string& to, const std::string& subject,
          const std::string& html, std::string& error) {
    auto& cfg = AppConfig::get();
    if (!cfg.emailAvailable()) {
        error = "smtp_not_configured";
        return false;
    }
    std::call_once(curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    std::string message;
    message += "From: " + encodeHeader(cfg.smtpFromName) + " <" + cfg.smtpFrom + ">\r\n";
    message += "To: <" + to + ">\r\n";
    message += "Subject: " + encodeHeader(subject) + "\r\n";
    message += "MIME-Version: 1.0\r\n";
    message += "Content-Type: text/html; charset=UTF-8\r\n";
    message += "Content-Transfer-Encoding: base64\r\n";
    message += "\r\n";
    message += wrapBase64(html);

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_init_failed";
        return false;
    }
    struct curl_slist* recipients = nullptr;
    recipients = curl_slist_append(recipients, ("<" + to + ">").c_str());

    std::string url = std::string(cfg.smtpUseTls ? "smtps://" : "smtp://") +
                      cfg.smtpHost + ":" + std::to_string(cfg.smtpPort);
    UploadContext ctx{&message, 0};
    char curlError[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.smtpUser.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.smtpPassword.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, ("<" + cfg.smtpFrom + ">").c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE, static_cast<long>(message.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    if (!cfg.smtpUseTls && cfg.smtpStartTls) {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    }

    CURLcode code = curl_easy_perform(curl);
    long response = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK || (response >= 400 && response != 0)) {
        error = curlError[0] ? curlError : curl_easy_strerror(code);
        return false;
    }
    return true;
}

}  // namespace mail

// ===========================================================================
// Email registration: pending record + 6 digit code
// ===========================================================================

namespace registration {

namespace {

std::string digestEmail(const std::string& email) {
    return util::hex(util::sha256Raw(util::lower(util::trim(email))));
}

std::string pendingKey(const std::string& email) {
    return "registration_pending:" + digestEmail(email);
}

std::string codeHash(const std::string& code) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    return util::hex(util::hmacSha256Raw(key, code));
}

std::string randomCode() {
    static std::atomic<unsigned int> counter{0};
    std::random_device rd;
    unsigned int value = rd() + counter.fetch_add(1);
    return std::to_string(100000 + (value % 900000));
}

bool rateLimited(const std::string& email, const std::string& ip) {
    std::string digest = digestEmail(email);
    long long emailCount = Cache::incr("registration_email_hour:" + digest, 3600);
    long long ipCount = Cache::incr("registration_ip_hour:" + (ip.empty() ? "-" : ip), 3600);
    long long sends = Cache::incr("registration_sends:" + digest, 900);
    return emailCount > 3 || ipCount > 10 || sends > 3;
}

std::string recordJson(const std::string& email, const std::string& passwordHash,
                       const std::string& username, const std::string& fullName,
                       const std::string& code, int sends,
                       const std::string& inviteCode) {
    std::map<std::string, JsonValue> record;
    record["email"] = JsonValue(email);
    record["password_hash"] = JsonValue(passwordHash);
    record["username"] = JsonValue(username);
    record["full_name"] = JsonValue(fullName);
    record["code_hash"] = JsonValue(codeHash(code));
    record["sends"] = jsonx::num(sends);
    record["invite_code"] = JsonValue(inviteCode);
    record["expires_at"] = jsonx::num(util::nowEpoch() + 900);
    return jsonx::dump(record);
}

bool sendCode(const std::string& email, const std::string& code, std::string& errorCode) {
    std::string html =
        "<div style=\"font-family:-apple-system,Segoe UI,PingFang SC,sans-serif;color:#171717\">"
        "<h2 style=\"font-size:20px;margin:0 0 12px\">UniSSO 邮箱验证码</h2>"
        "<p style=\"color:#5f6368;font-size:14px\">你的验证码是：</p>"
        "<p style=\"font-size:30px;font-weight:700;letter-spacing:6px;margin:16px 0\">" + code + "</p>"
        "<p style=\"color:#5f6368;font-size:13px\">验证码 15 分钟内有效。如果不是你本人操作，请忽略此邮件。</p>"
        "</div>";
    std::string error;
    if (!mail::send(email, "UniSSO 邮箱验证码", html, error)) {
        std::cerr << "[mail] send failed: " << error << std::endl;
        errorCode = "email_unavailable";
        return false;
    }
    return true;
}

}  // namespace

bool start(const std::string& email, const std::string& plainPassword,
           const std::string& username, const std::string& fullName,
           const std::string& clientIp, const std::string& inviteCode,
           bool& sseCodeRequired, std::string& errorCode, std::string& errorMessage) {
    sseCodeRequired = false;
    errorMessage.clear();
    auto& cfg = AppConfig::get();
    if (!cfg.emailAvailable()) {
        errorCode = "email_unavailable";
        return false;
    }
    if (rateLimited(email, clientIp)) {
        errorCode = "rate_limited";
        return false;
    }
    if (!inviteCode.empty()) {
        std::string sseError;
        if (!sse::validateEmail(email, sseError)) {
            errorCode = "sse_email_failed";
            errorMessage = sseError;
            return false;
        }
        sseCodeRequired = true;
    }
    std::string code = randomCode();
    std::string passwordHash;
    try {
        passwordHash = password::hash(plainPassword);
    } catch (const std::exception& e) {
        std::cerr << "[registration] hash failed: " << e.what() << std::endl;
        errorCode = "email_unavailable";
        return false;
    }
    std::string record = recordJson(email, passwordHash, username, fullName, code, 1, inviteCode);
    Cache::set(pendingKey(email), record, 900);
    if (!sendCode(email, code, errorCode)) {
        Cache::del(pendingKey(email));
        return false;
    }
    return true;
}

bool resend(const std::string& email, const std::string& clientIp,
            std::string& errorCode) {
    std::string raw = Cache::get(pendingKey(email));
    if (raw.empty()) {
        errorCode = "invalid_email";
        return false;
    }
    if (rateLimited(email, clientIp)) {
        errorCode = "rate_limited";
        return false;
    }
    std::map<std::string, JsonValue> record;
    if (!jsonx::parseObject(raw, record)) {
        errorCode = "invalid_email";
        return false;
    }
    std::string code = randomCode();
    int sends = 1;
    auto sendsIt = record.find("sends");
    if (sendsIt != record.end() && sendsIt->second.getType() == JsonValue::NUMBER) {
        sends = static_cast<int>(sendsIt->second.asNumber()) + 1;
    }
    record["code_hash"] = JsonValue(codeHash(code));
    record["sends"] = jsonx::num(sends);
    record["expires_at"] = jsonx::num(util::nowEpoch() + 900);
    Cache::set(pendingKey(email), jsonx::dump(record), 900);
    return sendCode(email, code, errorCode);
}

bool verify(const std::string& email, const std::string& code,
            const std::string& sseCode, const std::string& password,
            std::optional<User>& created, std::string& errorCode,
            std::string& errorMessage) {
    errorMessage.clear();
    std::string raw = Cache::get(pendingKey(email));
    if (raw.empty()) {
        errorCode = "invalid_code";
        return false;
    }
    std::map<std::string, JsonValue> record;
    if (!jsonx::parseObject(raw, record)) {
        errorCode = "invalid_code";
        return false;
    }
    long long expiresAt = 0;
    auto expiresIt = record.find("expires_at");
    if (expiresIt != record.end() && expiresIt->second.getType() == JsonValue::NUMBER) {
        expiresAt = static_cast<long long>(expiresIt->second.asNumber());
    }
    if (expiresAt <= util::nowEpoch()) {
        Cache::del(pendingKey(email));
        errorCode = "invalid_code";
        return false;
    }

    std::string expected = jsonx::getString(record, "code_hash");
    if (expected.empty() || !util::constantTimeEquals(expected, codeHash(code))) {
        long long attempts = Cache::incr("registration_attempts:" + digestEmail(email), 900);
        if (attempts >= 5) Cache::del(pendingKey(email));
        errorCode = "invalid_code";
        return false;
    }

    std::string inviteCode = jsonx::getString(record, "invite_code");
    if (!inviteCode.empty()) {
        if (sseCode.empty() || password.empty()) {
            errorCode = "sse_code_required";
            errorMessage = "请输入集市验证码和密码";
            return false;
        }
        sse::Profile profile;
        std::string sseError;
        std::string fullName = jsonx::getString(record, "full_name");
        if (!sse::registerAccount(email, password, fullName, sseCode, inviteCode,
                                  profile, sseError)) {
            errorCode = "sse_register_failed";
            errorMessage = sseError;
            return false;
        }
        auto sseUser = sse::upsertUser(profile, password);
        if (!sseUser) {
            errorCode = "persistence_unavailable";
            return false;
        }
        Cache::del(pendingKey(email));
        created = sseUser;
        return true;
    }

    auto existing = repo::findUserByEmail(email);
    if (existing) {
        Cache::del(pendingKey(email));
        errorCode = "conflict";
        return false;
    }

    User user;
    user.id = util::uuid4();
    user.email = email;
    user.emailVerified = true;
    user.passwordHash = jsonx::getString(record, "password_hash");
    user.username = jsonx::getString(record, "username");
    user.fullName = jsonx::getString(record, "full_name");
    user.isActive = true;
    user.isAdmin = false;

    if (!user.username.empty()) {
        auto clash = Db::query("SELECT id FROM users WHERE username=" + Db::quote(user.username) + " LIMIT 1");
        if (!clash.empty()) user.username.clear();
    }
    if (!repo::createUser(user)) {
        errorCode = "persistence_unavailable";
        return false;
    }
    Cache::del(pendingKey(email));
    created = repo::findUserById(user.id);
    if (!created) created = user;
    return true;
}

}  // namespace registration
