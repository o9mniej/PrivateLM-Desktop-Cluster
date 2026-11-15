# PrivateLM Linux CPU Worker (experimental)

A single native C++ worker for Linux x86-64. It builds against the exact
llama.cpp/GGML source vendored in the mobile plugin so its RPC protocol remains
aligned with the original PrivateLM app.

## Startup

Run `privatelm-worker`. It automatically:

1. Reads currently available RAM from `/proc/meminfo` and measures host-RAM read bandwidth.
2. Detects an active LAN IPv4 address and creates a persistent worker ID.
3. Checks CPU compatibility and loads the fastest available GGML CPU backend (AVX2, then AVX, then a portable baseline), followed by the RPC backend.
4. Generates the existing `privatelm://worker` QR format and saves an SVG under
   `${XDG_DATA_HOME:-~/.local/share}/PrivateLM/`.
5. Opens the QR with `xdg-open` when available.
6. Attempts mDNS advertisement through `avahi-publish-service`.
7. Starts the GGML RPC server on TCP port 50052.

mDNS is optional. Install Avahi utilities and ensure the Avahi daemon is running
for automatic discovery. QR/manual pairing remains available without Avahi.

## Requirements

- Linux x86-64
- CMake 3.22+, a C/C++ compiler, and Git
- Avahi utilities for mDNS discovery (optional)
- The build uses the repository's vendored llama.cpp source; no Python is needed.

On Debian/Ubuntu, typical build dependencies are:

```sh
sudo apt update
sudo apt install -y build-essential cmake git pkg-config
```

Optional mDNS support:

```sh
sudo apt install -y avahi-daemon avahi-utils
sudo systemctl enable --now avahi-daemon
```

## Build

From the repository root:

```sh
cmake -S desktop/linux-worker -B desktop/linux-worker/build -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/linux-worker/build --parallel
```

The CI artifact `privatelm-linux-worker-cpu-x64` includes three CPU backend
variants: AVX2, AVX, and a portable baseline. At startup the worker tries them
in that order. GGML's own backend score function checks the CPU's supported
instruction sets; an incompatible optimized variant is rejected before it is
registered, and the worker tries the next one.

For a manual single-variant build, keep the generated `libggml-*.so` files
beside `privatelm-worker` in the same directory. The worker also accepts the
standard `libggml-cpu.so` filename as a final fallback.

## Run

```sh
./privatelm-worker
```

Optional arguments:

```sh
./privatelm-worker --port 50052 --threads 8 --name "Linux-PC"
```

Pair from the original mobile app using **PrivateLM > Phone cluster > Master >
Scan a worker's QR code**. The worker uses the existing GGML RPC protocol and
does not require a modified mobile app.

## Security and limitations

- This is a worker, not a master/coordinator.
- RPC has no authentication or encryption. Use only on a trusted private LAN;
  do not port-forward the RPC port to the public internet.
- RAM and network address are sampled at startup. Restart to refresh them.
- The first version targets Linux x86-64 CPU only. The CI build includes AVX2, AVX, and baseline variants so older and newer x86-64 CPUs can use the fastest compatible backend. ARM64/Raspberry Pi and CUDA
  will be added separately after this target is validated.
- If the host has multiple LAN adapters, the worker chooses a private IPv4
  address first; manual pairing can be used with another interface address.

## QR encoder

The QR encoder is Project Nayuki's QR Code generator under the MIT License.
See `src/third_party/LICENSE.qrcodegen.txt`.
