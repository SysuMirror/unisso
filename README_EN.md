# UniSSO - Sun Yat-sen University Unified Identity Platform

An OAuth2/OIDC-based unified identity authentication platform. The primary identity is the **Sun Yat-sen University email** (`*.mail*.sysu.edu.cn`), with support for binding external identities such as marketplace accounts, GitHub, and WeChat. Designed for the ssesinfra student group development platform, providing single sign-on (SSO) and third-party application authorization.

> [中文 README](README.md)

## Core Design

```
┌─────────────────────────────────────────────────────────────┐
│                    UniSSO Identity Center                    │
│  ┌─────────────┐    ┌─────────────────────────────────────┐ │
│  │ Primary     │    │ External Identity Binding           │ │
│  │ *.mail*.    │───▶│  • ssemarket (marketplace account)  │ │
│  │ sysu.edu.cn │    │  • github                           │ │
│  │ (email+pwd) │    │  • wechat                           │ │
│  └─────────────┘    │  • custom:xxx (other apps)          │ │
│                     └─────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
        ┌─────────┐    ┌─────────┐    ┌─────────┐
        │ App A   │    │ App B   │    │ App C   │
        └─────────┘    └─────────┘    └─────────┘
```

### Identity Hierarchy

| Level | Description | Example |
|-------|-------------|---------|
| **Primary** | SYSU email, unique identifier | `xxx@mail2.sysu.edu.cn` |
| **Display** | Nickname / full name, editable | `student` / `Zhang San` |
| **External** | Bound third-party accounts | Marketplace `market_user` |

## Features

- **SYSU Email Login**: Registration and login require `*.mail*.sysu.edu.cn` email
- **External Identity Binding**: Bind marketplace, GitHub, WeChat accounts
- **OAuth2 Authorization Server**: Standard Authorization Code Flow + PKCE
- **OIDC Support**: UserInfo endpoint and OpenID Discovery
- **App Registration**: Developers can register apps, get client_id/client_secret
- **User Consent Management**: Users can view, modify, and revoke app authorizations
- **RBAC Permission Model**: Role-based access control
- **One-Click Deployment**: Fully compliant with the ssesinfra platform deployment contract

## Architecture

| Component | File | Responsibility |
|-----------|------|----------------|
| `app/auth.py` | Auth module | Email login, session management, identity binding, permission checks |
| `app/oauth2_server.py` | OAuth2 server | Authorization codes, token issuance/validation, consent management |
| `app/routes.py` | Routes | Pages + API endpoints |
| `app/models.py` | Data models | User, UserIdentity, Application, etc. |
| `app/middleware.py` | Security middleware | Security headers, rate limiting, host validation, HTTPS detection |
| `app/audit.py` | Audit logging | Security event recording with data masking |
| `app/security.py` | Security utilities | Password policy, JWT, CSRF, PKCE |
| `app/platform.py` | Platform awareness | Host context, load sensing, dynamic worker adjustment |
| `app/storage.py` | Storage module | MinIO file upload/download |

### Data Models

- **User**: User table, primary identity is `email` (SYSU email)
- **UserIdentity**: External identity binding table
- **Application**: OAuth2 client application registration
- **UserConsent**: User authorization records
- **Role / Permission**: RBAC role-permission system

## Deployment

### One-Click Deploy on ssesinfra Platform

1. Fill in the "Deploy" card on the platform:

| Field | Value |
|-------|-------|
| Repository | `https://github.com/YatTerra/unisso.git` |
| Branch | `main` |
| Subdirectory | (empty) |
| Type | Persistent service (auto-restart on crash) |

2. On the "Database" page, create a MySQL credential with label = your group name (**required**)
3. Add environment variables on the "Deploy" page (**required**):

```
UNISSO_SECRET_KEY=your-strong-random-key (openssl rand -base64 48)
UNISSO_ADMIN_PASSWORD=strong-admin-password
```

4. Click "Deploy"

### Environment Variables

Automatically injected by the platform:

| Variable | Description |
|----------|-------------|
| `PORT` | Service port (default 8080) |
| `MYSQL_HOST/PORT/USER/PASSWORD/DB` | MySQL connection info |
| `REDIS_HOST/PORT/USER/PASSWORD/PREFIX` | Redis connection info (optional) |
| `DEPLOY_ID/REPORT_URL/REPORT_TOKEN` | Platform reporting info |

Optional configuration:

| Variable | Description |
|----------|-------------|
| `UNISSO_SECRET_KEY` | JWT signing key (**required in production, >=32 chars**) |
| `UNISSO_ADMIN_PASSWORD` | Initial admin password (**must change in production**) |
| `TRUST_PROXY` | `true` to trust reverse proxy HTTPS headers |
| `FORCE_HTTPS` | `true` to auto-redirect HTTP to HTTPS (308) |
| `HTTPS_CERT/HTTPS_KEY` | Self-signed certificate paths (for app-level HTTPS) |

### HTTPS Configuration (Automatic)

#### Scenario A: Platform has HTTPS reverse proxy (recommended)

Add environment variables on the "Deploy" page:

```
TRUST_PROXY=true
FORCE_HTTPS=true
UNISSO_SECRET_KEY=your-strong-random-key
```

The app will automatically:
- Recognize the `X-Forwarded-Proto: https` header
- Set `Secure` cookies
- Redirect HTTP requests to HTTPS (308)
- Add HSTS response headers

#### Scenario B: App listens HTTPS directly

Add environment variables:

```
HTTPS_CERT=/app/certs/cert.pem
HTTPS_KEY=/app/certs/key.pem
UNISSO_SECRET_KEY=your-strong-random-key
```

`deploy.sh` will automatically generate a self-signed certificate and start HTTPS.

### First Deployment

- Automatic database migration (`alembic upgrade head`)
- Automatic creation of default roles and permissions
- Automatic creation of admin account (email set via `UNISSO_ADMIN_EMAIL`, default `admin@mail.sysu.edu.cn`)

## OAuth2 Integration Guide

### 1. Register an Application

Log in to UniSSO → App Management → Register App, fill in:
- App name
- Callback URL (e.g., `http://localhost:3000/callback`)
- Required scopes (e.g., `openid profile email`)

Get `client_id` and `client_secret`.

### 2. Authorization Flow (Authorization Code + PKCE)

**Step 1: Generate PKCE**
```python
import secrets, hashlib, base64

verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip('=')
challenge = base64.urlsafe_b64encode(
    hashlib.sha256(verifier.encode()).digest()
).decode().rstrip('=')
```

**Step 2: Redirect user to authorization page**
```
GET /authorize?response_type=code
    &client_id=YOUR_CLIENT_ID
    &redirect_uri=http://localhost:3000/callback
    &scope=openid profile
    &state=random_state
    &code_challenge=CHALLENGE
    &code_challenge_method=S256
```

**Step 3: Exchange code for token**
```bash
curl -X POST /api/oauth/token \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "grant_type=authorization_code" \
  -d "code=CODE_FROM_CALLBACK" \
  -d "redirect_uri=http://localhost:3000/callback" \
  -d "client_id=YOUR_CLIENT_ID" \
  -d "client_secret=YOUR_SECRET" \
  -d "code_verifier=VERIFIER"
```

**Step 4: Get user info**
```bash
curl /api/oauth/userinfo \
  -H "Authorization: Bearer ACCESS_TOKEN"
```

Response example:
```json
{
  "sub": "user-id",
  "preferred_username": "student",
  "email": "student@mail2.sysu.edu.cn",
  "name": "Student Name",
  "roles": ["user"]
}
```

### 3. OIDC Discovery

```bash
GET /.well-known/openid-configuration
```

## API Endpoints

### Authentication
- `POST /api/auth/login` - Login (email + password)
- `POST /api/auth/logout` - Logout
- `POST /api/auth/register` - Register (email must be sysu format)
- `GET /api/auth/me` - Current user info

### External Identity Binding
- `GET /api/identities` - List bound external identities
- `POST /api/identities` - Bind external identity
- `DELETE /api/identities/{id}` - Unbind external identity

### OAuth2
- `GET /authorize` - Authorization page
- `POST /api/oauth/authorize` - Confirm authorization
- `POST /api/oauth/token` - Get token
- `POST /api/oauth/introspect` - Token introspection
- `GET /api/oauth/userinfo` - User info (OIDC)
- `GET /.well-known/openid-configuration` - OIDC discovery

### App Management
- `GET /api/apps` - List apps
- `POST /api/apps` - Register app
- `GET/PUT/DELETE /api/apps/{id}` - App CRUD
- `POST /api/apps/{id}/reset-secret` - Reset client secret

### User Consent Management
- `GET /api/consents` - My authorizations
- `PUT /api/consents/{id}` - Modify authorization scope
- `DELETE /api/consents/{id}` - Revoke authorization

### Admin
- `GET /api/admin/users` - User list
- `POST /api/admin/users` - Create user
- `PUT/DELETE /api/admin/users/{id}` - User management

### Platform Probes
- `GET /health` - Health check
- `GET /env` - Environment variable status
- `GET /db` - Database connectivity
- `GET /redis` - Redis connectivity
- `GET /minio` - MinIO connectivity
- `GET /platform` - Platform info

## Local Development

```bash
# 1. Create virtual environment
python3 -m venv .venv
source .venv/bin/activate

# 2. Install dependencies
pip install -r requirements.txt

# 3. Run migrations
alembic upgrade head

# 4. Start service (development mode)
UNISSO_SECRET_KEY="$(openssl rand -base64 48)" \
  uvicorn app.main:app --reload --port 8080

# Or use production launcher (with environment awareness)
UNISSO_SECRET_KEY="$(openssl rand -base64 48)" \
  python start.py

# 5. Local HTTPS (optional)
./setup-https.sh
UNISSO_SECRET_KEY="$(openssl rand -base64 48)" \
HTTPS_CERT=./certs/cert.pem \
HTTPS_KEY=./certs/key.pem \
python start.py
```

## Security Hardening (Built-in)

### 1. Password Security
- **bcrypt hashing** (72-byte limit, unified truncation)
- **Password complexity policy**: Min 8 chars, must include uppercase, lowercase, digit, special char
- **Weak password detection**: Blocks common passwords and sequential characters

### 2. Login Protection
- **Rate limiting**: 5 failures in 5 minutes locks IP for 15 minutes
- **Audit logging**: All login/register/authorization/admin actions logged to `logs/audit.log`
- **Data masking**: Passwords, tokens, and other sensitive fields are automatically masked in logs

### 3. HTTP Security Headers
- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: DENY`
- `X-XSS-Protection: 1; mode=block`
- `Content-Security-Policy`
- `Referrer-Policy: strict-origin-when-cross-origin`
- `Permissions-Policy`
- `Strict-Transport-Security` (auto-enabled in HTTPS)

### 4. Cookie Security
- `HttpOnly` + `Secure` (when HTTPS) + `SameSite=Lax` + `Path=/`

### 5. OAuth2 Security
- **PKCE required**: All authorization code flows must include code_challenge
- **Strict redirect_uri matching**: Prevents open redirect vulnerabilities
- **One-time authorization codes**: Codes are invalidated immediately after use

### 6. JWT Security
- **Fixed algorithm HS256**: Prevents algorithm confusion attacks
- **aud/iss/jti claims**: Includes audience, issuer, and unique identifier
- **Key length validation**: Production requires >= 32 character non-default key

### 7. Rate Limiting
- Login endpoint: 10 per minute
- Register endpoint: 5 per minute
- Authorization endpoint: 20 per minute
- Token endpoint: 30 per minute

### 8. File Upload Security
- **MIME type whitelist**: Only JPEG/PNG/GIF/WebP/SVG allowed
- **Magic number check**: Verifies file headers to prevent extension spoofing
- **Size limits**: Avatar 5MB, icon 2MB

### 9. Admin Security
- **Default password detection**: Security warning printed on startup if default password is used
- **Operation audit**: All admin actions are logged to audit log

## Security Notes

1. **Set `UNISSO_SECRET_KEY` in production** (at least 32 random characters)
2. **Change the default admin password** (via `UNISSO_ADMIN_PASSWORD` environment variable)
3. **Enable HTTPS**: Set `TRUST_PROXY=true FORCE_HTTPS=true` when behind a reverse proxy
4. **Rotate keys regularly**: Reset application client_secret periodically
5. **Check audit logs**: `tail -f logs/audit.log`

## Tech Stack

- **FastAPI** - Web framework
- **SQLAlchemy 2.0 + Alembic** - ORM and database migrations
- **Redis** - Session, cache (optional, falls back to in-memory storage)
- **PyJWT** - JWT tokens
- **Passlib + bcrypt** - Password hashing
- **Jinja2** - Server-side templates
- **MinIO** - Object storage (avatars/icons)

## License

MIT License - see [LICENSE](LICENSE) file.
