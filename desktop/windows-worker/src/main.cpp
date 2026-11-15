#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <windns.h>
#include <shellapi.h>
#include <objbase.h>
#endif

#include "ggml-backend.h"
#include "ggml-rpc.h"
#include "third_party/qrcodegen.hpp"

namespace fs = std::filesystem;

#ifdef _WIN32

struct NetworkAddress {
    std::string address;
    ULONG interface_index = 0;
    bool is_private = false;
};

static std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), count, nullptr, nullptr);
    return result;
}

static std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
    return result;
}

static fs::path local_app_data() {
    wchar_t buffer[32768] = {};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (count == 0 || count >= std::size(buffer)) {
        throw std::runtime_error("Could not locate the Windows Local AppData folder");
    }
    return fs::path(buffer);
}

static std::string computer_name() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD length = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameW(buffer, &length)) return "Windows-PC";
    return narrow(std::wstring(buffer, length));
}

static std::string make_worker_id(const fs::path& root) {
    fs::create_directories(root);
    const fs::path id_path = root / L"worker-id";
    {
        std::ifstream input(id_path);
        std::string existing;
        if (input && std::getline(input, existing) && !existing.empty()) return existing;
    }

    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Could not create a persistent worker ID");
    char value[64] = {};
    sprintf_s(value, "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    std::ofstream output(id_path, std::ios::trunc);
    if (!output) throw std::runtime_error("Could not save the worker ID");
    output << value << '\n';
    return value;
}

static bool is_private_ipv4(const IN_ADDR& address) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&address.S_un.S_addr);
    return bytes[0] == 10 ||
        (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
        (bytes[0] == 192 && bytes[1] == 168);
}

static std::vector<NetworkAddress> local_ipv4_addresses() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> storage(size);
    ULONG result = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data()), &size);
    }
    if (result != NO_ERROR) throw std::runtime_error("Could not enumerate network adapters");

    std::vector<NetworkAddress> addresses;
    for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(storage.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* item = adapter->FirstUnicastAddress; item != nullptr; item = item->Next) {
            if (!item->Address.lpSockaddr || item->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* socket_address = reinterpret_cast<const sockaddr_in*>(item->Address.lpSockaddr);
            if (socket_address->sin_addr.S_un.S_addr == htonl(INADDR_LOOPBACK)) continue;
            char text[INET_ADDRSTRLEN] = {};
            if (!InetNtopA(AF_INET, const_cast<IN_ADDR*>(&socket_address->sin_addr), text, sizeof(text))) continue;
            NetworkAddress entry{ text, adapter->IfIndex, is_private_ipv4(socket_address->sin_addr) };
            if (std::none_of(addresses.begin(), addresses.end(), [&](const auto& current) {
                return current.address == entry.address;
            })) addresses.push_back(entry);
        }
    }
    std::stable_sort(addresses.begin(), addresses.end(), [](const auto& a, const auto& b) {
        return a.is_private && !b.is_private;
    });
    return addresses;
}

static double measure_memory_bandwidth_gbps() {
    constexpr size_t word_count = 8u * 1024u * 1024u; // 64 MiB
    std::vector<uint64_t> data(word_count, 1);
    volatile uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 4; ++pass) {
        const volatile uint64_t* values = data.data();
        uint64_t sum = 0;
        for (size_t i = 0; i < word_count; ++i) sum += values[i];
        checksum = checksum + sum;
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (checksum == 0 || elapsed <= 0.0) return 0.0;
    return (static_cast<double>(data.size() * sizeof(uint64_t) * 4) / elapsed) / 1'000'000'000.0;
}

static std::string url_encode(const std::string& value) {
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            output << static_cast<char>(c);
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return output.str();
}

static fs::path write_qr_svg(const fs::path& output_dir, const std::string& host,
    unsigned short port, const std::string& name, uint64_t ram_mb, double bandwidth,
    const std::string& worker_id) {
    std::ostringstream uri;
    uri << "privatelm://worker?host=" << host
        << "&port=" << port
        << "&name=" << url_encode(name)
        << "&ram=" << ram_mb
        << "&bw=" << std::fixed << std::setprecision(1) << bandwidth
        << "&id=" << url_encode(worker_id);

    const auto qr = qrcodegen::QrCode::encodeText(uri.str().c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int quiet_zone = 4;
    const int dimension = qr.getSize() + quiet_zone * 2;
    const fs::path output_path = output_dir / (L"worker-qr-" + widen(host) + L".svg");
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not write the worker QR code");

    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 "
           << dimension << " " << dimension << "\" shape-rendering=\"crispEdges\">"
           << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>"
           << "<path fill=\"black\" d=\"";
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (qr.getModule(x, y)) {
                output << "M" << x + quiet_zone << "," << y + quiet_zone
                       << "h1v1h-1z";
            }
        }
    }
    output << "\"/></svg>\n";
    if (!output) throw std::runtime_error("Failed while writing the worker QR code");
    return output_path;
}

static VOID WINAPI on_mdns_registration(DWORD status, PVOID, PDNS_SERVICE_INSTANCE instance) {
    if (status != ERROR_SUCCESS) {
        std::cerr << "mDNS registration callback reported Windows error " << status << ".\n";
    }
    if (instance) DnsServiceFreeInstance(instance);
}

static bool advertise_worker(const std::string& name, const NetworkAddress& network,
    unsigned short port, uint64_t ram_mb, double bandwidth, const std::string& id) {
    const std::wstring instance_name = L"PrivateLM " + widen(name) + L"._privatelm._tcp.local.";
    const std::wstring host_name = widen(name) + L".local.";
    const std::wstring ram = std::to_wstring(ram_mb);
    std::wostringstream bw_stream;
    bw_stream << std::fixed << std::setprecision(1) << bandwidth;
    const std::wstring bw = bw_stream.str();
    const std::wstring model = widen(name);
    const std::wstring version = L"rpc7";
    const std::wstring worker_id = widen(id);

    PCWSTR keys[] = { L"ram", L"bw", L"model", L"v", L"id" };
    PCWSTR values[] = { ram.c_str(), bw.c_str(), model.c_str(), version.c_str(), worker_id.c_str() };
    IN_ADDR parsed{};
    if (InetPtonA(AF_INET, network.address.c_str(), &parsed) != 1) return false;
    IP4_ADDRESS ip4 = parsed.S_un.S_addr;

    PDNS_SERVICE_INSTANCE instance = DnsServiceConstructInstance(
        instance_name.c_str(), host_name.c_str(), &ip4, nullptr,
        port, 0, 0, static_cast<DWORD>(std::size(keys)), keys, values);
    if (!instance) {
        std::cerr << "Could not construct the mDNS service record.\n";
        return false;
    }

    DNS_SERVICE_REGISTER_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.InterfaceIndex = network.interface_index;
    request.pServiceInstance = instance;
    request.pRegisterCompletionCallback = on_mdns_registration;
    request.unicastEnabled = FALSE; // FALSE explicitly requests mDNS.
    const DWORD status = DnsServiceRegister(&request, nullptr);
    DnsServiceFreeInstance(instance);
    if (status != DNS_REQUEST_PENDING) {
        std::cerr << "mDNS registration could not start (Windows error " << status << ").\n";
        return false;
    }
    return true;
}

static size_t parse_threads(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--threads") {
            const auto value = std::stoul(argv[i + 1]);
            if (value == 0 || value > 1024) throw std::runtime_error("--threads must be between 1 and 1024");
            return static_cast<size_t>(value);
        }
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

static unsigned short parse_port(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") {
            const auto value = std::stoul(argv[i + 1]);
            if (value < 1 || value > 65535) throw std::runtime_error("--port must be between 1 and 65535");
            return static_cast<unsigned short>(value);
        }
    }
    return 50052;
}

static std::string parse_name(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--name") return argv[i + 1];
    }
    return computer_name();
}

static fs::path executable_dir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) throw std::runtime_error("Could not determine executable directory");
    return fs::path(buffer).parent_path();
}

static bool load_backend(const fs::path& directory, const char* dll) {
    const fs::path path = directory / dll;
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
}

#endif

int main(int argc, char** argv) {
    try {
#ifndef _WIN32
        throw std::runtime_error("The PrivateLM desktop worker currently supports Windows only");
#else
        const auto exe_dir = executable_dir();
        const auto app_data = local_app_data() / L"PrivateLM";
        fs::create_directories(app_data);
        const unsigned short port = parse_port(argc, argv);
        const size_t threads = parse_threads(argc, argv);
        const std::string name = parse_name(argc, argv);
        const std::string listen = "0.0.0.0:" + std::to_string(port);

        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("Could not read available system RAM");
        const uint64_t ram_mb = memory.ullAvailPhys / (1024ull * 1024ull);
        const double bandwidth = measure_memory_bandwidth_gbps();
        const std::string id = make_worker_id(app_data);
        const auto addresses = local_ipv4_addresses();
        if (addresses.empty()) throw std::runtime_error("No active IPv4 network connection was found. Connect to Wi-Fi or Ethernet and restart.");

        const auto& network = addresses.front();
        std::cout << "PrivateLM Windows Worker\n"
                  << "=========================\n"
                  << "Worker ID: " << id << "\n"
                  << "Computer: " << name << "\n"
                  << "Available RAM: " << ram_mb << " MiB\n"
                  << "Measured memory bandwidth: " << std::fixed << std::setprecision(1) << bandwidth << " GB/s\n"
                  << "LAN address: " << network.address << "\n"
                  << "RPC port: " << port << "\n\n";

        const fs::path qr_path = write_qr_svg(app_data, network.address, port, name, ram_mb, bandwidth, id);
        std::cout << "Pairing QR: " << qr_path.string() << "\n";
        const auto open_result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr, L"open", qr_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (open_result <= 32) {
            std::cout << "Could not open the QR automatically. Open the SVG file shown above.\n";
        } else {
            std::cout << "Opened the pairing QR automatically.\n";
        }

        std::cout << "\nLoading GGML backends from: " << exe_dir.string() << "\n";
        ggml_backend_dev_t devices[GGML_RPC_MAX_SERVERS] = {};
        size_t device_count = 0;
        const bool cuda_loaded = load_backend(exe_dir, "ggml-cuda.dll");
        if (cuda_loaded) {
            for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
                ggml_backend_dev_t device = ggml_backend_dev_get(i);
                const auto type = ggml_backend_dev_type(device);
                if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
                if (device_count < GGML_RPC_MAX_SERVERS) devices[device_count++] = device;
            }
        }

        if (device_count > 0) {
            std::cout << "Selected CUDA/GPU backend (GPU-first mode).\n";
        } else {
            std::cout << "CUDA unavailable or no CUDA devices detected; falling back to CPU.\n";
            if (!load_backend(exe_dir, "ggml-cpu.dll")) {
                throw std::runtime_error("Neither a usable CUDA GPU nor the CPU backend is available");
            }
            for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
                ggml_backend_dev_t device = ggml_backend_dev_get(i);
                if (ggml_backend_dev_type(device) != GGML_BACKEND_DEVICE_TYPE_CPU) continue;
                if (device_count < GGML_RPC_MAX_SERVERS) devices[device_count++] = device;
            }
        }
        if (device_count == 0) throw std::runtime_error("No usable CUDA or CPU devices were registered");

        for (size_t i = 0; i < device_count; ++i) {
            std::cout << "Serving device: " << ggml_backend_dev_name(devices[i])
                      << " (" << ggml_backend_dev_description(devices[i]) << ")\n";
        }
        if (!load_backend(exe_dir, "ggml-rpc.dll")) throw std::runtime_error("RPC backend is required");

        ggml_backend_reg_t rpc = ggml_backend_reg_by_name("RPC");
        if (!rpc) throw std::runtime_error("RPC backend registry was not registered");
        using start_server_fn = void (*)(const char*, const char*, size_t, size_t, ggml_backend_dev_t*);
        auto start_server = reinterpret_cast<start_server_fn>(
            ggml_backend_reg_get_proc_address(rpc, "ggml_backend_rpc_start_server"));
        if (!start_server) throw std::runtime_error("RPC server entry point is missing");

        const bool mdns = advertise_worker(name, network, port, ram_mb, bandwidth, id);
        if (mdns) {
            std::cout << "mDNS: advertising _privatelm._tcp.local.\n";
        } else {
            std::cout << "mDNS: unavailable; QR/manual pairing is still available.\n";
        }

        const fs::path cache = app_data / L"rpc-cache";
        fs::create_directories(cache);
        std::cout << "\nStarting GGML RPC server on " << listen << "\n"
                  << "Devices: " << device_count << " | CPU threads: " << threads << "\n"
                  << "On your phone: PrivateLM > Phone cluster > Master > Scan worker QR\n"
                  << "Keep this window open while the worker is in use. Press Ctrl+C to stop.\n"
                  << "Security: RPC has no authentication. Use only on a trusted private LAN; do not port-forward it.\n\n";

        start_server(listen.c_str(), cache.string().c_str(), threads, device_count, devices);
        std::cerr << "RPC server returned unexpectedly.\n";
        return 2;
#endif
    } catch (const std::exception& error) {
        std::cerr << "Worker error: " << error.what() << "\n";
        return 1;
    }
}
