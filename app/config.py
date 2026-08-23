"""UniSSO 配置管理

符合 ssesinfra 平台契约，从环境变量读取所有配置。
"""
import os
from functools import lru_cache
from typing import Optional, List

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    """应用配置，优先从环境变量读取（符合 ssesinfra 平台注入规范）"""

    # 基础
    app_name: str = "UniSSO"
    app_version: str = "1.1.0"
    debug: bool = False
    # 生产环境必须通过 UNISSO_SECRET_KEY 环境变量设置，长度 >= 32
    secret_key: str = os.environ.get("UNISSO_SECRET_KEY", "")

    # HTTPS / 反向代理
    # 信任反向代理的 X-Forwarded-* 头（平台 Nginx/Traefik 场景）
    trust_proxy: bool = os.environ.get("TRUST_PROXY", "false").lower() == "true"
    # 强制 HTTPS（检测到 HTTP 时自动 308 重定向）
    force_https: bool = os.environ.get("FORCE_HTTPS", "false").lower() == "true"
    # 自签名证书路径（应用直接监听 HTTPS 时使用）
    https_cert: str = os.environ.get("HTTPS_CERT", "")
    https_key: str = os.environ.get("HTTPS_KEY", "")
    # 是否直接启用 HTTPS（有证书时自动 true）
    is_https: bool = os.environ.get("IS_HTTPS", "false").lower() == "true"

    # 端口（平台注入 PORT，默认 8080）
    port: int = int(os.environ.get("PORT", "8080"))

    # 数据库（平台注入 MySQL 凭证，label=组名）
    database_url: str = ""
    mysql_host: str = os.environ.get("MYSQL_HOST", "localhost")
    mysql_port: int = int(os.environ.get("MYSQL_PORT", "3306"))
    mysql_user: str = os.environ.get("MYSQL_USER", "")
    mysql_password: str = os.environ.get("MYSQL_PASSWORD", "")
    mysql_db: str = os.environ.get("MYSQL_DB", "unisso")

    # Redis（平台注入 Redis 凭证，label=组名）
    redis_host: str = os.environ.get("REDIS_HOST", "localhost")
    redis_port: int = int(os.environ.get("REDIS_PORT", "6379"))
    redis_user: Optional[str] = os.environ.get("REDIS_USER")
    redis_password: Optional[str] = os.environ.get("REDIS_PASSWORD")
    redis_prefix: str = os.environ.get("REDIS_PREFIX", "unisso")

    # MinIO（平台注入 MinIO 凭证，label=组名）
    minio_endpoint: str = os.environ.get("MINIO_ENDPOINT", "")
    minio_access_key: str = os.environ.get("MINIO_ACCESS_KEY", "")
    minio_secret: str = os.environ.get("MINIO_SECRET", "")
    minio_bucket: str = os.environ.get("MINIO_BUCKET", "")

    # Qdrant（平台注入，可选）
    qdrant_endpoint: str = os.environ.get("QDRANT_ENDPOINT", "")
    qdrant_grpc: str = os.environ.get("QDRANT_GRPC", "")
    qdrant_api_key: str = os.environ.get("QDRANT_API_KEY", "")
    qdrant_prefix: str = os.environ.get("QDRANT_PREFIX", "")

    # JWT
    jwt_algorithm: str = "HS256"
    jwt_access_token_expire_minutes: int = 30
    jwt_refresh_token_expire_days: int = 7
    jwt_id_token_expire_minutes: int = 60

    # OAuth2
    authorization_code_expire_seconds: int = 600
    pkce_required: bool = True

    # Session
    session_expire_hours: int = 24

    # 共享目录（平台注入）
    shared_dir: str = os.environ.get("SHARED_DIR", "/shared")

    # 平台上报（ssesinfra 注入）
    report_url: Optional[str] = os.environ.get("REPORT_URL")
    report_token: Optional[str] = os.environ.get("REPORT_TOKEN")
    deploy_id: Optional[str] = os.environ.get("DEPLOY_ID")
    deploy_name: Optional[str] = os.environ.get("DEPLOY_NAME")
    deploy_branch: Optional[str] = os.environ.get("DEPLOY_BRANCH")
    deploy_ref: Optional[str] = os.environ.get("DEPLOY_REF")
    group_name: Optional[str] = os.environ.get("GROUP_NAME")
    public_url: Optional[str] = os.environ.get("PUBLIC_URL")
    host_name: Optional[str] = os.environ.get("HOST_NAME")
    gpu_ids: Optional[str] = os.environ.get("GPU_IDS")
    gpu_models: Optional[str] = os.environ.get("GPU_MODELS")

    # 初始化管理员（首次启动时创建，必须通过环境变量设置）
    admin_username: str = os.environ.get("UNISSO_ADMIN_USERNAME", "admin")
    admin_password: str = os.environ.get("UNISSO_ADMIN_PASSWORD", "")
    admin_email: str = os.environ.get("UNISSO_ADMIN_EMAIL", "")

    # CORS
    cors_origins: List[str] = ["*"]

    # Gunicorn / Uvicorn 运行时配置
    workers: int = int(os.environ.get("WORKERS", "2"))
    worker_class: str = os.environ.get("WORKER_CLASS", "uvicorn.workers.UvicornWorker")
    log_level: str = os.environ.get("LOG_LEVEL", "info")
    max_requests: int = int(os.environ.get("MAX_REQUESTS", "0"))
    timeout: int = int(os.environ.get("TIMEOUT", "120"))

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        # 动态构建数据库 URL（如果未直接设置）
        if not self.database_url:
            has_mysql_env = bool(os.environ.get("MYSQL_HOST"))
            has_mysql_creds = bool(os.environ.get("MYSQL_USER")) and bool(os.environ.get("MYSQL_PASSWORD"))
            if has_mysql_env and has_mysql_creds:
                self.database_url = (
                    f"mysql+aiomysql://{self.mysql_user}:{self.mysql_password}"
                    f"@{self.mysql_host}:{self.mysql_port}/{self.mysql_db}"
                )
            else:
                self.database_url = "sqlite+aiosqlite:///./unisso.db"

    class Config:
        env_prefix = "UNISSO_"
        case_sensitive = False


@lru_cache()
def get_settings() -> Settings:
    return Settings()
