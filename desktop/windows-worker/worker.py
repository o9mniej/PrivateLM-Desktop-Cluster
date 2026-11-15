#!/usr/bin/env python3
"""Advertise and supervise the Windows GGML RPC worker for PrivateLM."""

from __future__ import annotations

import argparse
import ipaddress
import os
import platform
import subprocess
import time
import uuid
from pathlib import Path
from urllib.parse import urlencode

import numpy as np
import psutil
import qrcode
from zeroconf import ServiceInfo, Zeroconf

SERVICE_TYPE = "_privatelm._tcp.local."
DEFAULT_PORT = 50052


def worker_id() -> str:
    root = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "PrivateLM"
    root.mkdir(parents=True, exist_ok=True)
    path = root / "worker-id"
    if path.exists():
        value = path.read_text(encoding="utf-8").strip()
        if value:
            return value
    value = str(uuid.uuid4())
    path.write_text(value, encoding="utf-8")
    return value


def memory_bandwidth_gbps() -> float:
    # Same basic read-throughput idea as the mobile worker's benchmark:
    # stream through a 64 MiB buffer several times using native NumPy loops.
    data = np.ones(8 * 1024 * 1024, dtype=np.uint64)
    np.sum(data)  # warm up / fault in pages
    start = time.perf_counter()
    checksum = 0
    for _ in range(4):
        checksum += int(np.sum(data))
    elapsed = time.perf_counter() - start
    if checksum == 0 or elapsed <= 0:
        return 0.0
    return (data.nbytes * 4) / elapsed / 1_000_000_000


def local_ipv4_addresses() -> list[str]:
    addresses: list[str] = []
    for item in psutil.net_if_addrs().values():
        for address in item:
            if address.family.name == "AF_INET" and not address.address.startswith("127."):
                addresses.append(address.address)
    # Prefer ordinary private-LAN addresses over VPN/tunnel interfaces so
    # the QR opened automatically is more likely to match the phone's Wi-Fi.
    return sorted(
        set(addresses),
        key=lambda value: (not ipaddress.ip_address(value).is_private, value),
    )


def make_worker_qr(
    host: str,
    port: int,
    name: str,
    ram_mb: int,
    bandwidth: float,
    worker_id_value: str,
) -> Path:
    """Write a QR using the exact URI format understood by the mobile app."""
    params = {
        "host": host,
        "port": str(port),
        "name": name,
        "ram": str(ram_mb),
        "bw": f"{bandwidth:.1f}",
        "id": worker_id_value,
    }
    payload = f"privatelm://worker?{urlencode(params)}"

    output_dir = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "PrivateLM"
    output_dir.mkdir(parents=True, exist_ok=True)
    image_path = output_dir / f"worker-qr-{host.replace('.', '-')}.png"

    image = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=10,
        border=4,
    )
    image.add_data(payload)
    image.make(fit=True)
    image.make_image(fill_color="black", back_color="white").save(image_path)
    return image_path


def main() -> int:
    parser = argparse.ArgumentParser(description="PrivateLM Windows cluster worker")
    parser.add_argument("--rpc-exe", required=True, help="Path to privatelm-worker.exe")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--threads", type=int, default=0)
    parser.add_argument("--name", default=platform.node())
    args = parser.parse_args()

    rpc_exe = Path(args.rpc_exe).expanduser().resolve()
    if not rpc_exe.is_file():
        parser.error(f"RPC worker executable does not exist: {rpc_exe}")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")

    # Match the mobile worker's metadata: report currently available RAM,
    # not installed RAM. The app applies its own safety margin for placement.
    ram_mb = int(psutil.virtual_memory().available / (1024 * 1024))
    bandwidth = memory_bandwidth_gbps()
    addresses = local_ipv4_addresses()
    current_worker_id = worker_id()

    print(f"Worker ID: {current_worker_id}")
    print(f"Available RAM: {ram_mb} MiB")
    print(f"Measured memory read bandwidth: {bandwidth:.1f} GB/s")
    print("Local IPv4: " + (", ".join(addresses) if addresses else "not detected"))

    qr_paths: list[Path] = []
    for address in addresses:
        qr_path = make_worker_qr(
            address, args.port, args.name, ram_mb, bandwidth, current_worker_id
        )
        qr_paths.append(qr_path)
        print(f"QR for {address}: {qr_path}")

    if qr_paths and os.name == "nt":
        try:
            os.startfile(str(qr_paths[0]))
            if len(qr_paths) > 1:
                print("Opened the first QR. If it cannot connect, use another QR above.")
        except OSError as error:
            print(f"Could not open the QR image automatically: {error}")
    elif not addresses:
        print("No QR generated: connect to Wi-Fi/Ethernet and restart the worker.")

    print("Starting RPC server...")
    command = [str(rpc_exe), "--listen", f"0.0.0.0:{args.port}"]
    if args.threads > 0:
        command += ["--threads", str(args.threads)]
    cache_dir = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "PrivateLM" / "rpc-cache"
    command += ["--cache-dir", str(cache_dir)]

    process = subprocess.Popen(command, cwd=str(rpc_exe.parent))
    zeroconf = Zeroconf()
    service_name = f"PrivateLM {args.name}._privatelm._tcp.local."
    info = ServiceInfo(
        SERVICE_TYPE,
        service_name,
        port=args.port,
        properties={
            "ram": str(ram_mb),
            "bw": f"{bandwidth:.1f}",
            "model": args.name,
            "v": "rpc7",
            "id": current_worker_id,
        },
        server=f"{platform.node().replace(' ', '-')}.local.",
        addresses=[bytes(map(int, ip.split("."))) for ip in addresses],
    )

    registered = False
    try:
        zeroconf.register_service(info)
        registered = True
        print(f"Advertising as {service_name} on TCP {args.port}")
        print("Scan the opened QR from PrivateLM > Phone cluster > Master.")
        print("The QR includes available RAM; the app will apply its normal safety margin.")
        print("Press Ctrl+C to stop.")
        while process.poll() is None:
            time.sleep(0.5)
        return int(process.returncode or 0)
    except KeyboardInterrupt:
        print("\nStopping worker...")
        process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
        return 0
    finally:
        if registered:
            zeroconf.unregister_service(info)
        zeroconf.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    raise SystemExit(main())
