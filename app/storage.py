"""UniSSO MinIO 存储客户端

符合 ssesinfra 平台契约：
- 凭证 label == 组名时注入 MINIO_ACCESS_KEY / MINIO_SECRET / MINIO_BUCKET
- 无凭证时优雅降级，返回提示而非崩溃
"""
import io
import uuid
from typing import Optional, BinaryIO

from minio import Minio
from minio.error import S3Error

from app.config import get_settings

_settings = get_settings()

# 全局 MinIO 客户端（懒加载）
_minio_client: Optional[Minio] = None


def get_minio_client() -> Optional[Minio]:
    """获取 MinIO 客户端（凭证存在时才创建）"""
    global _minio_client
    if _minio_client is not None:
        return _minio_client

    endpoint = _settings.minio_endpoint
    access_key = _settings.minio_access_key
    secret_key = _settings.minio_secret

    if not endpoint or not access_key or not secret_key:
        return None

    # 去掉 http:// 前缀
    endpoint_clean = endpoint.replace("http://", "").replace("https://", "")
    secure = endpoint.startswith("https://")

    try:
        _minio_client = Minio(
            endpoint_clean,
            access_key=access_key,
            secret_key=secret_key,
            secure=secure,
        )
        return _minio_client
    except Exception:
        return None


def get_bucket_name() -> Optional[str]:
    return _settings.minio_bucket


async def upload_file(
    data: bytes,
    filename: str,
    content_type: str = "application/octet-stream",
    prefix: str = "unisso",
) -> Optional[str]:
    """上传文件到 MinIO，返回 object_name"""
    client = get_minio_client()
    bucket = get_bucket_name()
    if not client or not bucket:
        return None

    object_name = f"{prefix}/{uuid.uuid4().hex}_{filename}"

    try:
        # 确保 bucket 存在
        if not client.bucket_exists(bucket):
            client.make_bucket(bucket)

        client.put_object(
            bucket_name=bucket,
            object_name=object_name,
            data=io.BytesIO(data),
            length=len(data),
            content_type=content_type,
        )
        return object_name
    except S3Error:
        return None
    except Exception:
        return None


async def get_file_url(object_name: str, expires: int = 3600) -> Optional[str]:
    """获取文件临时访问 URL"""
    client = get_minio_client()
    bucket = get_bucket_name()
    if not client or not bucket:
        return None

    try:
        url = client.presigned_get_object(bucket, object_name, expires=expires)
        return url
    except Exception:
        return None


async def delete_file(object_name: str) -> bool:
    """删除文件"""
    client = get_minio_client()
    bucket = get_bucket_name()
    if not client or not bucket:
        return False

    try:
        client.remove_object(bucket, object_name)
        return True
    except Exception:
        return False


async def list_files(prefix: str = "unisso/") -> list:
    """列出文件"""
    client = get_minio_client()
    bucket = get_bucket_name()
    if not client or not bucket:
        return []

    try:
        objects = client.list_objects(bucket, prefix=prefix, recursive=False)
        return [{"name": obj.object_name, "size": obj.size} for obj in objects]
    except Exception:
        return []
