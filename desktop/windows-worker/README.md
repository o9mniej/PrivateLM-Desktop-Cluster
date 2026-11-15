# PrivateLM Windows Worker (experimental)

The Windows worker is a **single native C++ executable**. Start
`privatelm-worker.exe` and it automatically:

1. Reads currently available system RAM and measures approximate memory-read bandwidth.
2. Detects the active LAN IPv4 address and loads the persistent worker ID.
3. Generates an app-compatible `privatelm://worker` QR code as an SVG and opens it.
4. Loads the CUDA backend when a usable GPU is available, otherwise falls back to CPU.
5. Advertises `_privatelm._tcp.local.` with the same TXT metadata as the mobile worker.
6. Starts the GGML RPC server on TCP port `50052`.

There is no Python runtime, Python dependency installation, wrapper process, or
separate setup script. Keep the console window open while the worker is in use.

## Compatibility

The worker builds against the exact llama.cpp / GGML source vendored in
`local_plugins/llama_flutter_android/android/src/main/cpp/llama.cpp`.
This keeps its RPC protocol aligned with the app.

- mDNS service: `_privatelm._tcp.local.`
- Default TCP port: `50052`
- TXT fields: `ram`, `bw`, `model`, `v=rpc7`, `id`
- QR URI: `privatelm://worker?host=...&port=...&name=...&ram=...&bw=...&id=...`

The QR reports **currently available RAM**, not installed RAM. The mobile app
continues to apply its existing 75% safety margin. The worker ID is persisted
under `%LOCALAPPDATA%\PrivateLM\worker-id`.

The QR is saved under `%LOCALAPPDATA%\PrivateLM\worker-qr-<ip>.svg` and
opened using the default browser. If it does not open automatically, open that
SVG file manually.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.22+
- NVIDIA CUDA Toolkit compatible with the vendored llama.cpp revision (optional;
  omit CUDA for a CPU-only worker)
- NVIDIA driver installed when using CUDA

No Python installation or Python packages are required.

## Build

Run from PowerShell at the repository root:

```powershell
cmake -S desktop/windows-worker -B desktop/windows-worker/build -A x64 -DPRIVATELM_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=120
cmake --build desktop/windows-worker/build --config Release
```

For a CPU-only build:

```powershell
cmake -S desktop/windows-worker -B desktop/windows-worker/build-cpu -A x64 -DPRIVATELM_ENABLE_CUDA=OFF
cmake --build desktop/windows-worker/build-cpu --config Release
```

The executable is placed in the build output's `bin` directory. Keep the
GGML runtime DLLs beside `privatelm-worker.exe`; the CI workflow stages a
ready-to-run folder containing the executable and its required DLLs.

## Run

Double-click `privatelm-worker.exe`, or run it from PowerShell:

```powershell
.\privatelm-worker.exe
```

Optional arguments:

```powershell
.\privatelm-worker.exe --port 50052 --threads 8 --name "Gaming-PC"
```

On the phone, open **PrivateLM > Phone cluster > Master > Scan a worker's QR
code**. The app does not need to be changed for this pairing flow.

Allow the worker through Windows Defender Firewall **only on Private networks**.
The RPC protocol has no authentication or encryption. Use it only on a trusted
LAN; do not expose or port-forward TCP port 50052 to the public internet.

## Current scope / limitations

- This is the Windows **worker**, not the Windows master/coordinator.
- It does not add Flutter Windows support or modify the existing mobile app.
- It serves the selected CUDA GPU when available, with CPU fallback.
- The worker chooses one active IPv4 adapter for its QR and mDNS advertisement.
  If your phone is on a different network interface, use manual pairing with
  that interface's address instead.
- Windows' built-in DNS-SD API is used for mDNS; if service advertising is
  blocked or unavailable, QR pairing remains available.
- Available RAM is sampled at startup. Restart the worker to refresh it.

## Third-party code

The QR encoder in `src/third_party` is Project Nayuki's QR Code generator,
distributed under the MIT License. See
`src/third_party/LICENSE.qrcodegen.txt`.

## Protocol alignment

The worker deliberately uses the vendored GGML headers and RPC DLL rather than
a separately downloaded llama.cpp version. Do not update llama.cpp independently
on one side of a cluster: the RPC protocol version and GGML operation set must
match between master and worker.
