"""UniSSO 安全中间件

提供：
1. SecurityHeadersMiddleware - HTTP 安全响应头
2. RateLimitMiddleware - 基于 IP 的请求限流
3. TrustedHostMiddleware - 生产环境 Host 头验证
"""
import time
import os
import ipaddress
from typing import Optional, Callable
from collections import defaultdict

from fastapi import Request, Response
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.types import ASGIApp

from app.config import get_settings

_settings = get_settings()


# ==================== 安全响应头中间件 ====================

class SecurityHeadersMiddleware(BaseHTTPMiddleware):
    """添加安全 HTTP 响应头"""

    async def dispatch(self, request: Request, call_next: Callable) -> Response:
        response = await call_next(request)

        # 防止 MIME 类型嗅探
        response.headers["X-Content-Type-Options"] = "nosniff"

        # 防止点击劫持
        response.headers["X-Frame-Options"] = "DENY"

        # XSS 保护（浏览器层面的额外保护）
        response.headers["X-XSS-Protection"] = "1; mode=block"

        #  referrer 策略
        response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"

        # 权限策略（限制浏览器 API）
        response.headers["Permissions-Policy"] = (
            "accelerometer=(), camera=(), geolocation=(), gyroscope=(), "
            "magnetometer=(), microphone=(), payment=(), usb=()"
        )

        # 内容安全策略 (CSP)
        # 只允许同源的脚本、样式、图片等
        csp = (
            "default-src 'self'; "
            "script-src 'self' 'unsafe-inline'; "  # 允许内联脚本（我们的简单 JS）
            "style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data: blob:; "
            "font-src 'self'; "
            "connect-src 'self'; "
            "frame-ancestors 'none'; "
            "base-uri 'self'; "
            "form-action 'self';"
        )
        response.headers["Content-Security-Policy"] = csp

        # HSTS（仅在 HTTPS 环境）
        if _settings.is_https:
            response.headers["Strict-Transport-Security"] = (
                "max-age=31536000; includeSubDomains; preload"
            )

        return response


# ==================== 限流中间件 ====================

class RateLimitMiddleware(BaseHTTPMiddleware):
    """基于 IP 的请求限流

    使用滑动窗口计数器算法，内存存储（生产环境建议用 Redis）。
    对敏感端点（登录、注册、授权）使用更严格的限制。
    """

    # 端点 -> (窗口秒数, 最大请求数)
    SENSITIVE_ENDPOINTS = {
        "/api/auth/login": (60, 10),      # 1分钟最多10次登录尝试
        "/api/auth/register": (60, 5),    # 1分钟最多5次注册
        "/authorize": (60, 20),           # 1分钟最多20次授权请求
        "/api/oauth/token": (60, 30),     # 1分钟最多30次token请求
        "/api/identities": (60, 10),      # 1分钟最多10次身份绑定
    }

    # 全局默认限制
    DEFAULT_WINDOW = 60
    DEFAULT_LIMIT = 100  # 1分钟100次

    def __init__(self, app: ASGIApp):
        super().__init__(app)
        # ip -> {endpoint -> [(timestamp, count)]}
        self._requests: dict = defaultdict(lambda: defaultdict(list))

    def _get_client_ip(self, request: Request) -> str:
        """获取真实客户端 IP"""
        # 优先从 X-Forwarded-For 获取（平台代理场景）
        forwarded = request.headers.get("x-forwarded-for")
        if forwarded:
            # 取第一个（最原始的客户端 IP）
            return forwarded.split(",")[0].strip()

        real_ip = request.headers.get("x-real-ip")
        if real_ip:
            return real_ip.strip()

        # 直接连接
        if request.client:
            return request.client.host

        return "unknown"

    def _is_internal_ip(self, ip: str) -> bool:
        """检查是否为内网 IP（豁免限流）"""
        try:
            addr = ipaddress.ip_address(ip)
            return addr.is_private or addr.is_loopback
        except ValueError:
            return False

    def _check_limit(self, ip: str, endpoint: str, window: int, limit: int) -> bool:
        """检查是否超过限流，返回 True 表示允许通过"""
        now = time.time()
        cutoff = now - window

        # 清理过期记录
        records = self._requests[ip][endpoint]
        records[:] = [r for r in records if r[0] > cutoff]

        # 统计当前窗口内的请求数
        total = sum(count for ts, count in records)
        if total >= limit:
            return False

        # 记录本次请求
        records.append((now, 1))
        return True

    async def dispatch(self, request: Request, call_next: Callable) -> Response:
        client_ip = self._get_client_ip(request)
        path = request.url.path

        # 内网 IP 豁免
        if self._is_internal_ip(client_ip):
            return await call_next(request)

        # 检查敏感端点
        for endpoint, (window, limit) in self.SENSITIVE_ENDPOINTS.items():
            if path == endpoint or path.startswith(endpoint + "/"):
                if not self._check_limit(client_ip, endpoint, window, limit):
                    from fastapi.responses import JSONResponse
                    return JSONResponse(
                        status_code=429,
                        content={
                            "error": "rate_limit_exceeded",
                            "detail": f"请求过于频繁，请 {window} 秒后再试",
                            "retry_after": window,
                        },
                        headers={"Retry-After": str(window)},
                    )

        # 全局默认限制
        if not self._check_limit(client_ip, "__global__", self.DEFAULT_WINDOW, self.DEFAULT_LIMIT):
            from fastapi.responses import JSONResponse
            return JSONResponse(
                status_code=429,
                content={
                    "error": "rate_limit_exceeded",
                    "detail": "请求过于频繁，请稍后再试",
                    "retry_after": self.DEFAULT_WINDOW,
                },
                headers={"Retry-After": str(self.DEFAULT_WINDOW)},
            )

        return await call_next(request)


# ==================== 可信 Host 中间件 ====================

class TrustedHostMiddleware(BaseHTTPMiddleware):
    """生产环境验证 Host 头，防止 Host 头攻击"""

    async def dispatch(self, request: Request, call_next: Callable) -> Response:
        if not _settings.debug:
            host = request.headers.get("host", "")
            # 简单检查：不允许空 Host，不允许非标准端口格式异常
            if not host or "/" in host or "\\" in host:
                from fastapi.responses import JSONResponse
                return JSONResponse(
                    status_code=400,
                    content={"error": "invalid_host"},
                )
        return await call_next(request)


# ==================== HTTPS 检测与重定向中间件 ====================

class HttpsDetectionMiddleware(BaseHTTPMiddleware):
    """检测请求是否通过 HTTPS 访问

    两种检测方式：
    1. 信任反向代理头（平台 Nginx/Traefik 场景）：
       - X-Forwarded-Proto: https
       - X-Forwarded-Ssl: on
    2. 直接 HTTPS 连接（应用自己监听 HTTPS）

    如果 FORCE_HTTPS=true 且检测到 HTTP，返回 308 重定向到 HTTPS。
    """

    async def dispatch(self, request: Request, call_next: Callable) -> Response:
        # 检测当前请求是否通过 HTTPS
        is_secure = self._is_https_request(request)

        # 将检测结果存入 request.state，供后续使用
        request.state.is_https = is_secure

        # 如果配置了强制 HTTPS，且当前不是 HTTPS，则重定向
        if _settings.force_https and not is_secure:
            # 避免重定向循环（只重定向 GET/HEAD 请求）
            if request.method in ("GET", "HEAD"):
                url = request.url.replace(scheme="https")
                # 如果端口是 80，改为 443
                if url.port == 80:
                    url = url.replace(port=443)
                from fastapi.responses import RedirectResponse
                return RedirectResponse(str(url), status_code=308)
            else:
                # POST/PUT/DELETE 等非安全方法返回 400
                from fastapi.responses import JSONResponse
                return JSONResponse(
                    status_code=400,
                    content={"error": "https_required", "detail": "此操作必须通过 HTTPS 访问"},
                )

        response = await call_next(request)

        # 如果检测到 HTTPS，自动添加 HSTS 头
        if is_secure:
            response.headers["Strict-Transport-Security"] = (
                "max-age=31536000; includeSubDomains; preload"
            )

        return response

    def _is_https_request(self, request: Request) -> bool:
        """判断当前请求是否通过 HTTPS"""
        # 1. 直接 HTTPS 连接
        if request.url.scheme == "https":
            return True

        # 2. 信任反向代理头
        if _settings.trust_proxy:
            forwarded_proto = request.headers.get("x-forwarded-proto", "")
            if forwarded_proto.lower() == "https":
                return True

            forwarded_ssl = request.headers.get("x-forwarded-ssl", "")
            if forwarded_ssl.lower() == "on":
                return True

            # Cloudflare 特定头
            cf_visitor = request.headers.get("cf-visitor", "")
            if "https" in cf_visitor.lower():
                return True

        return False


def is_request_https(request: Request) -> bool:
    """判断请求是否通过 HTTPS（供路由处理函数使用）"""
    # 优先使用中间件检测的结果
    if hasattr(request.state, "is_https"):
        return request.state.is_https

    # 回退：自行检测
    if request.url.scheme == "https":
        return True

    if _settings.trust_proxy:
        forwarded_proto = request.headers.get("x-forwarded-proto", "")
        if forwarded_proto.lower() == "https":
            return True

    return False
