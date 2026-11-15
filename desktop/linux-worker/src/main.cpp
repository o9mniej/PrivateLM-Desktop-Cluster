#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <random>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "ggml-backend.h"
#include "ggml-rpc.h"
#include "third_party/qrcodegen.hpp"

namespace fs = std::filesystem;

static pid_t g_mdns_pid = -1;
static void stop_mdns_on_signal(int signal_number) {
    if (g_mdns_pid > 0) kill(g_mdns_pid, SIGTERM);
    _exit(128 + signal_number);
}

struct NetworkAddress {
    std::string address;
    unsigned int interface_index = 0;
    bool is_private = false;
};

static bool is_private_ipv4(const in_addr& address) {
    const auto* b = reinterpret_cast<const unsigned char*>(&address.s_addr);
    return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
        (b[0] == 192 && b[1] == 168);
}

static std::vector<NetworkAddress> local_ipv4_addresses() {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) throw std::runtime_error("Could not enumerate network interfaces");
    std::vector<NetworkAddress> result;
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* sa = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &sa->sin_addr, text, sizeof(text))) continue;
        NetworkAddress entry{text, if_nametoindex(item->ifa_name), is_private_ipv4(sa->sin_addr)};
        if (std::none_of(result.begin(), result.end(), [&](const auto& a) { return a.address == entry.address; }))
            result.push_back(entry);
    }
    freeifaddrs(interfaces);
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.is_private && !b.is_private;
    });
    return result;
}

static uint64_t available_ram_mb() {
    std::ifstream input("/proc/meminfo");
    std::string key, unit;
    uint64_t value = 0;
    while (input >> key >> value >> unit) {
        if (key == "MemAvailable:") return value / 1024;
    }
    throw std::runtime_error("Could not read MemAvailable from /proc/meminfo");
}

static double measure_memory_bandwidth_gbps() {
    constexpr size_t words = 8u * 1024u * 1024u; // 64 MiB
    std::vector<uint64_t> data(words, 1);
    volatile uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 4; ++pass) {
        const volatile uint64_t* values = data.data();
        uint64_t sum = 0;
        for (size_t i = 0; i < words; ++i) sum += values[i];
        checksum = checksum + sum;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (checksum == 0 || seconds <= 0) return 0;
    return (static_cast<double>(data.size() * sizeof(uint64_t) * 4) / seconds) / 1'000'000'000.0;
}

static std::string hostname() {
    std::array<char, 256> value{};
    if (gethostname(value.data(), value.size() - 1) != 0) return "Linux-PC";
    return value.data();
}

static fs::path data_directory() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    fs::path root;
    if (xdg && *xdg) root = xdg;
    else if (home && *home) root = fs::path(home) / ".local" / "share";
    else throw std::runtime_error("Neither XDG_DATA_HOME nor HOME is set");
    return root / "PrivateLM";
}

static std::string make_worker_id(const fs::path& root) {
    fs::create_directories(root);
    const fs::path path = root / "worker-id";
    {
        std::ifstream in(path);
        std::string old;
        if (in && std::getline(in, old) && !old.empty()) return old;
    }
    std::random_device random;
    std::ostringstream id;
    id << std::hex << std::uppercase << std::setfill('0');
    for (int i = 0; i < 4; ++i) id << std::setw(8) << static_cast<uint32_t>(random());
    const std::string value = id.str();
    std::ofstream out(path, std::ios::trunc);
    if (!out) throw std::runtime_error("Could not persist worker ID at " + path.string());
    out << value << '\n';
    return value;
}

static std::string url_encode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out << static_cast<char>(c);
        else out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return out.str();
}

static fs::path write_qr_svg(const fs::path& root, const std::string& host, unsigned short port,
    const std::string& name, uint64_t ram_mb, double bandwidth, const std::string& id) {
    std::ostringstream uri;
    uri << "privatelm://worker?host=" << host << "&port=" << port
        << "&name=" << url_encode(name) << "&ram=" << ram_mb
        << "&bw=" << std::fixed << std::setprecision(1) << bandwidth
        << "&backend=cpu&vram=0&id=" << url_encode(id);
    const auto qr = qrcodegen::QrCode::encodeText(uri.str().c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int quiet = 4;
    const int size = qr.getSize() + quiet * 2;
    const fs::path path = root / ("worker-qr-" + host + ".svg");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Could not write QR SVG: " + path.string());
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << size << " " << size
        << "\" shape-rendering=\"crispEdges\"><rect width=\"100%\" height=\"100%\" fill=\"white\"/>"
        << "<path fill=\"black\" d=\"";
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y)) out << "M" << x + quiet << "," << y + quiet << "h1v1h-1z";
    out << "\"/></svg>\n";
    if (!out) throw std::runtime_error("Failed while writing QR SVG");
    return path;
}

static void open_file(const fs::path& path) {
    const pid_t child = fork();
    if (child == 0) {
        execlp("xdg-open", "xdg-open", path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) std::cerr << "Could not start xdg-open; open the QR SVG manually.\n";
    // Do not wait: xdg-open may remain alive for the lifetime of the browser.
}

static pid_t start_mdns(const std::string& name, unsigned short port, uint64_t ram_mb,
    double bandwidth, const std::string& id) {
    const pid_t child = fork();
    if (child == 0) {
        const std::string service = "PrivateLM " + name;
        const std::string port_text = std::to_string(port);
        const std::string ram = "ram=" + std::to_string(ram_mb);
        std::ostringstream bw;
        bw << "bw=" << std::fixed << std::setprecision(1) << bandwidth;
        const std::string model = "model=" + name;
        const std::string worker_id = "id=" + id;
        execlp("avahi-publish-service", "avahi-publish-service",
            service.c_str(), "_privatelm._tcp", port_text.c_str(),
            ram.c_str(), bw.str().c_str(), model.c_str(), "v=rpc7",
            worker_id.c_str(), "backend=cpu", "vram=0", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) {
        std::cerr << "mDNS: could not start Avahi publisher; QR/manual pairing remains available.\n";
        return;
    }
    std::cout << "mDNS: starting Avahi service advertisement (if avahi-publish-service is installed).\n";
}

static size_t parse_threads(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--threads") {
        const auto n = std::stoul(argv[i + 1]);
        if (n == 0 || n > 1024) throw std::runtime_error("--threads must be between 1 and 1024");
        return static_cast<size_t>(n);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int count = CPU_COUNT(&set);
        if (count > 0) return static_cast<size_t>(count);
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

static unsigned short parse_port(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--port") {
        const auto n = std::stoul(argv[i + 1]);
        if (n < 1 || n > 65535) throw std::runtime_error("--port must be between 1 and 65535");
        return static_cast<unsigned short>(n);
    }
    return 50052;
}

static std::string parse_name(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--name") return argv[i + 1];
    return hostname();
}

static bool load_backend(const fs::path& directory, const char* filename) {
    const fs::path path = directory / filename;
    if (!fs::exists(path)) {
        std::cerr << "Backend not found: " << path << '\n';
        return false;
    }
    if (!ggml_backend_load(path.c_str())) {
        std::cerr << "Could not load backend: " << path << '\n';
        return false;
    }
    std::cout << "Loaded " << filename << '\n';
    return true;
}

int main(int argc, char** argv) {
    try {
        const fs::path exe_dir = fs::canonical("/proc/self/exe").parent_path();
        const fs::path app_data = data_directory();
        fs::create_directories(app_data);
        const unsigned short port = parse_port(argc, argv);
        const size_t threads = parse_threads(argc, argv);
        const std::string name = parse_name(argc, argv);
        const uint64_t ram_mb = available_ram_mb();
        const double bandwidth = measure_memory_bandwidth_gbps();
        const std::string id = make_worker_id(app_data);
        const auto addresses = local_ipv4_addresses();
        if (addresses.empty()) throw std::runtime_error("No active LAN IPv4 address found; connect to Wi-Fi or Ethernet and restart");
        const auto& network = addresses.front();

        std::cout << "PrivateLM Linux Worker\n=======================\n"
                  << "Worker ID: " << id << "\nComputer: " << name
                  << "\nAvailable RAM: " << ram_mb << " MiB"
                  << "\nMeasured memory bandwidth: " << std::fixed << std::setprecision(1) << bandwidth
                  << " GB/s\nLAN address: " << network.address << "\nRPC port: " << port << "\n";

        std::cout << "\nLoading GGML backends from: " << exe_dir << '\n';
        if (!load_backend(exe_dir, "libggml-cpu.so"))
            throw std::runtime_error("CPU backend could not be loaded");
        ggml_backend_dev_t devices[GGML_RPC_MAX_SERVERS] = {};
        size_t count = 0;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            auto device = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(device) == GGML_BACKEND_DEVICE_TYPE_CPU && count < GGML_RPC_MAX_SERVERS)
                devices[count++] = device;
        }
        if (count == 0) throw std::runtime_error("GGML did not register a CPU device");
        for (size_t i = 0; i < count; ++i)
            std::cout << "Serving device: " << ggml_backend_dev_name(devices[i])
                      << " (" << ggml_backend_dev_description(devices[i]) << ")\n";
        if (!load_backend(exe_dir, "libggml-rpc.so"))
            throw std::runtime_error("RPC backend is required");

        ggml_backend_reg_t rpc = ggml_backend_reg_by_name("RPC");
        if (!rpc) throw std::runtime_error("RPC backend registry was not registered");
        using start_server_fn = void (*)(const char*, const char*, size_t, size_t, ggml_backend_dev_t*);
        auto start_server = reinterpret_cast<start_server_fn>(
            ggml_backend_reg_get_proc_address(rpc, "ggml_backend_rpc_start_server"));
        if (!start_server) throw std::runtime_error("RPC server entry point is missing");

        const fs::path qr = write_qr_svg(app_data, network.address, port, name, ram_mb, bandwidth, id);
        std::cout << "Pairing QR: " << qr << '\n';
        open_file(qr);
        std::signal(SIGINT, stop_mdns_on_signal);
        std::signal(SIGTERM, stop_mdns_on_signal);
        g_mdns_pid = start_mdns(name, port, ram_mb, bandwidth, id);

        const fs::path cache = app_data / "rpc-cache";
        fs::create_directories(cache);
        const std::string listen = "0.0.0.0:" + std::to_string(port);
        std::cout << "\nStarting GGML RPC server on " << listen
                  << "\nDevices: " << count << " | CPU threads: " << threads
                  << "\nOn your phone: PrivateLM > Phone cluster > Master > Scan worker QR"
                  << "\nKeep this process running while the worker is in use. Press Ctrl+C to stop."
                  << "\nSecurity: RPC has no authentication. Use only on a trusted private LAN; do not expose port "
                  << port << " to the public internet.\n\n";
        start_server(listen.c_str(), cache.c_str(), threads, count, devices);
        std::cerr << "RPC server returned unexpectedly.\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "Worker error: " << e.what() << '\n';
        return 1;
    }
}
