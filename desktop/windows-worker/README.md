# PrivateLM Windows Worker (experimental)

This folder adds a **Windows GGML RPC worker** without changing the Flutter
application or its Android/iOS plugin.

It builds against the exact llama.cpp source vendored in
`local_plugins/llama_flutter_android/android/src/main/cpp/llama.cpp`.
The worker uses GGML RPC protocol **7.0.0** and advertises the same mDNS
service and TXT fields as the original mobile worker:

- Service: `_privatelm._tcp.`
- Default TCP port: `50052`
- TXT: `ram`, `bw`, `model`, `v=rpc7`, `id`

It also generates a QR code in the same `privatelm://worker` format already
understood by the original app. The QR contains the worker IP, port, name,
available RAM, memory bandwidth, and persistent worker ID. Available RAM is
reported (not installed RAM), matching the mobile worker's existing metadata.
The app applies its existing 75% safety margin when estimating worker capacity.

The QR PNG is saved under `%LOCALAPPDATA%\PrivateLM\worker-qr-<ip>.png`
and the first detected address's QR is opened automatically. If the PC has
multiple network interfaces, use the QR for the address on the same LAN as
the phone.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.22+
- Python 3.10+
- NVIDIA CUDA Toolkit compatible with the CUDA version supported by the
  vendored llama.cpp revision (optional; omit CUDA for a CPU-only worker)
- NVIDIA driver installed if using CUDA

## Build

Run from PowerShell:

```powershell
cd desktop/windows-worker
cmake -S . -B build -A x64 -DPRIVATELM_ENABLE_CUDA=ON
cmake --build build --config Release
```

For a CPU-only build:

```powershell
cmake -S . -B build -A x64 -DPRIVATELM_ENABLE_CUDA=OFF
cmake --build build --config Release
```

The executable and GGML backend DLLs are expected in the build output directory.
If CMake places backend DLLs in a different configuration subdirectory, copy
the generated backend DLLs beside `privatelm-worker.exe` before running.

## Install Python dependencies

```powershell
py -m pip install -r requirements.txt
```

## Run

```powershell
.\run-worker.ps1
```

Or choose a port and thread count:

```powershell
.\run-worker.ps1 -Port 50052 -Threads 8 -Name "Gaming-PC"
```

The worker will:
1. Measure currently available system RAM and approximate memory-read bandwidth.
2. Generate a QR code containing the app-compatible worker URI and open the first
   detected network address's QR image.
3. Start the native GGML RPC server on `0.0.0.0:50052`.
4. Advertise itself over mDNS using the same metadata.
5. Stop the RPC process and withdraw the mDNS record when Ctrl+C is pressed.

On the phone, open **PrivateLM > Phone cluster > Master > Scan a worker's QR code**.
If the PC has multiple network interfaces, scan the QR for the IP on the phone's
Wi-Fi network.

Allow the worker through Windows Defender Firewall **only on Private networks**.
Do not expose TCP 50052 to the public internet.

## Current scope / limitations

- This is the **worker**, not the Windows master/coordinator yet.
- It does not modify the original Flutter application.
- It serves locally registered CPU and CUDA devices when those backends are
  built and load successfully.
- mDNS discovery may be blocked by some networks; QR pairing avoids needing
  multicast discovery.
- The RPC protocol itself has no authentication or encryption. Run only on a
  trusted LAN. Do not port-forward this service.
- Available RAM is sampled when the worker starts, matching the existing
  mobile worker's "free at start" behaviour. Restart the worker to refresh it.

## Protocol alignment

The worker deliberately uses the vendored GGML headers and RPC DLL rather than
a separately downloaded llama.cpp version. Do not update llama.cpp independently
on one side of a cluster: the RPC protocol version and GGML operation set must
match between master and worker.
