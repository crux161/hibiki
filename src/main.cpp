// hibiki CLI - drives the hibiki tuner library.
//
// Modes:
//   (default)      boot, tune --freq, serve MPEG-TS over HTTP for VLC
//   --load-only    boot the Eagle firmware and exit
//   --tune-only    boot + tune and exit
//   --scan         sweep ATSC channels on one booted device
//
// VLC: Media > Open Network Stream > http://127.0.0.1:<port>
// (macOS "Open Capture Device" is gated by a signed CoreMediaIO extension.)

#include "hibiki/tuner.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void on_signal(int) { hibiki::g_running = false; }

void msleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void report_status(const hibiki::AtscStatus& st) {
    std::cout << "[*] ATSC snr " << std::fixed << std::setprecision(1)
              << (st.snr_db_tenths / 10.0) << " dB, carrier "
              << std::showpos << st.carrier_offset_hz << std::noshowpos
              << " Hz, timing " << (int)st.timing_offset
              << " | frame=" << st.frame_lock << " atsc=" << st.atsc_lock
              << " fec=" << st.fec_lock << " -> "
              << (st.locked() ? "LOCKED" : "no lock") << "\n";
}

// Tune + wait for lock, printing telemetry like the original CLI.
bool tune_and_lock(hibiki::Tuner& t, uint32_t freq_hz) {
    std::cout << "[*] Starting ATSC carrier acquisition...\n";
    if (t.SetPara(freq_hz) != 0) return false;
    std::cout << "[+] ATSC tune sequence complete\n";
    hibiki::AtscStatus st;
    for (int i = 0; i < 10 && hibiki::g_running; ++i) {
        if (!t.ReadStatus(st)) {
            std::cerr << "[-] ATSC demod status query failed\n";
            return false;
        }
        report_status(st);
        if (st.locked()) return true;
        if (i + 1 < 10) msleep(500);
    }
    std::cerr << "[-] ATSC never locked (no signal on this frequency?)\n";
    return false;
}

// Sweep channels on one booted device (firmware upload paid once).
int scan_channels(hibiki::Tuner& t, int lo, int hi, int dwell_ms) {
    const int attempts = std::max(1, dwell_ms / 200);
    std::cout << "[*] Scanning ATSC channels " << lo << "-" << hi << ", "
              << dwell_ms << " ms dwell each\n";
    int found = 0;
    for (int ch = lo; ch <= hi && hibiki::g_running; ++ch) {
        const uint32_t f = hibiki::atsc_channel_freq_hz(ch);
        if (f == 0) continue;
        if (t.SetPara(f) != 0) {
            std::cerr << "[-] ch " << ch << ": tune failed\n";
            continue;
        }
        hibiki::AtscStatus st;
        bool locked = false;
        for (int i = 0; i < attempts && hibiki::g_running; ++i) {
            if (!t.ReadStatus(st)) break;
            if (st.locked()) { locked = true; break; }
            if (i + 1 < attempts) msleep(200);
        }
        std::cout << (locked ? "[+] ch " : "[ ] ch ") << std::setw(2) << ch
                  << "  " << std::fixed << std::setprecision(3)
                  << std::setw(8) << (f / 1e6) << " MHz"
                  << "  snr " << std::setprecision(1) << std::setw(6)
                  << (st.snr_db_tenths / 10.0) << " dB"
                  << "  carrier " << std::showpos << std::setw(6)
                  << st.carrier_offset_hz << std::noshowpos << " Hz"
                  << "  frame=" << st.frame_lock << " atsc=" << st.atsc_lock
                  << " fec=" << st.fec_lock
                  << (locked ? "   *** LOCKED ***" : "") << "\n";
        if (locked) ++found;
    }
    std::cout << "[+] Scan complete: " << found << " channel(s) locked\n";
    return found;
}

// Drain the library's TS ring to a single client socket. The library's async
// reader keeps the USB endpoint drained independently of this send() loop.
void stream_to_client(hibiki::Tuner& t, int client_fd) {
    std::cout << "[+] client connected, streaming TS from EP 0x"
              << std::hex << (int)t.ts_endpoint() << std::dec << "\n";
    std::vector<uint8_t> buf(65536);
    while (hibiki::g_running) {
        int got = t.ReadTS(buf.data(), (uint32_t)buf.size());
        if (got <= 0) continue;
        int sent_total = 0;
        bool dead = false;
        while (sent_total < got) {
            ssize_t s = ::send(client_fd, buf.data() + sent_total,
                               got - sent_total, 0);
            if (s <= 0) { dead = true; break; }
            sent_total += (int)s;
        }
        if (dead) {
            std::cerr << "[-] client disconnected\n";
            break;
        }
    }
    ::close(client_fd);
}

void serve_http(hibiki::Tuner& t, int port) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) throw std::runtime_error("socket");
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0)
        throw std::runtime_error("bind");
    if (::listen(srv, 1) != 0)
        throw std::runtime_error("listen");

    std::cout << "[+] Hibiki listening on http://127.0.0.1:" << port
              << "  (VLC: Open Network Stream)\n";

    while (hibiki::g_running) {
        // Wait for a connection with a timeout so a SIGINT/SIGTERM (which only
        // clears g_running) is noticed promptly and we shut down gracefully —
        // cancelling the in-flight USB transfers instead of being kill -9'd,
        // which can lock up the FX2 bridge.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        timeval tv{0, 200000}; // 200 ms
        int ready = ::select(srv + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int cfd = ::accept(srv, (sockaddr*)&peer, &plen);
        if (cfd < 0) continue;

        char junk[1024];
        ::recv(cfd, junk, sizeof(junk), MSG_DONTWAIT);

        const char* hdr =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: video/MP2T\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n";
        ::send(cfd, hdr, std::strlen(hdr), 0);

        stream_to_client(t, cfd);
    }
    ::close(srv);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    hibiki::TunerConfig cfg;
    uint32_t freq_hz = 473000000;
    int port = 8080;
    int scan_lo = hibiki::ATSC_CH_MIN, scan_hi = hibiki::ATSC_CH_MAX_POST_REPACK;
    int dwell_ms = 1000;
    enum Stage { STAGE_LOAD_ONLY, STAGE_TUNE_ONLY, STAGE_SCAN, STAGE_FULL };
    Stage stage = STAGE_FULL;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fw" && i + 1 < argc) cfg.fw_path = argv[++i];
        else if (a == "--freq" && i + 1 < argc)
            freq_hz = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--trace") cfg.trace = true;
        else if (a == "--load-only") stage = STAGE_LOAD_ONLY;
        else if (a == "--tune-only") stage = STAGE_TUNE_ONLY;
        else if (a == "--scan") stage = STAGE_SCAN;
        else if (a == "--scan-channels" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d-%d", &scan_lo, &scan_hi) != 2) {
                std::cerr << "--scan-channels expects <lo>-<hi>, e.g. 14-36\n";
                return 2;
            }
        }
        else if (a == "--dwell" && i + 1 < argc) dwell_ms = std::atoi(argv[++i]);
        else if (a == "--skip-soft-reset") cfg.skip_soft_reset = true;
        else if (a == "--skip-pre-fw-config") cfg.skip_pre_fw_config = true;
        else if (a == "--verify-fw") cfg.verify_fw = true;
        else if (a == "--fw-boot-delay" && i + 1 < argc)
            cfg.fw_boot_delay_ms = std::atoi(argv[++i]);
        else if (a == "--rpc-initial-seq" && i + 1 < argc)
            cfg.initial_rpc_seq = std::atoi(argv[++i]);
        else if (a == "--rpc-retries" && i + 1 < argc)
            cfg.rpc_poll_retries = std::atoi(argv[++i]);
        else {
            std::cerr << "usage: " << argv[0]
                      << " [--fw <path>] [--freq <hz>] [--port <n>]"
                         " [--trace] [--load-only|--tune-only|--scan]"
                         " [--scan-channels <lo>-<hi>] [--dwell <ms>]"
                         " [--skip-soft-reset] [--skip-pre-fw-config]"
                         " [--verify-fw]"
                         " [--fw-boot-delay <ms>] [--rpc-initial-seq <n>]"
                         " [--rpc-retries <n>]\n";
            return 2;
        }
    }
    if (cfg.initial_rpc_seq < 0) cfg.initial_rpc_seq = 0;
    if (cfg.initial_rpc_seq > 255) cfg.initial_rpc_seq = 255;
    if (scan_lo < hibiki::ATSC_CH_MIN) scan_lo = hibiki::ATSC_CH_MIN;
    if (scan_hi > hibiki::ATSC_CH_MAX) scan_hi = hibiki::ATSC_CH_MAX;
    if (scan_lo > scan_hi) {
        std::cerr << "--scan-channels range is empty\n";
        return 2;
    }
    if (dwell_ms < 200) dwell_ms = 200;

    try {
        hibiki::Tuner t;
        if (t.Init(cfg) != 0) {
            std::cerr << "Fatal: tuner init failed\n";
            return 1;
        }
        if (stage == STAGE_LOAD_ONLY) { t.Close(); return 0; }
        if (stage == STAGE_SCAN) {
            int n = scan_channels(t, scan_lo, scan_hi, dwell_ms);
            t.Close();
            return n < 0 ? 1 : 0;
        }
        if (!tune_and_lock(t, freq_hz)) {
            std::cerr << "Fatal: ATSC tune failed\n";
            t.Close();
            return 1;
        }
        if (stage == STAGE_TUNE_ONLY) { t.Close(); return 0; }
        serve_http(t, port);
        t.Close();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
