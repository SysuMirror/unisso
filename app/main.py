"""Application composition. Serving requests never performs maintenance."""
from contextlib import asynccontextmanager, suppress
import asyncio
import os
from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pathlib import Path
from sqlalchemy import text
from app.config import get_settings
from app.database import engine
from app.redis_client import redis_client, StateUnavailable
from app.token_keys import validate_signing_configuration
from app.views import templates
from app.middleware import SecurityHeadersMiddleware, RateLimitMiddleware, TrustedHostMiddleware, HttpsDetectionMiddleware
from app.request_security import CSRFMiddleware
from app.routes import router

_settings = get_settings()

async def check_database():
    async with engine.connect() as connection:
        version = await connection.scalar(text('SELECT version_num FROM alembic_version'))
        if version != '002':
            raise RuntimeError('Database migration 002 is required; run maintenance separately')

async def report_health():
    from app.platform import report_state
    while True:
        try:
            await redis_client.validate_ready()
            await check_database()
            state = 'ready'
        except Exception:
            state = 'degraded'
        await asyncio.to_thread(report_state, state, 'dependency health check')
        await asyncio.sleep(30)

@asynccontextmanager
async def lifespan(app):
    app.state.ready = False
    validate_signing_configuration()
    heartbeat = None
    try:
        await redis_client.connect()
        await check_database()
        app.state.ready = True
        if os.environ.get('DEPLOY_ID'):
            heartbeat = asyncio.create_task(report_health())
        yield
    finally:
        app.state.ready = False
        if heartbeat:
            heartbeat.cancel()
            with suppress(asyncio.CancelledError):
                await heartbeat
        await redis_client.disconnect()
        await engine.dispose()

app = FastAPI(title=_settings.app_name, version=_settings.app_version,
    lifespan=lifespan, root_path=_settings.root_path)
app.state.ready = False
app.add_middleware(CSRFMiddleware)
app.add_middleware(RateLimitMiddleware)
app.add_middleware(HttpsDetectionMiddleware)
app.add_middleware(TrustedHostMiddleware)
app.add_middleware(SecurityHeadersMiddleware)
app.add_middleware(CORSMiddleware, allow_origins=_settings.cors_origins,
    allow_credentials=True, allow_methods=['GET','POST','PUT','DELETE','OPTIONS'],
    allow_headers=['Authorization','Content-Type','X-CSRF-Token'])
if Path('static').is_dir():
    app.mount('/static', StaticFiles(directory='static'), name='static')
app.include_router(router)

@app.exception_handler(HTTPException)
async def http_exception_handler(request, exc):
    if 300 <= exc.status_code < 400 and (exc.headers or {}).get('Location'):
        return RedirectResponse(exc.headers['Location'], status_code=exc.status_code)
    return JSONResponse({'error': exc.detail}, status_code=exc.status_code, headers=exc.headers)

@app.exception_handler(StateUnavailable)
async def state_unavailable(request, exc):
    return JSONResponse({'error':'authentication_state_unavailable'}, status_code=503)

@app.get('/ready')
async def readiness():
    try:
        if not app.state.ready:
            raise RuntimeError('not ready')
        await redis_client.validate_ready()
        await check_database()
    except Exception:
        return JSONResponse({'ok':False}, status_code=503)
    return {'ok':True}
