"""Shared security state. Production errors never fall back to local memory."""
import json
import time
import uuid
from typing import Optional, Any
import redis.asyncio as redis
from app.config import get_settings

_settings = get_settings()

INCREMENT_SCRIPT = """local value = redis.call('INCR', KEYS[1])
if value == 1 then
    redis.call('EXPIRE', KEYS[1], ARGV[1])
end
return value"""

class StateUnavailable(RuntimeError):
    pass

class RedisClient:
    def __init__(self):
        self._client = None
        self._memory = {}

    def _memory_enabled(self):
        return _settings.debug and _settings.redis_memory_mode

    async def connect(self):
        if self._memory_enabled():
            return
        self._client = redis.Redis(host=_settings.redis_host, port=_settings.redis_port,
            username=_settings.redis_user or None, password=_settings.redis_password or None,
            decode_responses=True, socket_connect_timeout=2, socket_timeout=2)
        try:
            await self._client.ping()
            await self.validate_getdel()
        except StateUnavailable:
            await self.disconnect()
            raise
        except Exception as exc:
            await self.disconnect()
            raise StateUnavailable('Shared authentication state unavailable') from exc

    async def disconnect(self):
        if self._client is not None:
            await self._client.aclose()
            self._client = None

    def _key(self, key):
        return f'{_settings.redis_prefix}:{key}'

    def _mem_get(self, key, pop=False):
        item = self._memory.pop(key, None) if pop else self._memory.get(key)
        if item is None:
            return None
        value, deadline = item
        if deadline is not None and time.monotonic() >= deadline:
            self._memory.pop(key, None)
            return None
        return value

    async def _command(self, command, key, *args, **kwargs):
        if self._client is None:
            raise StateUnavailable('Shared authentication state unavailable')
        try:
            return await getattr(self._client, command)(self._key(key), *args, **kwargs)
        except Exception as exc:
            raise StateUnavailable('Shared authentication state unavailable') from exc

    async def _eval(self, script, num_keys, key, *args):
        if self._client is None:
            raise StateUnavailable('Shared authentication state unavailable')
        try:
            return await self._client.eval(script, num_keys, self._key(key), *args)
        except Exception as exc:
            raise StateUnavailable('Shared authentication state unavailable') from exc

    async def get(self, key):
        if self._memory_enabled():
            return self._mem_get(self._key(key))
        return await self._command('get', key)

    async def set(self, key, value, expire: Optional[int] = None):
        if expire is not None and expire <= 0:
            raise ValueError('TTL must be positive')
        if self._memory_enabled():
            self._memory[self._key(key)] = (value, time.monotonic() + expire if expire else None)
            return
        await self._command('set', key, value, ex=expire)

    async def delete(self, key):
        if self._memory_enabled():
            self._memory.pop(self._key(key), None)
            return
        await self._command('delete', key)

    async def incr(self, key, expire: int):
        if expire <= 0:
            raise ValueError('TTL must be positive')
        if self._memory_enabled():
            memory_key = self._key(key)
            current = self._mem_get(memory_key)
            value = int(current or 0) + 1
            existing = self._memory.get(memory_key)
            deadline = existing[1] if existing is not None else time.monotonic() + expire
            self._memory[memory_key] = (str(value), deadline)
            return value
        value = int(await self._eval(INCREMENT_SCRIPT, 1, key, expire))
        return value

    async def get_or_create(self, key, value, expire):
        if self._memory_enabled():
            existing = self._mem_get(self._key(key))
            if existing is not None:
                return existing
            await self.set(key, value, expire)
            return value
        await self._command('set', key, value, ex=expire, nx=True)
        result = await self.get(key)
        if result is None:
            raise StateUnavailable('Security state expired during initialization')
        return result

    async def getdel_json(self, key):
        if self._memory_enabled():
            data = self._mem_get(self._key(key), pop=True)
        else:
            data = await self._command('getdel', key)
        return json.loads(data) if data is not None else None

    async def set_json(self, key, value: Any, expire=None):
        await self.set(key, json.dumps(value), expire)

    async def get_json(self, key):
        data = await self.get(key)
        return json.loads(data) if data is not None else None

    async def exists(self, key):
        return await self.get(key) is not None

    async def ping(self):
        if self._memory_enabled():
            return True
        if self._client is None:
            raise StateUnavailable('Shared authentication state unavailable')
        try:
            return await self._client.ping()
        except Exception as exc:
            raise StateUnavailable('Shared authentication state unavailable') from exc

    async def validate_getdel(self):
        if self._memory_enabled():
            return
        if self._client is None:
            raise StateUnavailable('Shared authentication state unavailable')
        capability_key = f'{_settings.redis_prefix}:capability:getdel:{uuid.uuid4().hex}'
        try:
            await self._client.getdel(capability_key)
        except Exception as exc:
            raise StateUnavailable('Redis 6.2+ with GETDEL is required') from exc

    async def validate_ready(self):
        await self.ping()
        await self.validate_getdel()

redis_client = RedisClient()
