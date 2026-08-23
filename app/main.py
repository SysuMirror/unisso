"""UniSSO 主应用入口

符合 ssesinfra 平台部署契约：
- 绑定 0.0.0.0:$PORT
- 长驻前台（supervisord 直接监督）
- 平台上报（state + heartbeat）
- 环境感知（根据负载调整 worker 数等）
- 探针端点：/health /env /minio /db /redis /shared /crash
"""
import os
import sys
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from fastapi.responses import JSONResponse, RedirectResponse
from fastapi.middleware.cors import CORSMiddleware

from app.config import get_settings
from app.database import init_db, async_session_maker
from app.redis_client import redis_client
from app.auth import init_default_data
from app.platform import (
    host_context, runtime_config, start_report_loop, report_state
)
from app.routes import router
from app.middleware import (
    SecurityHeadersMiddleware, RateLimitMiddleware, TrustedHostMiddleware,
    HttpsDetectionMiddleware,
)
from app.security import validate_secret_key

_settings = get_settings()

# 生产环境验证密钥强度
try:
    validate_secret_key()
except RuntimeError as e:
    import sys
    print(f"【安全错误】{e}", file=sys.stderr)
    # 非生产环境允许继续，生产环境应阻止启动
    if not _settings.debug:
        raise


# ==================== 平台上报 ====================
DEPLOY_ID = os.environ.get("DEPLOY_ID", "")
if DEPLOY_ID:
    start_report_loop(interval=30)


# ==================== 生命周期管理 ====================

@asynccontextmanager
async def lifespan(app: FastAPI):
    """应用生命周期"""
    print(f"【UniSSO】v{_settings.app_version} 启动中...")
    print(f"【UniSSO】端口: {_settings.port}, 工作目录: {os.getcwd()}")
    print(f"【UniSSO】数据库: {_settings.mysql_host}:{_settings.mysql_port}")
    print(f"【UniSSO】MinIO: {_settings.minio_endpoint or '未配置'}")
    print(f"【UniSSO】Redis: {_settings.redis_host}:{_settings.redis_port}")
    print(f"【UniSSO】宿主机: {host_context.host_name}, GPU: {host_context.gpu_models}")
    print(f"【UniSSO】运行时配置: workers={runtime_config.workers}, log_level={runtime_config.log_level}")

    # 连接 Redis
    try:
        await redis_client.connect()
        print("【UniSSO】Redis 连接成功")
    except Exception as e:
        print(f"【UniSSO】Redis 连接失败（降级到内存存储）: {e}")

    # 初始化数据库
    try:
        await init_db()
        print("【UniSSO】数据库表已初始化")
    except Exception as e:
        print(f"【UniSSO】数据库初始化警告: {e}")

    # 初始化默认数据
    try:
        async with async_session_maker() as session:
            await init_default_data(session)
            await session.commit()
        print("【UniSSO】默认数据已初始化")
    except Exception as e:
        print(f"【UniSSO】默认数据初始化警告: {e}")

    report_state("ready", f"UniSSO 启动完成, workers={runtime_config.workers}")
    yield

    # 关闭
    await redis_client.disconnect()
    print("【UniSSO】服务已关闭")


# ==================== FastAPI 应用 ====================

app = FastAPI(
    title=_settings.app_name,
    version=_settings.app_version,
    description="UniSSO - 中山大学统一身份认证平台",
    lifespan=lifespan,
    root_path=_settings.root_path,
)

# 安全中间件（顺序重要：越先添加的越外层）
app.add_middleware(SecurityHeadersMiddleware)
app.add_middleware(RateLimitMiddleware)
app.add_middleware(TrustedHostMiddleware)
app.add_middleware(HttpsDetectionMiddleware)

# CORS（生产环境不允许 *）
app.add_middleware(
    CORSMiddleware,
    allow_origins=_settings.cors_origins,
    allow_credentials=True,
    allow_methods=["GET", "POST", "PUT", "DELETE", "OPTIONS"],
    allow_headers=["Authorization", "Content-Type", "X-CSRF-Token"],
)

# 静态文件（支持子路径部署）
_static_path = f"{_settings.root_path}/static" if _settings.root_path else "/static"
app.mount(_static_path, StaticFiles(directory="static"), name="static")

# 模板
templates = Jinja2Templates(directory="templates")
templates.env.globals["root_path"] = _settings.root_path

# 路由
app.include_router(router)


# ==================== 全局异常处理 ====================

@app.exception_handler(HTTPException)
async def http_exception_handler(request: Request, exc: HTTPException):
    """统一处理 HTTP 异常"""
    if request.headers.get("accept", "").startswith("application/json"):
        return JSONResponse(
            status_code=exc.status_code,
            content={"error": exc.detail},
        )
    if exc.status_code == 401:
        rp = _settings.root_path.rstrip("/")
        login_path = f"{rp}/login" if rp else "/login"
        return RedirectResponse(f"{login_path}?next={request.url.path}", status_code=302)
    if exc.status_code == 403:
        return templates.TemplateResponse(request, "error.html", {
            "status_code": exc.status_code,
            "detail": exc.detail,
        })
    return JSONResponse(
        status_code=exc.status_code,
        content={"error": exc.detail},
    )


# ==================== 平台探针端点 ====================

@app.get("/env")
async def env_dump():
    """环境变量探针（兼容 base-deploy-repo 契约）"""
    keys = [
        "PORT", "SHARED_DIR",
        "MINIO_ENDPOINT", "MINIO_ACCESS_KEY", "MINIO_SECRET", "MINIO_BUCKET",
        "MYSQL_HOST", "MYSQL_PORT", "MYSQL_USER", "MYSQL_PASSWORD", "MYSQL_DB",
        "REDIS_HOST", "REDIS_PORT", "REDIS_USER", "REDIS_PASSWORD", "REDIS_PREFIX",
        "QDRANT_ENDPOINT", "QDRANT_GRPC", "QDRANT_API_KEY", "QDRANT_PREFIX",
        "DEPLOY_ID", "DEPLOY_NAME", "DEPLOY_BRANCH", "DEPLOY_REF",
        "GROUP_NAME", "PUBLIC_URL", "HOST_NAME", "GPU_IDS", "GPU_MODELS",
    ]
    return {k: ("<set>" if os.environ.get(k) else "<unset>") for k in keys}


@app.get("/shared")
async def shared_probe():
    """共享目录探针"""
    d = _settings.shared_dir
    try:
        entries = sorted(os.listdir(d))
        return {"ok": True, "shared_dir": d, "entries": entries}
    except OSError as e:
        return {"ok": False, "reason": f"读取 {d} 失败: {e}"}


@app.get("/minio")
async def minio_probe():
    """MinIO 连通性探针"""
    if not _settings.minio_access_key:
        return {"ok": False, "reason": "未注入 MINIO 凭证（在存储页建 label=组名 的访问密钥）"}
    try:
        from app.storage import get_minio_client, get_bucket_name
        client = get_minio_client()
        if not client:
            return {"ok": False, "reason": "MinIO 客户端初始化失败"}
        bucket = get_bucket_name()
        buckets = [b.name for b in client.list_buckets()]
        if bucket:
            exists = client.bucket_exists(bucket)
            return {"ok": True, "buckets": buckets, "target_bucket": bucket, "exists": exists}
        return {"ok": True, "buckets": buckets}
    except Exception as e:
        return {"ok": False, "reason": str(e)}


@app.get("/db")
async def db_probe():
    """数据库连通性探针"""
    if not _settings.mysql_user:
        return {"ok": False, "reason": "未注入 MYSQL 凭证（在数据库页建 label=组名 的凭证）"}
    try:
        import mysql.connector
        cn = mysql.connector.connect(
            host=_settings.mysql_host,
            port=int(_settings.mysql_port),
            user=_settings.mysql_user,
            password=_settings.mysql_password,
            database=_settings.mysql_db,
        )
        cur = cn.cursor()
        cur.execute("SELECT VERSION()")
        ver = cur.fetchone()[0]
        cur.execute("SHOW TABLES")
        tables = [r[0] for r in cur.fetchall()]
        cn.close()
        return {"ok": True, "version": ver, "tables": tables}
    except Exception as e:
        return {"ok": False, "reason": str(e)}


@app.get("/redis")
async def redis_probe():
    """Redis 连通性探针"""
    if not _settings.redis_user:
        return {"ok": False, "reason": "未注入 REDIS 凭证（在数据库页建 label=组名 的凭证）"}
    try:
        import redis as sync_redis
        r = sync_redis.Redis(
            host=_settings.redis_host,
            port=int(_settings.redis_port),
            username=_settings.redis_user,
            password=_settings.redis_password,
            decode_responses=True,
        )
        return {"ok": True, "ping": r.ping(), "prefix": _settings.redis_prefix}
    except Exception as e:
        return {"ok": False, "reason": str(e)}


@app.get("/crash")
async def crash():
    """硬杀进程，验证 supervisord autorestart"""
    import threading
    def _die():
        report_state("degraded", "崩溃测试，进程退出")
        os._exit(1)
    threading.Thread(target=_die, daemon=True).start()
    return {"ok": False, "reason": "进程已请求退出，supervisord 应在几秒内重启"}


@app.get("/platform")
async def platform_info():
    """平台信息端点（宿主机上下文 + 运行时配置）"""
    return {
        "ok": True,
        "host": {
            "group_name": host_context.group_name,
            "public_url": host_context.public_url,
            "host_name": host_context.host_name,
            "gpu_ids": host_context.gpu_ids,
            "gpu_models": host_context.gpu_models,
            "deploy_id": host_context.deploy_id,
            "deploy_name": host_context.deploy_name,
        },
        "runtime": {
            "workers": runtime_config.workers,
            "log_level": runtime_config.log_level,
            "max_requests_per_worker": runtime_config.max_requests_per_worker,
            "enable_metrics": runtime_config.enable_metrics,
        },
        "app": {
            "name": _settings.app_name,
            "version": _settings.app_version,
            "port": _settings.port,
        },
    }
