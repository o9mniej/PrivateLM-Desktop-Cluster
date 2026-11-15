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

The Android app can also add it manually by IP and port if multicast discovery
is blocked by the network.

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
the generated `ggml-*.dll` files beside `privatelm-worker.exe` before running.

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
1. Measure available system RAM and approximate memory-read bandwidth.
2. Start the native GGML RPC server on `0.0.0.0:50052`.
3. Advertise itself over mDNS using the metadata expected by the original app.
4. Stop the RPC process and withdraw the mDNS record when Ctrl+C is pressed.

Allow the worker through Windows Defender Firewall **only on Private networks**.
Do not expose TCP 50052 to the public internet.

## Current scope / limitations

- This is the **worker**, not the Windows master/coordinator yet.
- It does not modify the original Flutter app.
- It serves locally registered CPU and CUDA devices when those backends are
  built and load successfully.
- The Python discovery wrapper needs multicast/mDNS to be allowed on the LAN.
  If discovery fails, use the app's manual IP entry.
- The RPC protocol itself has no authentication or encryption. Run only on a
  trusted LAN. Do not port-forward this service.
- The first commit is an integration scaffold and has not yet been compiled or
  tested on Windows. Backend DLL placement and CUDA build compatibility still
  need validation on a Windows machine.

## Protocol alignment

The worker deliberately uses the vendored GGML headers and RPC DLL rather than
a separately downloaded llama.cpp version. Do not update llama.cpp independently
on one side of a cluster: the RPC protocol version and GGML operation set must
match between master and worker.
