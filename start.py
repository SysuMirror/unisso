#!/usr/bin/env python3
"""Start workers with explicit container configuration; no database or network writes."""
import os
import sys

def main():
    workers = int(os.environ.get('UNISSO_WORKERS', os.environ.get('WORKERS', '2')))
    if workers < 1 or workers > 32:
        raise ValueError('Worker count must be between 1 and 32')
    # Trusted proxy handling belongs to request_security, not implicit server defaults.
    cmd = [sys.executable, '-m', 'gunicorn', '-w', str(workers),
        '-k', 'uvicorn.workers.UvicornWorker', '-b', '0.0.0.0:' + os.environ.get('PORT', '8080'),
        '--forwarded-allow-ips=', '--access-logfile', '-', '--error-logfile', '-',
        '--timeout', '120', '--keep-alive', '5', 'app.main:app']
    cert, key = os.environ.get('HTTPS_CERT'), os.environ.get('HTTPS_KEY')
    if bool(cert) != bool(key):
        raise ValueError('HTTPS_CERT and HTTPS_KEY must be provided together')
    if cert:
        cmd[-1:-1] = ['--certfile', cert, '--keyfile', key]
    os.execvp(cmd[0], cmd)

if __name__ == '__main__':
    main()
