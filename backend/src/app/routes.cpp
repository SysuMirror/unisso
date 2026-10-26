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
    app.get("/api/account/device-trust", handlers::deviceTrustStatus);
    app.post("/api/account/device-trust", handlers::deviceTrustSet);
    app.post("/api/auth/login", handlers::authLogin);
    app.post("/api/auth/email-code/start", handlers::emailCodeStart);
    app.post("/api/auth/email-code/verify", handlers::emailCodeVerify);
    app.post("/api/auth/password-reset/start", handlers::passwordResetStart);
    app.post("/api/auth/password-reset/confirm", handlers::passwordResetConfirm);
    app.post("/api/auth/step-up/methods", handlers::stepUpMethods);
    app.post("/api/auth/step-up/device/start", handlers::stepUpDeviceStart);
    app.post("/api/auth/step-up/device/status", handlers::stepUpDeviceStatus);
    app.get("/api/auth/step-up/device/pending", handlers::stepUpDevicePending);
    app.post("/api/auth/step-up/device/{id}/decide", handlers::stepUpDeviceDecide);
    app.post("/api/auth/step-up/email", handlers::stepUpSendEmail);
    app.post("/api/auth/step-up/verify", handlers::stepUpVerify);
    app.post("/api/auth/step-up/cancel", handlers::stepUpCancel);
    app.post("/api/auth/step-up/face/session", handlers::stepUpFaceSession);
    app.post("/api/auth/step-up/face/verify", handlers::stepUpFaceVerify);

    app.get("/api/account/faces/status", handlers::faceStatus);
    app.get("/api/account/faces", handlers::facesList);
    app.post("/api/account/faces/session", handlers::faceSession);
    app.post("/api/account/faces/probe", handlers::faceProbe);
    app.post("/api/account/faces/send-code", handlers::faceSendCode);
    app.post("/api/account/faces/confirm", handlers::faceConfirm);
    app.post("/api/account/faces/enroll", handlers::faceEnroll);
    app.del("/api/account/faces/{id}", handlers::faceUnbind);
    app.post("/api/auth/logout", handlers::authLogout);
    app.get("/api/auth/me", handlers::authMe);
    app.get("/api/auth/session", handlers::authSession);
    app.post("/api/auth/register", handlers::authRegister);
    app.post("/api/auth/verify-email", handlers::authVerify);
    app.post("/api/auth/resend-registration", handlers::authResend);
    app.get("/api/auth/sse/bind/start", handlers::sseBindStart);
    app.post("/api/logs/client", handlers::clientLog);
    app.get("/api/admin/logs", handlers::adminLogsList);
    app.get("/api/admin/logs/{file}", handlers::adminLogsTail);

    app.put("/api/me/profile", handlers::accountUpdateProfile);
    app.get("/api/account/links", handlers::accountLinksList);
    app.post("/api/account/links/send-code", handlers::accountLinkSendCode);
    app.post("/api/account/links", handlers::accountLinkBind);
    app.del("/api/account/links/{id}", handlers::accountLinkUnbind);
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
    app.post("/api/oauth/introspect", handlers::oauthIntrospect);
    app.get("/api/oauth/userinfo", handlers::oauthUserinfo);
    app.get("/.well-known/openid-configuration", handlers::oidcDiscovery);
    app.get("/.well-known/jwks.json", handlers::oidcJwks);

    // Applications

    app.get("/api/orgs", handlers::organizationsList);
    app.post("/api/orgs", handlers::organizationsCreate);
    app.get("/api/orgs/{id}/members", handlers::organizationsGet);
    app.post("/api/orgs/{id}/members", handlers::organizationsAddMember);
    app.put("/api/orgs/{id}/members/{userId}", handlers::organizationsSetMember);
    app.del("/api/orgs/{id}/members/{userId}", handlers::organizationsRemoveMember);

    app.get("/api/apps/integration-skill", handlers::appIntegrationSkill);
    app.get("/api/apps", handlers::appsList);
    app.post("/api/apps", handlers::appsCreate);
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
