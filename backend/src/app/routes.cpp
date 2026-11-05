#include "core.h"

namespace handlers {

void registerAll(Server& app) {
    // Health
    app.get("/api/health", handlers::health);
    app.get("/health", handlers::health);
    app.get("/api/csrf-token", handlers::csrfToken);

    // Browser auth
    app.get("/api/account/sessions", handlers::sessionsList);
    app.del("/api/account/sessions/{id}", handlers::sessionRevoke);
    app.post("/api/account/sessions/revoke-others", handlers::sessionRevokeOthers);
    app.post("/api/auth/login", handlers::authLogin);
    app.post("/api/auth/password-reset/start", handlers::passwordResetStart);
    app.post("/api/auth/password-reset/confirm", handlers::passwordResetConfirm);
    app.post("/api/auth/logout", handlers::authLogout);
    app.get("/api/auth/me", handlers::authMe);
    app.get("/api/auth/session", handlers::authSession);
    app.post("/api/auth/register", handlers::authRegister);
    app.post("/api/auth/verify-email", handlers::authVerify);
    app.post("/api/auth/resend-registration", handlers::authResend);
    app.post("/api/logs/client", handlers::clientLog);
    app.get("/api/admin/logs", handlers::adminLogsList);
    app.get("/api/admin/logs/{file}", handlers::adminLogsTail);

    app.put("/api/me/profile", handlers::accountUpdateProfile);
    app.get("/api/auth/sse/qr/status", handlers::sseQrStatus);
    app.get("/api/auth/sse/qr/start", handlers::sseQrStart);
    app.get("/api/auth/sse/qr/poll", handlers::sseQrPoll);
    app.get("/api/auth/sse/oauth/callback", handlers::sseOauthCallback);

    // Scan login confirmed on an already-logged-in UniSSO mobile session.
    app.get("/api/auth/qr/start", handlers::qrLoginStart);
    app.get("/api/auth/qr/poll", handlers::qrLoginPoll);
    app.get("/api/auth/qr/scan", handlers::qrLoginScan);
    app.get("/api/auth/qr/detail", handlers::qrLoginDetail);
    app.post("/api/auth/qr/confirm", handlers::qrLoginConfirm);
    app.post("/api/auth/qr/deny", handlers::qrLoginDeny);

    // OAuth2 / OIDC
    app.get("/api/oauth/authorize/context", handlers::oauthContext);
    app.post("/api/oauth/authorize", handlers::oauthAuthorize);
    app.post("/api/oauth/token", handlers::oauthToken);
    app.post("/api/oauth/password/verify", handlers::oauthPasswordVerify);
    app.post("/api/oauth/session/verify", handlers::oauthSessionVerify);
    app.post("/api/oauth/session/revoke", handlers::oauthSessionRevoke);
    app.post("/api/oauth/introspect", handlers::oauthIntrospect);
    app.get("/api/oauth/userinfo", handlers::oauthUserinfo);
    app.get("/.well-known/openid-configuration", handlers::oidcDiscovery);
    app.get("/.well-known/jwks.json", handlers::oidcJwks);

    // Applications: first-party (admin-managed) and third-party (apply/review).
    app.get("/api/apps/integration-skill", handlers::appIntegrationSkill);
    app.get("/api/apps", handlers::appsList);
    app.post("/api/apps", handlers::appsCreate);
    app.post("/api/apps/apply", handlers::appsApply);
    app.get("/api/apps/pending", handlers::appsPending);
    app.post("/api/apps/{id}/review", handlers::appsReview);
    app.get("/api/apps/{id}", handlers::appsGet);
    app.put("/api/apps/{id}", handlers::appsUpdate);
    app.del("/api/apps/{id}", handlers::appsDelete);
    app.post("/api/apps/{id}/reset-secret", handlers::appsResetSecret);
    app.post("/api/apps/{id}/icon", handlers::appsIcon);

    // Consents
    app.get("/api/consents", handlers::consentsList);
    app.put("/api/consents/{id}", handlers::consentsUpdate);
    app.del("/api/consents/{id}", handlers::consentsRevoke);
    app.post("/api/consents/{id}", handlers::consentsRevoke);

    // External identities
    app.get("/api/identities", handlers::identitiesList);
    // Legacy POST /api/identities is intentionally disabled: identities are
    // attached only through a verified marketplace OAuth round-trip.
    app.del("/api/identities/{id}", handlers::identitiesUnbind);

    // Admin
    app.get("/api/admin/users", handlers::adminUsersList);
    app.post("/api/admin/users", handlers::adminUsersCreate);
    app.put("/api/admin/users/{id}", handlers::adminUsersUpdate);
    app.del("/api/admin/users/{id}", handlers::adminUsersDelete);

    // Avatar
    app.get("/api/me/avatar", handlers::avatarGet);
    app.post("/api/me/avatar", handlers::avatarUpload);
}

}  // namespace handlers
