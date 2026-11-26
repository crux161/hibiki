// hibiki tuner library implementation. See include/hibiki/tuner.h.
//
// The boot / RPC / tune logic is byte-exact against the native Android driver
// (libPadTV_jni.so); see developers/journal/journal_findings_04.md. The TS
// reader (async queue + ring) is the verified fix for the 2048-byte FX2 FIFO
// overrun; see journal_findings_05.md.

#include "eagle_backend.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace hibiki {


// -------- Hardware constants (statically reverse-engineered) --------
namespace {
constexpr uint16_t VID = 0x1F4D;
constexpr uint16_t PID = 0xE691;

constexpr uint8_t EP_I2C_OUT = 0x01;
constexpr uint8_t EP_I2C_IN  = 0x81;
constexpr uint8_t EP_TS_FALLBACK = 0x82;

constexpr uint8_t MXL_I2C_ADDR = 0x60;
constexpr uint8_t OP_FX2_GPIO_WRITE = 0x0E;

constexpr uint32_t MXL_REG_RESET    = 0x80000100;
constexpr uint32_t MXL_REG_CHIP_ID  = 0x70000188;
constexpr uint32_t MXL_EXPECTED_CHIP_ID = 0x3;
constexpr uint32_t MXL_REG_CPU_GO   = 0x70000018;
constexpr uint32_t MXL_REG_TOP_CLK  = 0x70000010;
constexpr uint32_t MXL_REG_PWR_CTL0 = 0x90000000;
constexpr uint32_t MXL_REG_PWR_CTL1 = 0x90000018;
constexpr uint32_t MXL_REG_XTAL_CTL = 0x90000030;
constexpr uint32_t MXL_REG_RF_CTL   = 0x9001014C;

constexpr uint8_t OP_I2C_WRITE = 0x08;
constexpr uint8_t OP_I2C_READ  = 0x09;
constexpr uint8_t MXL_OP_MEM_WRITE = 0xFC;
constexpr uint8_t MXL_OP_MEM_READ  = 0xFB;
constexpr uint8_t MXL_OP_RPC_WRITE = 0xFE;
constexpr uint8_t MXL_OP_RPC_READ  = 0xFD;

constexpr size_t MXL_CHUNK_MAX = 48;
constexpr size_t MXL_RPC_FRAME_MAX = 52;
constexpr int MXL_RPC_POLL_RETRIES = 20;

constexpr int SCAN_POLL_MS = 200;

// -------- helpers --------
void be32_pack(uint8_t* out, uint32_t v) {
    out[0] = (v >> 24) & 0xFF; out[1] = (v >> 16) & 0xFF;
    out[2] = (v >> 8) & 0xFF;  out[3] = v & 0xFF;
}
void le32_pack(uint8_t* out, uint32_t v) {
    out[0] = v & 0xFF;         out[1] = (v >> 8) & 0xFF;
    out[2] = (v >> 16) & 0xFF; out[3] = (v >> 24) & 0xFF;
}
uint32_t be32_read(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}
uint32_t le32_read(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void swap_words_be(uint8_t* p, size_t n) {
    for (size_t i = 0; i + 4 <= n; i += 4) {
        std::swap(p[i + 0], p[i + 3]);
        std::swap(p[i + 1], p[i + 2]);
    }
}
void msleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
void hex_dump_always(const char* tag, const uint8_t* p, size_t n) {
    std::cerr << tag << " [" << n << "]";
    for (size_t i = 0; i < n; ++i) {
        if (i % 16 == 0) std::cerr << "\n  ";
        std::cerr << std::hex << std::setw(2) << std::setfill('0')
                  << (int)p[i] << " ";
    }
    std::cerr << std::dec << std::setfill(' ') << "\n";
}

// -------- TS ring buffer --------
// Producer (the async USB reader) never blocks, so the endpoint is always
// drained and the FX2 FIFO can never overrun. When the ring is full it
// overwrites the OLDEST bytes (a sliding window of the newest data), so the
// buffered content stays a contiguous slice of the stream even if the reader
// runs ahead of any consumer. A consumer that keeps up (ReadTS) keeps the ring
// near-empty, so nothing is overwritten in normal operation.
class TsRing {
public:
    explicit TsRing(size_t cap) : buf_(cap) {}

    bool write(const uint8_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_ || n == 0) return false;
        const size_t cap = buf_.size();
        if (n > cap) { p += n - cap; n = cap; } // keep only the newest cap bytes
        size_t overflow = (count_ + n > cap) ? (count_ + n - cap) : 0;
        if (overflow) { head_ = (head_ + overflow) % cap; count_ -= overflow; }
        size_t pos = (head_ + count_) % cap;
        size_t first = std::min(n, cap - pos);
        std::memcpy(buf_.data() + pos, p, first);
        if (n > first) std::memcpy(buf_.data(), p + first, n - first);
        count_ += n;
        cv_data_.notify_all();
        return true;
    }

    size_t read(uint8_t* p, size_t n) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_data_.wait(lk, [&] { return stopped_ || count_ > 0; });
        if (count_ == 0) return 0;
        return read_locked(p, n);
    }

    size_t read_for(uint8_t* p, size_t n, int ms) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_data_.wait_for(lk, std::chrono::milliseconds(ms),
                          [&] { return stopped_ || count_ > 0; });
        if (count_ == 0) return 0;
        return read_locked(p, n);
    }

    void flush() {
        std::lock_guard<std::mutex> lk(mu_);
        head_ = 0; count_ = 0;
    }
    void stop() {
        std::lock_guard<std::mutex> lk(mu_);
        stopped_ = true;
        cv_data_.notify_all();
    }

private:
    size_t read_locked(uint8_t* p, size_t n) {
        size_t chunk = std::min(count_, n);
        size_t first = std::min(chunk, buf_.size() - head_);
        std::memcpy(p, buf_.data() + head_, first);
        if (chunk > first) std::memcpy(p + first, buf_.data(), chunk - first);
        head_ = (head_ + chunk) % buf_.size();
        count_ -= chunk;
        return chunk;
    }
    std::vector<uint8_t> buf_;
    size_t head_ = 0, count_ = 0;
    bool stopped_ = false;
    std::mutex mu_;
    std::condition_variable cv_data_;
};

// -------- async TS reader --------
constexpr int NUM_XFERS = 8;
constexpr int XFER = 16384;
} // namespace

// Keep NUM_XFERS bulk-IN transfers in flight at all times so the kernel always
// has a read pending; a gap between synchronous reads lets the FX2 FIFO overrun.
struct EagleBackend::Reader {
    TsRing ring{8 * 1024 * 1024};
    std::atomic<bool> failed{false};
    std::atomic<bool> stop{false};
    std::atomic<int> in_flight{0};
    std::thread th;
    libusb_transfer* xf[NUM_XFERS] = {};
    libusb_context* ctx = nullptr;
    libusb_device_handle* dev = nullptr;
    uint8_t ep = 0;

    static void on_xfer(libusb_transfer* t) {
        Reader* r = static_cast<Reader*>(t->user_data);
        bool again = false;
        if (t->status == LIBUSB_TRANSFER_COMPLETED) {
            if (t->actual_length > 0)
                r->ring.write(t->buffer, (size_t)t->actual_length);
            again = true;
        } else if (t->status == LIBUSB_TRANSFER_TIMED_OUT) {
            again = true;
        }
        if (r->stop.load() || r->failed.load()) again = false;
        if (again && libusb_submit_transfer(t) == 0) return;
        if (again) r->failed.store(true); // resubmit failed
        r->in_flight.fetch_sub(1);
    }

    void start(libusb_context* c, libusb_device_handle* d, uint8_t e) {
        ctx = c; dev = d; ep = e;
        th = std::thread([this] {
            for (int i = 0; i < NUM_XFERS; ++i) {
                xf[i] = libusb_alloc_transfer(0);
                libusb_fill_bulk_transfer(xf[i], dev, ep, new uint8_t[XFER],
                                          XFER, on_xfer, this, 1000);
                in_flight.fetch_add(1);
                if (libusb_submit_transfer(xf[i]) != 0) {
                    failed = true;
                    in_flight.fetch_sub(1);
                }
            }
            timeval tv{0, 100000}; // 100 ms
            while (g_running && !failed.load() && !stop.load())
                libusb_handle_events_timeout(ctx, &tv);
            stop = true;
            for (int i = 0; i < NUM_XFERS; ++i)
                libusb_cancel_transfer(xf[i]);
            while (in_flight.load() > 0)
                libusb_handle_events_timeout(ctx, &tv);
            for (int i = 0; i < NUM_XFERS; ++i) {
                delete[] xf[i]->buffer;
                libusb_free_transfer(xf[i]);
                xf[i] = nullptr;
            }
            ring.stop();
        });
    }

    void shutdown() {
        stop = true;
        ring.stop();
        if (th.joinable()) th.join();
    }
};

// -------- Tuner --------
EagleBackend::EagleBackend() = default;

EagleBackend::~EagleBackend() { Close(); }

void EagleBackend::hex_dump(const char* tag, const uint8_t* p, size_t n) {
    if (!cfg_.trace) return;
    hex_dump_always(tag, p, n);
}

int EagleBackend::Init(const TunerConfig& cfg) {
    if (initialized_) return 0;
    cfg_ = cfg;
    if (cfg_.rpc_poll_retries < 1) cfg_.rpc_poll_retries = MXL_RPC_POLL_RETRIES;
    if (cfg_.fw_boot_delay_ms < 0) cfg_.fw_boot_delay_ms = 0;
    next_rpc_seq_ = static_cast<uint8_t>(cfg_.initial_rpc_seq & 0xFF);

    if (libusb_init(&ctx_) != 0) {
        std::cerr << "[-] libusb_init failed\n";
        return 1;
    }
    dev_ = libusb_open_device_with_vid_pid(ctx_, VID, PID);
    if (!dev_) {
        std::cerr << "[-] Tuner not found at 1F4D:E691 "
                     "(has the 8051 stage-1 firmware been loaded?)\n";
        libusb_exit(ctx_); ctx_ = nullptr;
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(dev_, 1);
    int r = libusb_claim_interface(dev_, 0);
    if (r != 0) {
        std::cerr << "[-] claim_interface: " << libusb_error_name(r) << "\n";
        libusb_close(dev_); dev_ = nullptr;
        libusb_exit(ctx_); ctx_ = nullptr;
        return 1;
    }
    // The MxL691 needs a short settle window after the FX2 interface is claimed.
    msleep(100);
    discover_ts_endpoint();

    if (!eagle_load_fw(cfg_.fw_path)) {
        std::cerr << "[-] FW load failed\n";
        Close();
        return 1;
    }
    if (!configure_atsc_demod()) {
        std::cerr << "[-] ATSC demod config failed\n";
        Close();
        return 1;
    }
    start_reader();
    initialized_ = true;
    return 0;
}

int EagleBackend::Close() {
    stop_reader();
    if (dev_) {
        libusb_release_interface(dev_, 0);
        libusb_close(dev_);
        dev_ = nullptr;
    }
    if (ctx_) { libusb_exit(ctx_); ctx_ = nullptr; }
    initialized_ = false;
    return 0;
}

void EagleBackend::start_reader() {
    if (reader_) return;
    reader_ = new Reader();
    reader_->start(ctx_, dev_, ts_ep_);
}

void EagleBackend::stop_reader() {
    if (!reader_) return;
    reader_->shutdown();
    delete reader_;
    reader_ = nullptr;
}

void EagleBackend::discover_ts_endpoint() {
    ts_ep_ = EP_TS_FALLBACK;
    libusb_device* dev = libusb_get_device(dev_);
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        std::cerr << "[!] No active config descriptor; using fallback EP 0x"
                  << std::hex << (int)ts_ep_ << std::dec << "\n";
        return;
    }
    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        const auto& intf = cfg->interface[i];
        for (int a = 0; a < intf.num_altsetting; ++a) {
            const auto& alt = intf.altsetting[a];
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                uint8_t addr = alt.endpoint[e].bEndpointAddress;
                uint8_t attr = alt.endpoint[e].bmAttributes & 0x03;
                bool is_in   = (addr & 0x80) != 0;
                bool is_bulk = (attr == LIBUSB_TRANSFER_TYPE_BULK);
                if (is_in && is_bulk && addr != EP_I2C_IN) {
                    ts_ep_ = addr;
                    std::cout << "[+] TS endpoint auto-detected: 0x"
                              << std::hex << (int)ts_ep_ << std::dec
                              << " (iface " << i << " alt " << a << ")\n";
                    libusb_free_config_descriptor(cfg);
                    return;
                }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    std::cerr << "[!] No bulk-IN TS endpoint found; using fallback 0x"
              << std::hex << (int)ts_ep_ << std::dec << "\n";
}

// ---- FX2 raw commands ----
bool EagleBackend::fx2_cmd_write(const uint8_t* frame, size_t len) {
    std::lock_guard<std::mutex> lk(i2c_mu_);
    int actual = 0;
    hex_dump("TX FX2", frame, len);
    int r = libusb_bulk_transfer(dev_, EP_I2C_OUT,
                                 const_cast<uint8_t*>(frame),
                                 (int)len, &actual, 1000);
    if (r != 0 || actual != (int)len) {
        std::cerr << "[-] fx2_cmd_write OUT fail: " << libusb_error_name(r) << "\n";
        return false;
    }
    uint8_t ack = 0;
    r = libusb_bulk_transfer(dev_, EP_I2C_IN, &ack, 1, &actual, 1000);
    if (r != 0 || actual != 1) {
        std::cerr << "[-] fx2_cmd_write ACK fail: " << libusb_error_name(r) << "\n";
        return false;
    }
    hex_dump("RX ACK", &ack, 1);
    return true;
}

bool EagleBackend::eagle_hw_reset() {
    const uint8_t lo[3] = {OP_FX2_GPIO_WRITE, 0x80, 0x00};
    const uint8_t hi[3] = {OP_FX2_GPIO_WRITE, 0x80, 0x01};
    std::cout << "[*] Eagle HW reset via FX2 GPIO port 0x80...\n";
    if (!fx2_cmd_write(lo, sizeof(lo))) return false;
    msleep(100);
    if (!fx2_cmd_write(hi, sizeof(hi))) return false;
    msleep(100);
    return true;
}

bool EagleBackend::fx2_start_ts() {
    const uint8_t cmd[3] = {0x36, 0x03, 0x00};
    std::lock_guard<std::mutex> lk(i2c_mu_);
    hex_dump("TX TS-START", cmd, sizeof(cmd));
    int actual = 0;
    int r = libusb_bulk_transfer(dev_, EP_I2C_OUT,
                                 const_cast<uint8_t*>(cmd),
                                 (int)sizeof(cmd), &actual, 1000);
    if (r != 0 || actual != (int)sizeof(cmd)) {
        std::cerr << "[-] TS start command failed: " << libusb_error_name(r) << "\n";
        return false;
    }
    std::cout << "[*] Sent FX2 TS-start command [36 03 00]\n";
    return true;
}

// ---- I2C tunnel ----
bool EagleBackend::i2c_tunnel_write(const uint8_t* payload, size_t len) {
    std::lock_guard<std::mutex> lk(i2c_mu_);
    std::vector<uint8_t> frame;
    frame.reserve(3 + len);
    frame.push_back(OP_I2C_WRITE);
    frame.push_back(MXL_I2C_ADDR);
    frame.push_back(static_cast<uint8_t>(len));
    frame.insert(frame.end(), payload, payload + len);

    int actual = 0;
    hex_dump("TX OUT", frame.data(), frame.size());
    int r = libusb_bulk_transfer(dev_, EP_I2C_OUT, frame.data(),
                                 (int)frame.size(), &actual, 1000);
    if (r != 0 || actual != (int)frame.size()) {
        std::cerr << "[-] i2c_tunnel_write OUT fail: " << libusb_error_name(r) << "\n";
        return false;
    }
    uint8_t ack = 0;
    r = libusb_bulk_transfer(dev_, EP_I2C_IN, &ack, 1, &actual, 1000);
    if (r != 0 || actual != 1) {
        std::cerr << "[-] i2c_tunnel_write ACK fail: " << libusb_error_name(r) << "\n";
        return false;
    }
    hex_dump("RX ACK", &ack, 1);
    return true;
}

bool EagleBackend::i2c_tunnel_read(const uint8_t* req, size_t req_len,
                            uint8_t* resp, size_t resp_len) {
    if (req && req_len != 0 && !i2c_tunnel_write(req, req_len))
        return false;
    std::lock_guard<std::mutex> lk(i2c_mu_);
    std::vector<uint8_t> frame;
    frame.reserve(4);
    frame.push_back(OP_I2C_READ);
    frame.push_back(0x00);
    frame.push_back(static_cast<uint8_t>(resp_len));
    frame.push_back(MXL_I2C_ADDR);

    int actual = 0;
    hex_dump("TX RD-REQ", frame.data(), frame.size());
    int r = libusb_bulk_transfer(dev_, EP_I2C_OUT, frame.data(),
                                 (int)frame.size(), &actual, 1000);
    if (r != 0 || actual != (int)frame.size()) return false;

    std::vector<uint8_t> tmp(resp_len + 1, 0);
    r = libusb_bulk_transfer(dev_, EP_I2C_IN, tmp.data(), (int)tmp.size(),
                             &actual, 1000);
    if (r != 0 || actual != (int)tmp.size()) return false;
    hex_dump("RX IN", tmp.data(), tmp.size());
    std::copy(tmp.begin() + 1, tmp.end(), resp);
    return true;
}

// ---- Eagle RPC mailbox ----
uint32_t EagleBackend::eagle_rpc_checksum(std::vector<uint8_t> frame) {
    if (frame.size() < 8) return 0;
    frame[4] = frame[5] = frame[6] = frame[7] = 0;
    while (frame.size() % 4 != 0) frame.push_back(0);
    uint32_t sum = 0;
    for (size_t i = 0; i < frame.size(); i += 4)
        sum += be32_read(frame.data() + i);
    return sum ^ 0xDEADBEEF;
}

bool EagleBackend::eagle_rpc_checksum_ok(const std::vector<uint8_t>& frame) {
    if (frame.size() < 8) return false;
    uint32_t expected = be32_read(frame.data() + 4);
    return expected == eagle_rpc_checksum(frame);
}

void EagleBackend::eagle_transmit_swap(uint8_t cmd, std::vector<uint8_t>& frame) {
    auto reverse_field = [&](size_t off, size_t len) {
        if (off + len <= frame.size())
            std::reverse(frame.begin() + off, frame.begin() + off + len);
    };
    switch (cmd) {
    case 0x05:
    case 0x0A:
    case 0x25:
        reverse_field(8, 4);
        break;
    case 0x13:
        reverse_field(13, 4);
        reverse_field(17, 4);
        break;
    default:
        break;
    }
}

void EagleBackend::eagle_receive_swap(uint8_t cmd, std::vector<uint8_t>& frame) {
    auto reverse_field = [&](size_t off, size_t len) {
        if (off + len <= frame.size())
            std::reverse(frame.begin() + off, frame.begin() + off + len);
    };
    if (cmd == 0x0F) {
        reverse_field(8, 2);
        reverse_field(10, 2);
        reverse_field(12, 4);
    }
}

std::vector<uint8_t> EagleBackend::eagle_pack_rpc(uint8_t cmd,
                                           const std::vector<uint8_t>& payload,
                                           uint8_t seq) {
    std::vector<uint8_t> logical(8 + payload.size(), 0);
    logical[0] = cmd;
    logical[1] = seq;
    logical[2] = static_cast<uint8_t>(payload.size());
    logical[3] = 0x00;
    std::copy(payload.begin(), payload.end(), logical.begin() + 8);
    eagle_transmit_swap(cmd, logical);
    uint32_t checksum = eagle_rpc_checksum(logical);
    be32_pack(logical.data() + 4, checksum);
    while (logical.size() % 4 != 0) logical.push_back(0);
    swap_words_be(logical.data(), logical.size());

    std::vector<uint8_t> packet;
    packet.reserve(2 + logical.size());
    packet.push_back(MXL_OP_RPC_WRITE);
    packet.push_back(static_cast<uint8_t>(logical.size()));
    packet.insert(packet.end(), logical.begin(), logical.end());
    return packet;
}

bool EagleBackend::eagle_read_rpc_response(size_t expected_payload_len,
                                    std::vector<uint8_t>& response) {
    size_t frame_len = 8 + expected_payload_len;
    size_t padded_len = (frame_len + 3) & ~size_t(3);
    uint8_t req[2] = {MXL_OP_RPC_READ, 0x00};
    response.assign(padded_len, 0);
    if (!i2c_tunnel_write(req, sizeof(req))) return false;
    for (size_t off = 0; off < padded_len; off += 4) {
        if (!i2c_tunnel_read(nullptr, 0, response.data() + off, 4))
            return false;
    }
    swap_words_be(response.data(), response.size());
    response.resize(frame_len);
    return true;
}

bool EagleBackend::eagle_send_and_receive(uint8_t cmd,
                                   const std::vector<uint8_t>& payload,
                                   size_t expected_payload_len,
                                   std::vector<uint8_t>* payload_out) {
    if (payload.size() > MXL_RPC_FRAME_MAX - 8) {
        std::cerr << "[-] Eagle RPC payload too large for cmd 0x"
                  << std::hex << (int)cmd << std::dec << "\n";
        return false;
    }
    if (expected_payload_len > MXL_RPC_FRAME_MAX - 8) {
        std::cerr << "[-] Eagle RPC response too large for cmd 0x"
                  << std::hex << (int)cmd << std::dec << "\n";
        return false;
    }
    uint8_t seq = next_rpc_seq_;
    next_rpc_seq_++;
    if (next_rpc_seq_ == 0) next_rpc_seq_ = 1;

    std::vector<uint8_t> packet = eagle_pack_rpc(cmd, payload, seq);
    if (!i2c_tunnel_write(packet.data(), packet.size())) {
        std::cerr << "[-] Eagle RPC write failed for cmd 0x"
                  << std::hex << (int)cmd << std::dec << "\n";
        return false;
    }
    for (int attempt = 0; attempt < cfg_.rpc_poll_retries; ++attempt) {
        std::vector<uint8_t> rx;
        if (!eagle_read_rpc_response(expected_payload_len, rx)) { msleep(5); continue; }
        if (rx.size() < 8) { msleep(5); continue; }
        uint8_t rx_cmd = rx[0], rx_seq = rx[1], rx_len = rx[2], rx_status = rx[3];
        uint32_t rx_checksum = be32_read(rx.data() + 4);
        if (rx_seq == 0 && rx_checksum == 0) { msleep(5); continue; }
        if (rx_cmd != cmd || rx_seq != seq || rx_len != expected_payload_len) {
            std::cerr << "[-] Eagle RPC response mismatch for cmd 0x"
                      << std::hex << (int)cmd << std::dec
                      << " (got cmd=0x" << std::hex << (int)rx_cmd
                      << " seq=0x" << (int)rx_seq << std::dec
                      << " len=" << (int)rx_len << ")\n";
            return false;
        }
        if (rx_status != 0) {
            std::cerr << "[-] Eagle firmware status 0x" << std::hex
                      << (int)rx_status << std::dec << " for cmd 0x"
                      << std::hex << (int)cmd << std::dec << "\n";
            return false;
        }
        if (!eagle_rpc_checksum_ok(rx)) {
            std::cerr << "[-] Eagle RPC checksum mismatch for cmd 0x"
                      << std::hex << (int)cmd << std::dec << "\n";
            return false;
        }
        eagle_receive_swap(cmd, rx);
        if (payload_out && expected_payload_len != 0)
            payload_out->assign(rx.begin() + 8, rx.end());
        return true;
    }
    std::cerr << "[-] Eagle RPC timeout for cmd 0x"
              << std::hex << (int)cmd << std::dec << "\n";
    return false;
}

// ---- MxL memory / registers ----
bool EagleBackend::mxl_mem_write_chunk(uint32_t addr, const uint8_t* data, size_t n) {
    if (n == 0 || n > MXL_CHUNK_MAX) return false;
    std::vector<uint8_t> buf;
    buf.reserve(2 + 4 + n);
    buf.push_back(MXL_OP_MEM_WRITE);
    buf.push_back(static_cast<uint8_t>(n + 4));
    buf.push_back(addr & 0xFF);
    buf.push_back((addr >> 8) & 0xFF);
    buf.push_back((addr >> 16) & 0xFF);
    buf.push_back((addr >> 24) & 0xFF);
    buf.insert(buf.end(), data, data + n);
    return i2c_tunnel_write(buf.data(), buf.size());
}

bool EagleBackend::mxl_mem_write(uint32_t addr, const uint8_t* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t step = std::min(MXL_CHUNK_MAX, n - off);
        if (!mxl_mem_write_chunk(addr + off, data + off, step)) return false;
        off += step;
    }
    return true;
}

bool EagleBackend::mxl_mem_write_u32(uint32_t addr, uint32_t value) {
    uint8_t v[4];
    le32_pack(v, value);
    return mxl_mem_write_chunk(addr, v, 4);
}

bool EagleBackend::mxl_mem_read_u32(uint32_t addr, uint32_t& out) {
    uint8_t req[6];
    req[0] = MXL_OP_MEM_READ;
    req[1] = 0x04;
    req[2] = addr & 0xFF;
    req[3] = (addr >> 8) & 0xFF;
    req[4] = (addr >> 16) & 0xFF;
    req[5] = (addr >> 24) & 0xFF;
    uint8_t resp[4] = {};
    if (!i2c_tunnel_read(req, sizeof(req), resp, sizeof(resp))) return false;
    out = le32_read(resp);
    return true;
}

bool EagleBackend::mxl_mem_read(uint32_t addr, uint8_t* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t step = std::min(MXL_CHUNK_MAX, n - off);
        uint8_t req[6];
        req[0] = MXL_OP_MEM_READ;
        req[1] = 0x04;
        uint32_t a = addr + static_cast<uint32_t>(off);
        req[2] = a & 0xFF;
        req[3] = (a >> 8) & 0xFF;
        req[4] = (a >> 16) & 0xFF;
        req[5] = (a >> 24) & 0xFF;
        if (!i2c_tunnel_read(req, sizeof(req), data + off, step)) return false;
        off += step;
    }
    return true;
}

bool EagleBackend::mxl_update_u32(uint32_t addr, uint32_t clear_mask,
                           uint32_t set_mask, const char* label) {
    uint32_t v = 0;
    if (!mxl_mem_read_u32(addr, v)) {
        std::cerr << "[-] " << label << " read failed at 0x"
                  << std::hex << addr << std::dec << "\n";
        return false;
    }
    v = (v & ~clear_mask) | set_mask;
    if (!mxl_mem_write_u32(addr, v)) {
        std::cerr << "[-] " << label << " write failed at 0x"
                  << std::hex << addr << std::dec << "\n";
        return false;
    }
    return true;
}

bool EagleBackend::cfg_dev_power_regulators(bool enable) {
    uint32_t v = 0;
    if (!mxl_mem_read_u32(MXL_REG_PWR_CTL0, v)) {
        std::cerr << "[-] power regulator read failed\n";
        return false;
    }
    v = (v & 0x00FFFFFFu) | ((enable ? 0x10u : 0x14u) << 24);
    if (!mxl_mem_write_u32(MXL_REG_PWR_CTL0, v)) {
        std::cerr << "[-] power regulator write failed\n";
        return false;
    }
    return mxl_update_u32(MXL_REG_PWR_CTL1, 0, 0x00000800u,
                          "power regulator latch");
}

bool EagleBackend::cfg_dev_xtal_a692() {
    uint32_t v = 0;
    if (!mxl_mem_read_u32(MXL_REG_PWR_CTL0, v)) {
        std::cerr << "[-] xtal control read failed\n";
        return false;
    }
    v &= ~0x0000001Fu;
    v |= 26u;
    v &= ~0x00000100u;
    v &= ~0x00000200u;
    v &= ~0x00010400u;
    if (!mxl_mem_write_u32(MXL_REG_PWR_CTL0, v)) {
        std::cerr << "[-] xtal control write failed\n";
        return false;
    }
    if (!mxl_update_u32(MXL_REG_XTAL_CTL, 0x00000032u, 0x00000030u, "xtal mode"))
        return false;
    if (!mxl_update_u32(MXL_REG_TOP_CLK, 0, 0x00000008u, "top clock gate"))
        return false;
    if (!mxl_update_u32(MXL_REG_CPU_GO, 0, 0x00000010u, "cpu clock gate"))
        return false;
    uint32_t rf = 0;
    if (!mxl_mem_read_u32(MXL_REG_RF_CTL, rf)) {
        std::cerr << "[-] xtal rf toggle read failed\n";
        return false;
    }
    rf &= ~0x00001000u;
    if (!mxl_mem_write_u32(MXL_REG_RF_CTL, rf)) {
        std::cerr << "[-] xtal rf toggle low failed\n";
        return false;
    }
    rf |= 0x00001000u;
    if (!mxl_mem_write_u32(MXL_REG_RF_CTL, rf)) {
        std::cerr << "[-] xtal rf toggle high failed\n";
        return false;
    }
    msleep(50);
    return true;
}

bool EagleBackend::cfg_dev_loopthrough(bool enable) {
    return mxl_update_u32(MXL_REG_RF_CTL, enable ? 0u : 0x00002000u,
                          enable ? 0x00002000u : 0u, "loopthrough");
}

bool EagleBackend::cfg_pre_fw_native_a692() {
    std::cout << "[*] Applying native pre-FW power/XTAL config...\n";
    if (!cfg_dev_power_regulators(true)) return false;
    if (!cfg_dev_xtal_a692()) return false;
    if (!cfg_dev_loopthrough(true)) return false;
    return true;
}

// ---- firmware ----
std::vector<uint8_t> EagleBackend::mxl_fw_segment_image(const uint8_t* data, size_t n) {
    std::vector<uint8_t> swapped(data, data + n);
    for (size_t i = 0; i + 4 <= swapped.size(); i += 4) {
        std::swap(swapped[i + 0], swapped[i + 3]);
        std::swap(swapped[i + 1], swapped[i + 2]);
    }
    return swapped;
}

bool EagleBackend::mxl_fw_segment_write(uint32_t addr, const uint8_t* data, size_t n) {
    if (n % 4 != 0) return false;
    std::vector<uint8_t> swapped = mxl_fw_segment_image(data, n);
    return mxl_mem_write(addr, swapped.data(), swapped.size());
}

bool EagleBackend::mxl_verify_mem_window(uint32_t addr, const uint8_t* expected,
                                  size_t n, const std::string& label) {
    std::vector<uint8_t> actual(n, 0);
    if (!mxl_mem_read(addr, actual.data(), actual.size())) {
        std::cerr << "[-] " << label << " readback failed at 0x"
                  << std::hex << addr << std::dec << "\n";
        return false;
    }
    if (!std::equal(actual.begin(), actual.end(), expected)) {
        std::cerr << "[-] " << label << " readback mismatch at 0x"
                  << std::hex << addr << std::dec << "\n";
        hex_dump_always("expected", expected, n);
        hex_dump_always("actual", actual.data(), actual.size());
        return false;
    }
    std::cout << "[+] " << label << " readback OK at 0x"
              << std::hex << addr << std::dec << "\n";
    return true;
}

bool EagleBackend::eagle_load_fw(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "[-] fw open failed: " << path << "\n";
        return false;
    }
    std::vector<uint8_t> fw((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (fw.size() < 0x10) {
        std::cerr << "[-] fw truncated\n";
        return false;
    }
    static constexpr uint8_t MAGIC[8] =
        {0x4D, 0x31, 0x10, 0x02, 0x40, 0x00, 0x00, 0x80};
    if (std::memcmp(fw.data(), MAGIC, sizeof(MAGIC)) != 0) {
        std::cerr << "[-] bad fw magic\n";
        return false;
    }
    uint32_t hdr_len = ((uint32_t)fw[8] << 16) | ((uint32_t)fw[9] << 8) |
                       (uint32_t)fw[10];
    if (hdr_len != fw.size() - 0x10) {
        std::cerr << "[-] fw header length mismatch: hdr=" << hdr_len
                  << " file-16=" << (fw.size() - 0x10) << "\n";
        return false;
    }
    std::cout << "[+] FW loaded " << fw.size() << " bytes\n";

    if (!eagle_hw_reset()) {
        std::cerr << "[-] HW reset failed\n";
        return false;
    }
    if (cfg_.skip_soft_reset) {
        std::cout << "[*] Skipping MxL soft reset before FW load\n";
    } else {
        msleep(100);
        if (!mxl_mem_write_u32(MXL_REG_RESET, 2)) {
            std::cerr << "[-] reset write failed\n";
            return false;
        }
    }
    uint32_t chip_id = 0;
    if (mxl_mem_read_u32(MXL_REG_CHIP_ID, chip_id)) {
        std::cout << "[+] MxL chip ID: 0x" << std::hex << chip_id
                  << " (native expects 0x" << MXL_EXPECTED_CHIP_ID
                  << std::dec << ")\n";
        if (chip_id != MXL_EXPECTED_CHIP_ID)
            std::cerr << "[!] chip ID mismatch (native ignores this)\n";
    }
    if (cfg_.skip_pre_fw_config) {
        std::cout << "[*] Skipping native pre-FW power/XTAL config\n";
    } else if (!cfg_pre_fw_native_a692()) {
        std::cerr << "[-] native pre-FW config failed\n";
        return false;
    }

    size_t off = 0x10;
    int seg = 0;
    while (off + 8 <= fw.size()) {
        if (fw[off] != 'S') {
            std::cerr << "[-] expected 'S' segment at " << std::hex << off
                      << std::dec << " got 0x" << std::hex << (int)fw[off]
                      << std::dec << "\n";
            break;
        }
        uint32_t seg_len = (uint32_t)fw[off + 1] << 16 |
                           (uint32_t)fw[off + 2] << 8 | (uint32_t)fw[off + 3];
        uint32_t load_addr = be32_read(fw.data() + off + 4);
        size_t aligned_len = (seg_len + 3) & ~size_t(3);
        if (off + 8 + seg_len > fw.size()) {
            std::cerr << "[-] segment " << seg << " overruns fw\n";
            return false;
        }
        std::cout << "[+] seg " << seg << " -> 0x" << std::hex << load_addr
                  << " len " << std::dec << seg_len << "\n";
        std::vector<uint8_t> seg_data(aligned_len, 0);
        std::copy(fw.begin() + off + 8, fw.begin() + off + 8 + seg_len,
                  seg_data.begin());
        if (!mxl_fw_segment_write(load_addr, seg_data.data(), seg_data.size())) {
            std::cerr << "[-] seg " << seg << " upload failed\n";
            return false;
        }
        if (cfg_.verify_fw) {
            std::vector<uint8_t> expected =
                mxl_fw_segment_image(seg_data.data(), seg_data.size());
            size_t head_len = std::min<size_t>(16, expected.size());
            if (!mxl_verify_mem_window(load_addr, expected.data(), head_len,
                                       "seg " + std::to_string(seg) + " head"))
                return false;
            size_t tail_len = std::min<size_t>(16, expected.size());
            uint32_t tail_addr =
                load_addr + static_cast<uint32_t>(expected.size() - tail_len);
            if (!mxl_verify_mem_window(tail_addr,
                                       expected.data() + expected.size() - tail_len,
                                       tail_len,
                                       "seg " + std::to_string(seg) + " tail"))
                return false;
        }
        off += 8 + aligned_len;
        seg++;
    }

    if (!mxl_mem_write_u32(MXL_REG_CPU_GO, 1)) {
        std::cerr << "[-] CPU release failed\n";
        return false;
    }
    msleep(cfg_.fw_boot_delay_ms);

    std::vector<uint8_t> fw_version;
    if (!eagle_send_and_receive(0x08, {}, 4, &fw_version)) {
        std::cerr << "[-] Eagle FW did not answer version handshake\n";
        return false;
    }
    std::cout << "[+] Eagle FW version: ";
    for (uint8_t b : fw_version) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << (int)b << " ";
    }
    std::cout << std::dec << std::setfill(' ') << "\n";
    std::cout << "[+] Eagle FW booted (" << seg << " segments)\n";
    return true;
}

// ---- demod / tune ----
bool EagleBackend::configure_atsc_demod() {
    std::cout << "[*] Configuring Eagle ATSC demod...\n";
    if (!eagle_send_and_receive(0x00, {0x02})) return false; // DemodType: ATSC
    if (!eagle_send_and_receive(0x02, {0x01})) return false; // PowerMode: active
    const bool is_a682 = (PID == 0xA682);
    if (!eagle_send_and_receive(0x01, {
            static_cast<uint8_t>(!is_a682), 0x01, 0x01, 0x00,
            0x00, 0x01, 0x01, static_cast<uint8_t>(is_a682 ? 0 : 3),
            0x03, 0x03, 0x03, 0x03
        })) {
        return false;
    }
    return fx2_start_ts();
}

bool EagleBackend::tune_channel(uint32_t freq_hz) {
    std::vector<uint8_t> tune_payload(6, 0);
    le32_pack(tune_payload.data(), freq_hz);
    tune_payload[4] = 0x00;
    tune_payload[5] = 0x00;
    if (!eagle_send_and_receive(0x0A, tune_payload)) return false;
    msleep(200);
    if (!eagle_send_and_receive(0x0D, {})) return false; // AtscDemodInit
    if (!eagle_send_and_receive(0x0E, {})) return false; // AcquireCarrier
    return true;
}

bool EagleBackend::ReadStatus(AtscStatus& out) {
    std::vector<uint8_t> st;
    if (!eagle_send_and_receive(0x0F, {}, 11, &st) || st.size() < 11)
        return false;
    out.snr_db_tenths     = (int16_t)(uint16_t)(st[0] | (uint16_t)(st[1] << 8));
    out.timing_offset     = (int16_t)(uint16_t)(st[2] | (uint16_t)(st[3] << 8));
    out.carrier_offset_hz = (int32_t)le32_read(st.data() + 4);
    out.frame_lock = (st[8]  == 1);
    out.atsc_lock  = (st[9]  == 1);
    out.fec_lock   = (st[10] == 1);
    return true;
}

bool EagleBackend::WaitLock(int timeout_ms, AtscStatus* out) {
    AtscStatus st;
    int elapsed = 0;
    while (g_running && elapsed < timeout_ms) {
        if (!ReadStatus(st)) return false;
        if (out) *out = st;
        if (st.locked()) return true;
        msleep(SCAN_POLL_MS);
        elapsed += SCAN_POLL_MS;
    }
    if (out) *out = st;
    return st.locked();
}

// ---- Android-parity surface ----
int EagleBackend::SetPara(uint32_t frequency_hz) {
    if (!initialized_) return 1;
    if (reader_) reader_->ring.flush(); // drop stale TS before the new channel
    std::cout << "[*] Tuning Eagle RF frontend to " << frequency_hz << " Hz\n";
    if (!tune_channel(frequency_hz)) return 1;
    return 0;
}

int EagleBackend::SetWorkMode(int mode) {
    work_mode_ = mode;
    return 0;
}

int EagleBackend::IsLock() {
    AtscStatus st;
    if (!ReadStatus(st)) return 0;
    return st.locked() ? 1 : 0;
}

int EagleBackend::GetQuality() {
    AtscStatus st;
    if (!ReadStatus(st)) return 0;
    return st.snr_db_tenths;
}

int EagleBackend::GetStrength() {
    // The Eagle 0x0F status reports SNR; a distinct RF-strength field is not
    // exposed, so strength is reported as SNR (see journal 05).
    AtscStatus st;
    if (!ReadStatus(st)) return 0;
    return st.snr_db_tenths;
}

int EagleBackend::ReadTS(uint8_t* buf, uint32_t len) {
    if (!reader_ || !buf || len == 0) return 0;
    return (int)reader_->ring.read_for(buf, len, 100);
}

int EagleBackend::AddPid(int, int, int) { return 0; }
int EagleBackend::RemovePid(int, int, int) { return 0; }
int EagleBackend::ResetPid() { return 0; }
int EagleBackend::PIDOnOff(bool) { return 0; }
int EagleBackend::GetSplitMode() { return 0; }

int EagleBackend::i2c_read(uint8_t slave, uint8_t reg, uint8_t* data, int len) {
    if (!initialized_ || !data || len <= 0) return 1;
    (void)slave; // tunnel is hardwired to the MxL slave (0x60)
    uint8_t req[1] = {reg};
    return i2c_tunnel_read(req, sizeof(req), data, (size_t)len) ? 0 : 1;
}

int EagleBackend::i2c_write(uint8_t slave, uint8_t reg, uint8_t* data, int len) {
    if (!initialized_ || len < 0) return 1;
    (void)slave;
    std::vector<uint8_t> payload;
    payload.push_back(reg);
    if (data && len > 0) payload.insert(payload.end(), data, data + len);
    return i2c_tunnel_write(payload.data(), payload.size()) ? 0 : 1;
}

} // namespace hibiki
