"""Browser request security, independent of OAuth resource authentication."""
import ipaddress
import secrets
from urllib.parse import urlsplit, unquote, parse_qs
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.responses import JSONResponse
from app.config import get_settings
from app.redis_client import redis_client, StateUnavailable


def trusted_peer(request):
    peer = request.client.host if request.client else ''
    try:
        return any(ipaddress.ip_address(peer) in ipaddress.ip_network(cidr)
                   for cidr in get_settings().trusted_proxy_cidrs)
    except ValueError:
        return False


def get_client_ip(request):
    peer = request.client.host if request.client else 'unknown'
    if not trusted_peer(request):
        return peer
    # Walk from the trusted edge toward the client, stopping at the first untrusted hop.
    hops = [p.strip() for p in request.headers.get('x-forwarded-for', '').split(',') if p.strip()]
    for hop in reversed(hops):
        try:
            address = ipaddress.ip_address(hop)
        except ValueError:
            return peer
        if not any(address in ipaddress.ip_network(c) for c in get_settings().trusted_proxy_cidrs):
            return str(address)
    return peer


def is_request_https(request):
    return request.url.scheme == 'https' or (
        trusted_peer(request) and request.headers.get('x-forwarded-proto', '').lower() == 'https')


def application_path(request):
    """Return the route path relative to the configured deployment prefix."""
    path = request.url.path
    prefix = get_settings().root_path
    if prefix and path == prefix:
        return '/'
    if prefix and path.startswith(prefix + '/'):
        return path[len(prefix):]
    return path


def safe_next_url(value, default='/'):
    value = value or default
    decoded = unquote(value)
    if (not value.startswith('/') or decoded.startswith('//') or '\\' in decoded
            or any(ord(c) < 32 for c in decoded) or urlsplit(decoded).netloc):
        return default
    return value


async def _context(request):
    session = request.cookies.get('unisso_session', '')
    if session and await redis_client.exists(f'session:{session}'):
        return 'session:' + session
    anonymous = request.cookies.get('unisso_csrf_context', '')
    if len(anonymous) == 43 and all(c.isalnum() or c in '-_' for c in anonymous):
        return 'anonymous:' + anonymous
    return None


async def issue_csrf_token(request, response):
    context = await _context(request)
    if context is None:
        anonymous = secrets.token_urlsafe(32)
        context = 'anonymous:' + anonymous
        response.set_cookie('unisso_csrf_context', anonymous, httponly=True,
            secure=is_request_https(request), samesite='lax', path='/', max_age=3600)
    key = 'browser_csrf:' + context
    token = await redis_client.get_or_create(key, secrets.token_urlsafe(32), expire=3600)
    response.headers['Cache-Control'] = 'no-store'
    return token


class CSRFMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request, call_next):
        path = application_path(request)
        if request.method in ('GET', 'HEAD', 'OPTIONS') or path in ('/api/oauth/token', '/api/oauth/introspect'):
            return await call_next(request)
        origin = request.headers.get('origin')
        s = get_settings()
        issuer = urlsplit(s.issuer or s.public_url or '')
        allowed = set(s.csrf_trusted_origins)
        allowed.add(f'{issuer.scheme}://{issuer.netloc}')
        if origin and origin not in allowed:
            return JSONResponse({'error': 'csrf_origin_rejected'}, status_code=403)
        if request.headers.get('sec-fetch-site') == 'cross-site':
            return JSONResponse({'error': 'csrf_origin_rejected'}, status_code=403)
        token = request.headers.get('x-csrf-token', '')
        if not token and request.headers.get('content-type', '').split(';')[0] == 'application/x-www-form-urlencoded':
            body = await request.body()
            if len(body) > 65536:
                return JSONResponse({'error': 'request_too_large'}, status_code=413)
            token = parse_qs(body.decode('utf-8', errors='replace')).get('csrf_token', [''])[0]
        try:
            context = await _context(request)
            expected = await redis_client.get('browser_csrf:' + context) if context else None
        except StateUnavailable:
            return JSONResponse({'error': 'authentication_state_unavailable'}, status_code=503)
        if not expected or not token or not secrets.compare_digest(expected, token):
            return JSONResponse({'error': 'csrf_validation_failed'}, status_code=403)
        return await call_next(request)
