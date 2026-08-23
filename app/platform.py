"""UniSSO 平台环境感知模块

读取 ssesinfra 平台注入的环境变量和上下文文件，实现：
1. 宿主机信息读取（context.json, deploy-info.json）
2. 负载感知（GPU/CPU 状态）
3. 动态调整应用行为（worker 数、日志级别等）
4. 平台上报（state + metrics）
"""
import os
import json
import threading
import urllib.request
from typing import Optional, Dict, Any
from dataclasses import dataclass


@dataclass
class HostContext:
    """宿主机上下文"""
    group_name: str = ""
    public_url: str = ""
    host_name: str = ""
    gpu_ids: str = ""
    gpu_models: str = ""
    deploy_id: str = ""
    deploy_name: str = ""
    shared_dir: str = "/shared"
    # 调度信息
    scheduling: Dict[str, Any] = None
    # 服务信息
    services: Dict[str, Any] = None


@dataclass
class RuntimeConfig:
    """运行时动态配置"""
    workers: int = 2
    log_level: str = "info"
    max_requests_per_worker: int = 0  # 0 = 不限制
    enable_metrics: bool = True


# ==================== 读取宿主机上下文 ====================

def _read_context_json() -> Dict[str, Any]:
    """读取平台写入的 context.json"""
    paths = [
        "/home/cloud/deploy/context.json",
        "/home/sse/deploy/context.json",
    ]
    for p in paths:
        try:
            with open(p, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return {}


def _read_deploy_json(deploy_id: str) -> Dict[str, Any]:
    """读取本部署信息"""
    if not deploy_id:
        return {}
    paths = [
        f"/home/cloud/deploy/deploy-{deploy_id}.json",
        f"/home/sse/deploy/deploy-{deploy_id}.json",
    ]
    for p in paths:
        try:
            with open(p, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return {}


def load_host_context() -> HostContext:
    """加载宿主机上下文"""
    ctx = _read_context_json()
    deploy_id = os.environ.get("DEPLOY_ID", "")
    deploy_info = _read_deploy_json(deploy_id)

    host = ctx.get("host", {})
    scheduling = ctx.get("scheduling", {})
    services = ctx.get("services", {})
    public = ctx.get("public", {})

    return HostContext(
        group_name=os.environ.get("GROUP_NAME", ctx.get("group", "")),
        public_url=os.environ.get("PUBLIC_URL", public.get("url", "")),
        host_name=os.environ.get("HOST_NAME", host.get("name", "")),
        gpu_ids=os.environ.get("GPU_IDS", host.get("gpu_ids", "")),
        gpu_models=os.environ.get("GPU_MODELS", host.get("gpu_models", "")),
        deploy_id=deploy_id,
        deploy_name=os.environ.get("DEPLOY_NAME", deploy_info.get("name", "")),
        shared_dir=os.environ.get("SHARED_DIR", "/shared"),
        scheduling=scheduling,
        services=services,
    )


# ==================== 负载感知 ====================

def _read_loadavg() -> tuple[float, float, float]:
    """读取 Linux loadavg"""
    try:
        with open("/proc/loadavg", "r") as f:
            parts = f.read().strip().split()
            return float(parts[0]), float(parts[1]), float(parts[2])
    except Exception:
        return 0.0, 0.0, 0.0


def _read_meminfo() -> Dict[str, int]:
    """读取内存信息（KB）"""
    result = {}
    try:
        with open("/proc/meminfo", "r") as f:
            for line in f:
                if ":" in line:
                    key, val = line.split(":", 1)
                    result[key.strip()] = int(val.strip().split()[0])
    except Exception:
        pass
    return result


def _count_cpu_cores() -> int:
    """获取 CPU 核心数"""
    try:
        return os.cpu_count() or 2
    except Exception:
        return 2


def compute_runtime_config() -> RuntimeConfig:
    """根据宿主机负载计算最优运行时配置"""
    cores = _count_cpu_cores()
    load1, load5, load15 = _read_loadavg()
    mem = _read_meminfo()
    mem_total = mem.get("MemTotal", 0)
    mem_available = mem.get("MemAvailable", mem.get("MemFree", 0))

    # 基础 worker 数：CPU 核心数的一半，最少 1 个，最多 4 个
    base_workers = max(1, min(cores // 2, 4))

    # 负载调整：如果 1 分钟负载超过核心数的 80%，减少 worker
    if load1 > cores * 0.8:
        workers = max(1, base_workers - 1)
        log_level = "warning"
    elif load1 > cores * 0.5:
        workers = base_workers
        log_level = "info"
    else:
        workers = min(base_workers + 1, 4)
        log_level = "info"

    # 内存调整：可用内存 < 500MB 时减少 worker
    if mem_available > 0 and mem_available < 500 * 1024:  # 500MB
        workers = max(1, workers - 1)
        log_level = "warning"

    # GPU 感知：如果有 GPU，稍微增加 worker（GPU 任务不占用 CPU worker）
    gpu_ids = os.environ.get("GPU_IDS", "")
    if gpu_ids:
        workers = min(workers + 1, 4)

    return RuntimeConfig(
        workers=workers,
        log_level=log_level,
        max_requests_per_worker=10000 if workers > 1 else 0,
        enable_metrics=True,
    )


# ==================== 平台上报 ====================

def report_state(state: str, message: str = "", metrics: str = ""):
    """POST 状态到平台（best-effort，永不抛异常）"""
    report_url = os.environ.get("REPORT_URL", "")
    report_token = os.environ.get("REPORT_TOKEN", "")
    deploy_id = os.environ.get("DEPLOY_ID", "")

    if not report_url or not report_token or not deploy_id:
        return

    body = json.dumps({
        "deploy_id": deploy_id,
        "state": state,
        "message": message,
        "metrics": metrics,
    }).encode()

    req = urllib.request.Request(report_url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("X-Report-Token", report_token)
    try:
        urllib.request.urlopen(req, timeout=5)
    except Exception:
        pass


def start_report_loop(interval: int = 30):
    """启动平台上报心跳线程"""
    import time

    def _loop():
        time.sleep(2)
        report_state("ready", "UniSSO 服务已启动")
        while True:
            time.sleep(interval)
            report_state("ready", "heartbeat")

    t = threading.Thread(target=_loop, daemon=True)
    t.start()
    return t


# 全局宿主机上下文（启动时加载）
host_context = load_host_context()
runtime_config = compute_runtime_config()
