#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ggml-backend.h"
#include "ggml-rpc.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static fs::path executable_dir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        throw std::runtime_error("Could not determine executable directory");
    }
    return fs::path(buffer).parent_path();
#else
    return fs::current_path();
#endif
}

static std::string arg_value(int argc, char** argv, const std::string& key,
                             const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == key) return argv[i + 1];
    }
    return fallback;
}

int main(int argc, char** argv) {
    try {
        const auto exe_dir = executable_dir();
        const std::string listen = arg_value(argc, argv, "--listen", "0.0.0.0:50052");
        const std::string cache = arg_value(argc, argv, "--cache-dir", "rpc-cache");
        const std::string threads_text = arg_value(argc, argv, "--threads", "0");
        size_t threads = 0;
        try { threads = static_cast<size_t>(std::stoul(threads_text)); }
        catch (...) { throw std::runtime_error("--threads must be a positive integer"); }
        if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());

        std::cout << "PrivateLM Windows Worker\n"
                  << "Loading GGML backends from: " << exe_dir.string() << "\n";

        const auto load_backend = [&](const char* dll) {
            const fs::path path = exe_dir / dll;
            if (!fs::exists(path)) {
                std::cerr << "Backend not found (optional): " << path.string() << "\n";
                return false;
            }
            if (!ggml_backend_load(path.string().c_str())) {
                std::cerr << "Could not load backend: " << path.string() << "\n";
                return false;
            }
            std::cout << "Loaded " << dll << "\n";
            return true;
        };

        if (!load_backend("ggml-cpu.dll")) {
            throw std::runtime_error("CPU backend is required");
        }
        // CUDA is optional at runtime; a CPU-only worker is still useful.
        load_backend("ggml-cuda.dll");

        ggml_backend_dev_t devices[GGML_RPC_MAX_SERVERS] = {};
        size_t device_count = 0;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            const auto type = ggml_backend_dev_type(dev);
            if (type != GGML_BACKEND_DEVICE_TYPE_CPU &&
                type != GGML_BACKEND_DEVICE_TYPE_GPU &&
                type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
                continue;
            }
            if (device_count >= GGML_RPC_MAX_SERVERS) break;
            devices[device_count++] = dev;
            std::cout << "Serving device: " << ggml_backend_dev_name(dev)
                      << " (" << ggml_backend_dev_description(dev) << ")\n";
        }
        if (device_count == 0) throw std::runtime_error("No local GGML devices were registered");

        if (!load_backend("ggml-rpc.dll")) {
            throw std::runtime_error("RPC backend is required");
        }

        ggml_backend_reg_t rpc = ggml_backend_reg_by_name("RPC");
        if (!rpc) throw std::runtime_error("RPC backend registry was not registered");

        using start_server_fn = void (*)(const char*, const char*, size_t, size_t,
                                         ggml_backend_dev_t*);
        auto start_server = reinterpret_cast<start_server_fn>(
            ggml_backend_reg_get_proc_address(rpc, "ggml_backend_rpc_start_server"));
        if (!start_server) throw std::runtime_error("RPC server entry point is missing");

        fs::create_directories(cache);
        std::cout << "\nListening on " << listen << "\n"
                  << "Devices: " << device_count << " | CPU threads: " << threads << "\n"
                  << "Keep this window open while the worker is in use.\n"
                  << "RPC has no authentication; only expose it on a trusted LAN.\n";

        start_server(listen.c_str(), cache.c_str(), threads, device_count, devices);
        std::cerr << "RPC server returned unexpectedly.\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "Worker error: " << e.what() << "\n";
        return 1;
    }
}
