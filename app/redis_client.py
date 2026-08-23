"""UniSSO Redis 客户端（带内存回退）"""
import json
from typing import Optional, Any, Dict

import redis.asyncio as redis

from app.config import get_settings

_settings = get_settings()

# 内存回退存储（仅用于开发和测试，生产环境使用 Redis）
_memory_store: Dict[str, Any] = {}


class RedisClient:
    """异步 Redis 客户端封装"""

    def __init__(self):
        self._client: Optional[redis.Redis] = None

    async def connect(self):
        """建立连接（失败则静默降级）"""
        kwargs = {
            "host": _settings.redis_host,
            "port": _settings.redis_port,
            "decode_responses": True,
        }
        if _settings.redis_password:
            kwargs["password"] = _settings.redis_password
        if _settings.redis_user:
            kwargs["username"] = _settings.redis_user

        try:
            self._client = redis.Redis(**kwargs)
            await self._client.ping()
        except Exception:
            self._client = None

    async def disconnect(self):
        """关闭连接"""
        if self._client:
            await self._client.close()
            self._client = None

    def _key(self, key: str) -> str:
        """添加前缀"""
        return f"{_settings.redis_prefix}:{key}"

    def _mem_key(self, key: str) -> str:
        return self._key(key)

    async def get(self, key: str) -> Optional[str]:
        if self._client is not None:
            try:
                return await self._client.get(self._key(key))
            except Exception:
                pass
        # 内存回退
        return _memory_store.get(self._mem_key(key))

    async def set(self, key: str, value: str, expire: Optional[int] = None):
        if self._client is not None:
            try:
                await self._client.set(self._key(key), value, ex=expire)
                return
            except Exception:
                pass
        # 内存回退
        _memory_store[self._mem_key(key)] = value

    async def delete(self, key: str):
        if self._client is not None:
            try:
                await self._client.delete(self._key(key))
                return
            except Exception:
                pass
        # 内存回退
        _memory_store.pop(self._mem_key(key), None)

    async def set_json(self, key: str, value: Any, expire: Optional[int] = None):
        await self.set(key, json.dumps(value), expire=expire)

    async def get_json(self, key: str) -> Optional[Any]:
        data = await self.get(key)
        if data:
            return json.loads(data)
        return None

    async def exists(self, key: str) -> bool:
        if self._client is not None:
            try:
                return await self._client.exists(self._key(key)) > 0
            except Exception:
                pass
        # 内存回退
        return self._mem_key(key) in _memory_store


# 全局实例
redis_client = RedisClient()
