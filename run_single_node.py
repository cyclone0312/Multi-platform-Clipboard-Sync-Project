#!/usr/bin/env python3
"""Cross-platform single node launcher for clipboard_sync.

Usage examples:
  python scripts/run_single_node.py --peer-host 192.168.1.20
  python scripts/run_single_node.py --listen-port 45455 --peer-port 45454 --peer-host 10.0.0.8
  python scripts/run_single_node.py --exe ./build-linux/clipboard_sync --node-id kylin-node
"""

from __future__ import annotations

import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path


def is_windows() -> bool:
    return platform.system().lower().startswith("win")


def default_ports() -> tuple[int, int]:
    # Keep defaults symmetric for common Win <-> Linux local testing.
    if is_windows():
        return 45454, 45455
    return 45455, 45454


def default_peer_host() -> str:
    # User's current VMnet setup:
    # - Windows host: 192.168.10.1
    # - Kylin guest : 192.168.10.140
    if is_windows():
        return "192.168.10.140"
    return "192.168.10.1"


def repo_root_from_script() -> Path:
    script_path = Path(__file__).resolve()
    for candidate in [script_path.parent] + list(script_path.parents):
        if (candidate / "CMakeLists.txt").exists() and (candidate / "src").exists():
            return candidate

    # Fallback to current working directory if project markers are not found.
    return Path.cwd().resolve()


def resolve_executable(repo_root: Path, user_exe: str | None) -> Path:
    if user_exe:
        exe = Path(user_exe).expanduser()
        if not exe.is_absolute():
            exe = (repo_root / exe).resolve()
        return exe

    if is_windows():
        candidates = [
            repo_root / "build" / "Desktop_Qt_5_15_2_MinGW_64_bit-Debug" / "clipboard_sync.exe",
            repo_root / "build" / "local-mingw-debug" / "clipboard_sync.exe",
            repo_root / "build" / "Release" / "clipboard_sync.exe",
            repo_root / "build" / "Debug" / "clipboard_sync.exe",
            repo_root / "build" / "clipboard_sync.exe",
        ]
    else:
        candidates = [
            repo_root / "build-linux" / "clipboard_sync",
            repo_root / "build" / "clipboard_sync",
            repo_root / "build" / "Release" / "clipboard_sync",
            repo_root / "build" / "Debug" / "clipboard_sync",
        ]

    for item in candidates:
        if item.exists() and item.is_file():
            return item

    return candidates[0]


def parse_args() -> argparse.Namespace:
    listen_default, peer_default = default_ports()
    peer_host_default = default_peer_host()

    parser = argparse.ArgumentParser(
        description="Start one clipboard_sync node with cross-platform defaults.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--peer-host", default=peer_host_default, help="Peer host/IP address")
    parser.add_argument("--listen-port", type=int, default=listen_default, help="Local listen port")
    parser.add_argument("--peer-port", type=int, default=peer_default, help="Peer listen port")
    parser.add_argument("--node-id", default="", help="Optional CSYNC_NODE_ID value")
    parser.add_argument(
        "--enable-monitor",
        type=int,
        choices=[0, 1],
        default=1,
        help="Set CSYNC_ENABLE_MONITOR (1=on, 0=off)",
    )
    parser.add_argument(
        "--exe",
        default="",
        help="Path to executable. If omitted, script auto-detects under common build folders.",
    )
    return parser.parse_args()


def validate_port(value: int, name: str) -> None:
    if value < 1 or value > 65535:
        raise ValueError(f"{name} must be in [1, 65535], got: {value}")


def main() -> int:
    args = parse_args()
    repo_root = repo_root_from_script()

    try:
        validate_port(args.listen_port, "listen-port")
        validate_port(args.peer_port, "peer-port")
    except ValueError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 2

    exe_path = resolve_executable(repo_root, args.exe or None)
    if not exe_path.exists():
        print("[ERROR] Executable not found.", file=sys.stderr)
        print(f"[ERROR] Tried: {exe_path}", file=sys.stderr)
        print("[HINT] Build the project first, or pass --exe explicitly.", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["CSYNC_LISTEN_PORT"] = str(args.listen_port)
    env["CSYNC_PEER_HOST"] = args.peer_host
    env["CSYNC_PEER_PORT"] = str(args.peer_port)
    env["CSYNC_ENABLE_MONITOR"] = str(args.enable_monitor)
    if args.node_id:
        env["CSYNC_NODE_ID"] = args.node_id

    print(f"[INFO] Platform: {platform.system()}")
    print(f"[INFO] Repo root: {repo_root}")
    print(f"[INFO] Executable: {exe_path}")
    print(f"[INFO] CSYNC_LISTEN_PORT={env['CSYNC_LISTEN_PORT']}")
    print(f"[INFO] CSYNC_PEER_HOST={env['CSYNC_PEER_HOST']}")
    print(f"[INFO] CSYNC_PEER_PORT={env['CSYNC_PEER_PORT']}")
    print(f"[INFO] CSYNC_ENABLE_MONITOR={env['CSYNC_ENABLE_MONITOR']}")
    if args.node_id:
        print(f"[INFO] CSYNC_NODE_ID={env['CSYNC_NODE_ID']}")

    try:
        completed = subprocess.run([str(exe_path)], cwd=str(repo_root), env=env)
        return int(completed.returncode)
    except KeyboardInterrupt:
        print("\n[INFO] Interrupted by user.")
        return 130
    except OSError as exc:
        print(f"[ERROR] Failed to start executable: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
