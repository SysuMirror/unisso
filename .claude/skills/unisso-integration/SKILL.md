---
name: unisso-integration
description: Integrate an application with UniSSO (OAuth2/OIDC SSO) — register a client, implement Authorization Code + PKCE, read identity claims (sub/internal/roles), and handle the linked-account selector. UniSSO is identity-only; the application owns its own groups/roles/authorization.
---

# UniSSO Integration Skill

Use this runbook when you need to add UniSSO login to an application or verify
UniSSO identities/permissions in a backend service.

UniSSO is an **identity provider**, not an authorization server for your app.
It tells you who the user is (`sub`, profile, email, `internal`, `roles`); your
application decides what that user may do. Do not wait for SSO user-group APIs.
UniSSO has organizations, but they are only for **application ownership and
application-admin/developer roles inside UniSSO**; they are not returned in
tokens and are not your app's user groups.

## 1. What UniSSO provides

- Issuer: `https://sso.ssemarket.cn`
- Discovery: `https://sso.ssemarket.cn/.well-known/openid-configuration`
- JWKS: `https://sso.ssemarket.cn/.well-known/jwks.json`
- Authorize: `https://sso.ssemarket.cn/authorize`
- Token: `POST https://sso.ssemarket.cn/api/oauth/token`
- UserInfo: `GET https://sso.ssemarket.cn/api/oauth/userinfo`
- Introspect: `POST https://sso.ssemarket.cn/api/oauth/introspect`

> Always start from discovery and use the `issuer` it returns.
- Protocol: OAuth2 Authorization Code + **PKCE S256**; OIDC id_token (RS256).

## 2. Register your application

Sign in at `https://sso.ssemarket.cn`, join an organization on the **组织** page
(`https://sso.ssemarket.cn/admin/orgs` — marketplace/集市 accounts auto-join the
SSE org; others can apply and are approved by the org's admins or platform
admins), then register the app in **应用管理**
(`https://sso.ssemarket.cn/admin/apps`) or via the API. **Any organization
member** may register apps under their org; the app is owned by that
organization, and org admins/developers can also manage it. You can also ask an
admin to register it for you.

You need:

| Field | Notes |
|---|---|
| name | shown on the consent page |
| redirect URIs | **exact match**, `https://…` (or `http://localhost` / `http://127.0.0.1` for development); no `?`/`#` suffix |
| allowed scopes | subset of `openid profile:username profile:name profile:avatar profile:student_id email roles` |
| confidential | `true` ⇒ you get a `client_secret` (keep server-side) |
| icon URL | optional; shown on the consent page |

The `client_secret` is shown **once**. Store it in the server environment /
secret manager, never in frontend code or git. If an app will be managed by an
organization, register it under that organization so the org's admins/developers
can manage it.

## 3. Scopes → claims

| Scope | UserInfo / id_token claims |
|---|---|
| `openid` | `sub`, `sso_sub`, `internal` (always returned); `sse_sub` when the account has an SSE login |
| `profile:username` | `preferred_username` |
| `profile:name` | `name` |
| `profile:avatar` | `picture` |
| `profile:student_id` | `student_id` |
| `email` | `email`, `email_verified` |
| `roles` | `roles` |

`internal` is always returned: `true` = 院内 (SSE-verified), `false` = 院外.
`sub` is the stable identity key. Do **not** use email as the primary key and
do not auto-link accounts by email.

### Two OAuth ids (may differ)

- `sub` and `sso_sub` are the **UniSSO** user id and are identical. Use one of
  them as your application's local key.
- `sse_sub` is the **SSE Market** OAuth user id bound to that account. It is
  only present when the account has an SSE login, and it is **not** guaranteed
  to equal `sub`/`sso_sub`. Do not use `sse_sub` as a replacement for `sub`.
- Both ids are returned in the token response, userinfo and id_token. When the
  consent page selects a linked account, the ids describe that selected
  account, not the account that opened the page.

**There is no `groups`/`organizations` claim.** User groups, application roles
and resource permissions are application-owned; store them in your own database
and key them by `sub`.

## 4. Linked accounts (symmetric, many-to-many)

A UniSSO user may link **multiple** UniSSO accounts. There is **no master/sub
relationship**: links are peer-to-peer and symmetric. If A links B, B sees A
immediately; either side can unlink. A can also link C; B and C do not become
linked to each other unless separately linked.

Permissions are **not shared**. When a user with linked accounts authorizes
your app, the consent page shows an account selector. The user chooses which
account this **single authorization** is for; the token's `sub`, profile and
permissions belong to that account. `accounts[].is_current` marks the account
that opened the consent page; other entries are directly linked peers.

Rules for your app:

- Treat `sub`/`sso_sub` as the authoritative identity; there is no master account.
- One authorization = one account. Do not try to aggregate permissions from
  multiple linked accounts.
- Re-authorize if the user wants to use a different account.
- Never trust an email address alone to switch identities.

## 5. Authorization Code + PKCE (server-side app)

1. Generate `state` (random, one-time) and a PKCE verifier/challenge.
2. Redirect the browser to:

```
GET https://sso.ssemarket.cn/authorize
  ?response_type=code
  &client_id=YOUR_CLIENT_ID
  &redirect_uri=https://your-app.example.com/callback
  &scope=openid%20profile%20email
  &state=STATE
  &code_challenge=CHALLENGE
  &code_challenge_method=S256
```

3. On your callback, verify `state` and exchange the code **server-side**:

```
POST https://sso.ssemarket.cn/api/oauth/token
Content-Type: application/x-www-form-urlencoded

grant_type=authorization_code
&code=CODE
&redirect_uri=https://your-app.example.com/callback
&client_id=YOUR_CLIENT_ID
&client_secret=YOUR_SECRET
&code_verifier=VERIFIER
```

4. Read the identity:

```
GET https://sso.ssemarket.cn/api/oauth/userinfo
Authorization: Bearer ACCESS_TOKEN
```

5. Upsert your local user by `sub`; store the claims you need. Create **your own**
session. Do not store the UniSSO access token as your session.

## 6. Token behavior

- access token: RS256, ~60 minutes, includes `sub/client_id/scope/type/exp`.
- token response includes top-level `sso_sub` and (if bound) `sse_sub`.
- refresh token: ~7 days, rotated on refresh, one-time.
- id_token: RS256; verify via JWKS and `iss`/`aud`.
- Revocation: consent revocation invalidates tokens for that client.
- `introspect` requires client authentication (`client_id` + `client_secret`;
  public clients send only their `client_id`) and only returns tokens issued to
  that client. It is not an anonymous token oracle.

## 7. Security checklist

- [ ] Keep `client_secret` server-side only.
- [ ] Use PKCE S256 and a one-time `state`.
- [ ] Exchange code and call userinfo from the server, over HTTPS.
- [ ] Validate `iss`, `aud`, `exp`, signature (JWKS) and `sub`.
- [ ] Map users by `sub`/`sso_sub`, not email; keep `sse_sub` as a separate
      external id when integrating with SSE Market.
- [ ] Do not log tokens, secrets or passwords.
- [ ] Do not put tokens in URLs or localStorage if avoidable.
- [ ] On logout, clear your own session; UniSSO logout does not invalidate your session.
- [ ] Request only the scopes you need (never assume a `groups` scope exists).
- [ ] Keep application roles/groups in your own database, keyed by `sub`.

## 8. Error handling

| Symptom | Likely cause |
|---|---|
| `invalid_client` | wrong client_id/secret or app disabled |
| redirect mismatch | `redirect_uri` not exactly registered |
| `invalid_grant` | code reused/expired, wrong verifier, wrong redirect |
| userinfo 401 | token expired/revoked, app disabled, user disabled |
| consent shows an account selector | the user has linked accounts; the chosen account is the identity |
| `org_member_required` when registering an app | join an organization first (组织 page) |
| introspect `invalid_client` | missing/wrong client credentials, or token belongs to another client |

## 9. Developer checklist

1. Register the app; save `client_id`/`client_secret`.
2. Add the exact callback URL.
3. Pick the minimum scopes (`openid profile email` is usually enough).
4. Implement `/login` → authorize redirect and `/callback` → token + userinfo.
5. Upsert by `sub`; show the chosen account if needed.
6. Use `internal` for 院内/院外 if needed; implement your own roles/groups.
7. Test: valid login, denied consent, state mismatch, expired code, wrong secret,
   linked-account selector, logout.
8. Add the issuer/JWKS endpoints to your allowlist if you run in a restricted network.

## 10. Environment example

```ini
UNISSO_ISSUER=https://sso.ssemarket.cn
UNISSO_CLIENT_ID=app_xxxxxxxx
UNISSO_CLIENT_SECRET=********
UNISSO_REDIRECT_URI=https://your-app.example.com/callback
UNISSO_SCOPES=openid profile email
```
