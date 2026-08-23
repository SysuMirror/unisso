#!/usr/bin/env python3
"""UniSSO 启动器

读取宿主机负载动态计算 gunicorn 参数，然后 exec 替换为 gunicorn 进程。
符合 ssesinfra 平台契约：长驻前台、绑定 0.0.0.0:$PORT。

支持两种 HTTPS 场景：
1. 平台反向代理（Nginx/Traefik）：设置 TRUST_PROXY=true
2. 应用直接监听 HTTPS：设置 HTTPS_CERT + HTTPS_KEY
"""
import os
import sys

# 确保项目根目录在 PYTHONPATH
project_root = os.path.dirname(os.path.abspath(__file__))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from app.platform import compute_runtime_config, host_context, report_state


def main():
    port = os.environ.get("PORT", "8080")
    rt = compute_runtime_config()

    https_cert = os.environ.get("HTTPS_CERT", "")
    https_key = os.environ.get("HTTPS_KEY", "")
    use_https = bool(https_cert and https_key)

    print(f"【UniSSO 启动器】端口={port}, workers={rt.workers}, log_level={rt.log_level}")
    print(f"【UniSSO 启动器】宿主机={host_context.host_name}, GPU={host_context.gpu_models}")
    if use_https:
        print(f"【UniSSO 启动器】HTTPS 模式: cert={https_cert}, key={https_key}")
    else:
        print(f"【UniSSO 启动器】HTTP 模式（如需 HTTPS，设置 HTTPS_CERT + HTTPS_KEY）")

    cmd = [
        sys.executable, "-m", "gunicorn",
        "-w", str(rt.workers),
        "-k", "uvicorn.workers.UvicornWorker",
        "-b", f"0.0.0.0:{port}",
        "--access-logfile", "-",
        "--error-logfile", "-",
        "--log-level", rt.log_level,
        "--capture-output",
        "--enable-stdio-inheritance",
    ]

    # HTTPS 证书
    if use_https:
        cmd += ["--certfile", https_cert]
        cmd += ["--keyfile", https_key]

    if rt.max_requests_per_worker > 0:
        cmd += ["--max-requests", str(rt.max_requests_per_worker)]
        cmd += ["--max-requests-jitter", str(rt.max_requests_per_worker // 10)]

    # 超时设置
    cmd += ["--timeout", "120"]
    cmd += ["--keep-alive", "5"]

    # 预加载（减少内存占用）
    cmd += ["--preload"]

    cmd += ["app.main:app"]

    report_state("starting", f"workers={rt.workers}, log_level={rt.log_level}, https={use_https}")
    os.execvp(cmd[0], cmd)


if __name__ == "__main__":
    main()
