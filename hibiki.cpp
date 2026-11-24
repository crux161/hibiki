// hibiki - EyeTV Stick (VID 0x1F4D / PID 0xE691) userspace driver
//
// Pipeline:
//   1. Open FX2LP at 0x1F4D:0xE691 (must already be re-enumerated post 8051 fw load)
//   2. I2C-tunnel the MxL691 Eagle ThreadX firmware over bulk EP 0x01
//   3. Release MxL CPU, then stream MPEG-TS from the bulk-in endpoint
//   4. Expose the TS as HTTP on 127.0.0.1:8080 for VLC (Open Network Stream)
//
// NOTE on "Open Capture Device": on macOS, that pathway is gated by a signed
// CoreMediaIO System Extension, which requires an Apple-issued entitlement.
// Use "Open Network Stream" -> http://127.0.0.1:8080 instead.
//
// NOTE: the MxL tuner/demod configuration talks to the Eagle ThreadX firmware
// through its 0xFE/0xFD RPC mailbox. Raw 0xFC memory writes are only used for
// bootstrapping the firmware image.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <libusb.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// -------- Hardware constants (statically reverse-engineered) --------
static constexpr uint16_t VID = 0x1F4D;
static constexpr uint16_t PID = 0xE691;

static constexpr uint8_t EP_I2C_OUT = 0x01;
static constexpr uint8_t EP_I2C_IN  = 0x81;

// TS endpoint is auto-detected from the config descriptor.
// Fallback if enumeration fails.
static constexpr uint8_t EP_TS_FALLBACK = 0x82;

// MxL691 I2C slave address (from the 0x08/0x60/LEN bulk framing).
static constexpr uint8_t MXL_I2C_ADDR = 0x60;

// FX2 native opcode for "write GPIO port" (bypasses the I2C tunnel).
// Frame on the wire is [0x0E, port, value]; an ACK byte is read on EP 0x81.
static constexpr uint8_t OP_FX2_GPIO_WRITE = 0x0E;

// MxL internal register map
static constexpr uint32_t MXL_REG_RESET    = 0x80000100; // write 2 before FW load
static constexpr uint32_t MXL_REG_CHIP_ID  = 0x70000188; // sanity read
// CfgDrvInit(handle, NULL, 3) stores 3 into ctx[8]; CfgDevReset compares the
// chip-ID read against it. mxl692_attach ignores that status, so so do we.
static constexpr uint32_t MXL_EXPECTED_CHIP_ID = 0x3;
static constexpr uint32_t MXL_REG_CPU_GO   = 0x70000018; // write 1 to release
static constexpr uint32_t MXL_REG_TOP_CLK  = 0x70000010;
static constexpr uint32_t MXL_REG_PWR_CTL0 = 0x90000000;
static constexpr uint32_t MXL_REG_PWR_CTL1 = 0x90000018;
static constexpr uint32_t MXL_REG_XTAL_CTL = 0x90000030;
static constexpr uint32_t MXL_REG_RF_CTL   = 0x9001014C;

// Bulk framing opcodes
static constexpr uint8_t OP_I2C_WRITE = 0x08;
static constexpr uint8_t OP_I2C_READ  = 0x09;

// MxL-level opcodes inside the I2C payload
static constexpr uint8_t MXL_OP_MEM_WRITE = 0xFC;
static constexpr uint8_t MXL_OP_MEM_READ  = 0xFB;
static constexpr uint8_t MXL_OP_RPC_WRITE = 0xFE;
static constexpr uint8_t MXL_OP_RPC_READ  = 0xFD;

// MxL mem-write payload is capped at 48 data bytes per transfer.
static constexpr size_t MXL_CHUNK_MAX = 48;

// Eagle firmware RPC frames are capped at 52 bytes: 8-byte header + <=44 bytes.
static constexpr size_t MXL_RPC_FRAME_MAX = 52;
static constexpr int MXL_RPC_POLL_RETRIES = 20;

// -------- globals --------
static std::atomic<bool> g_running{true};

static void on_signal(int) { g_running = false; }

// -------- helpers --------
static void be32_pack(uint8_t* out, uint32_t v) {
    out[0] = (v >> 24) & 0xFF;
    out[1] = (v >> 16) & 0xFF;
    out[2] = (v >> 8)  & 0xFF;
    out[3] = v & 0xFF;
}

static void le32_pack(uint8_t* out, uint32_t v) {
    out[0] = v & 0xFF;
    out[1] = (v >> 8) & 0xFF;
    out[2] = (v >> 16) & 0xFF;
    out[3] = (v >> 24) & 0xFF;
}

static uint32_t be32_read(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

static uint32_t le32_read(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Global trace switch; toggled by --trace.
static bool g_trace = false;
static bool g_skip_soft_reset = false;
static bool g_skip_pre_fw_config = false;
static bool g_verify_fw = false;
static int g_fw_boot_delay_ms = 50;
// CfgDrvInit seeds ctx[0] with 1 and SendAndReceive transmits the pre-increment
// value, so the first RPC on the wire carries seq=1.
static int g_initial_rpc_seq = 1;
static int g_rpc_poll_retries = MXL_RPC_POLL_RETRIES;

static void hex_dump(const char* tag, const uint8_t* p, size_t n) {
    if (!g_trace) return;
    std::cerr << tag << " [" << n << "]";
    for (size_t i = 0; i < n; ++i) {
        if (i % 16 == 0) std::cerr << "\n  ";
        std::cerr << std::hex << std::setw(2) << std::setfill('0')
                  << (int)p[i] << " ";
    }
    std::cerr << std::dec << std::setfill(' ') << "\n";
}

static void hex_dump_always(const char* tag, const uint8_t* p, size_t n) {
    std::cerr << tag << " [" << n << "]";
    for (size_t i = 0; i < n; ++i) {
        if (i % 16 == 0) std::cerr << "\n  ";
        std::cerr << std::hex << std::setw(2) << std::setfill('0')
                  << (int)p[i] << " ";
    }
    std::cerr << std::dec << std::setfill(' ') << "\n";
}

// Eagle RPC frames are DMA-fed as 32-bit lanes. Keep this helper isolated to
// the 0xFE/0xFD mailbox path; firmware segment upload has its own explicit
// DownloadFwSegment byte order.
static void swap_words_be(uint8_t* p, size_t n) {
    for (size_t i = 0; i + 4 <= n; i += 4) {
        std::swap(p[i + 0], p[i + 3]);
        std::swap(p[i + 1], p[i + 2]);
    }
}

static void msleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// -------- HibikiTuner --------
class HibikiTuner {
public:
    HibikiTuner() {
        if (libusb_init(&ctx) != 0) {
            throw std::runtime_error("libusb_init failed");
        }
        dev_handle = libusb_open_device_with_vid_pid(ctx, VID, PID);
        if (!dev_handle) {
            throw std::runtime_error(
                "Tuner not found at 1F4D:E691 "
                "(has the 8051 stage-1 firmware been loaded?)");
        }

        // Detach kernel driver if any (macOS rarely attaches one, but be safe).
        libusb_set_auto_detach_kernel_driver(dev_handle, 1);

        int r = libusb_claim_interface(dev_handle, 0);
        if (r != 0) {
            throw std::runtime_error(std::string("claim_interface: ") +
                                     libusb_error_name(r));
        }

        // The MxL691 needs a short settle window after the FX2 interface is
        // claimed; starting I2C immediately can race silicon power-up.
        msleep(100);

        next_rpc_seq = static_cast<uint8_t>(g_initial_rpc_seq & 0xFF);

        discover_ts_endpoint();
    }

    ~HibikiTuner() {
        if (dev_handle) {
            libusb_release_interface(dev_handle, 0);
            libusb_close(dev_handle);
        }
        if (ctx) libusb_exit(ctx);
    }

    // Walk the active config and pick the first IN endpoint that isn't
    // the I2C response EP. Falls back to EP_TS_FALLBACK if nothing else found.
    void discover_ts_endpoint() {
        ts_ep = EP_TS_FALLBACK;
        libusb_device* dev = libusb_get_device(dev_handle);
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
            std::cerr << "[!] No active config descriptor; using fallback EP 0x"
                      << std::hex << (int)ts_ep << std::dec << "\n";
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
                        ts_ep = addr;
                        std::cout << "[+] TS endpoint auto-detected: 0x"
                                  << std::hex << (int)ts_ep << std::dec
                                  << " (iface " << i << " alt " << a << ")\n";
                        libusb_free_config_descriptor(cfg);
                        return;
                    }
                }
            }
        }
        libusb_free_config_descriptor(cfg);
        std::cerr << "[!] No bulk-IN TS endpoint found; using fallback 0x"
                  << std::hex << (int)ts_ep << std::dec << "\n";
    }

    // Send a raw FX2 bulk command (no I2C tunnel wrapping) and read the
    // 1-byte ACK. Used for FX2 GPIO writes (opcode 0x0E) that drive the
    // MxL691 hardware reset pin before firmware upload.
    bool fx2_cmd_write(const uint8_t* frame, size_t len) {
        std::lock_guard<std::mutex> lk(i2c_mu);
        int actual = 0;
        hex_dump("TX FX2", frame, len);
        int r = libusb_bulk_transfer(dev_handle, EP_I2C_OUT,
                                     const_cast<uint8_t*>(frame),
                                     (int)len, &actual, 1000);
        if (r != 0 || actual != (int)len) {
            std::cerr << "[-] fx2_cmd_write OUT fail: "
                      << libusb_error_name(r) << "\n";
            return false;
        }
        uint8_t ack = 0;
        r = libusb_bulk_transfer(dev_handle, EP_I2C_IN, &ack, 1, &actual, 1000);
        if (r != 0 || actual != 1) {
            std::cerr << "[-] fx2_cmd_write ACK fail: "
                      << libusb_error_name(r) << "\n";
            return false;
        }
        hex_dump("RX ACK", &ack, 1);
        return true;
    }

    // MxL691/Eagle hardware reset via FX2 GPIO. The native mxl692_attach path
    // taken for non-Conexant (VID != 0x572) sticks toggles GPIO port 0x80:
    // write 0, wait 100 ms, write 1, wait 100 ms. This releases the Eagle
    // from hardware reset so firmware download can land.
    bool eagle_hw_reset() {
        const uint8_t lo[3] = {OP_FX2_GPIO_WRITE, 0x80, 0x00};
        const uint8_t hi[3] = {OP_FX2_GPIO_WRITE, 0x80, 0x01};
        std::cout << "[*] Eagle HW reset via FX2 GPIO port 0x80...\n";
        if (!fx2_cmd_write(lo, sizeof(lo))) return false;
        msleep(100);
        if (!fx2_cmd_write(hi, sizeof(hi))) return false;
        msleep(100);
        return true;
    }

    // Raw bulk write to EP_I2C_OUT, then read the 1-byte Cypress completion
    // response. The native A692 helper only checks that one byte arrived; the
    // byte value itself may be an echoed tunnel opcode such as 0x08.
    bool i2c_tunnel_write(const uint8_t* payload, size_t len) {
        std::lock_guard<std::mutex> lk(i2c_mu);
        std::vector<uint8_t> frame;
        frame.reserve(3 + len);
        frame.push_back(OP_I2C_WRITE);
        frame.push_back(MXL_I2C_ADDR);
        frame.push_back(static_cast<uint8_t>(len));
        frame.insert(frame.end(), payload, payload + len);

        int actual = 0;
        hex_dump("TX OUT", frame.data(), frame.size());
        int r = libusb_bulk_transfer(dev_handle, EP_I2C_OUT, frame.data(),
                                     (int)frame.size(), &actual, 1000);
        if (r != 0 || actual != (int)frame.size()) {
            std::cerr << "[-] i2c_tunnel_write OUT fail: "
                      << libusb_error_name(r) << "\n";
            return false;
        }

        uint8_t ack = 0;
        r = libusb_bulk_transfer(dev_handle, EP_I2C_IN, &ack, 1, &actual, 1000);
        if (r != 0 || actual != 1) {
            std::cerr << "[-] i2c_tunnel_write ACK fail: "
                      << libusb_error_name(r) << "\n";
            return false;
        }
        hex_dump("RX ACK", &ack, 1);
        return true;
    }

    // Write an optional MxL read request, then issue the native read tunnel
    // command: [0x09, 0x00, resp_len, slave]. The Cypress response contains
    // one leading status/echo byte; native code discards it before returning
    // the requested data bytes.
    bool i2c_tunnel_read(const uint8_t* req, size_t req_len,
                         uint8_t* resp, size_t resp_len) {
        if (req && req_len != 0 && !i2c_tunnel_write(req, req_len))
            return false;

        std::lock_guard<std::mutex> lk(i2c_mu);
        std::vector<uint8_t> frame;
        frame.reserve(4);
        frame.push_back(OP_I2C_READ);
        frame.push_back(0x00);
        frame.push_back(static_cast<uint8_t>(resp_len));
        frame.push_back(MXL_I2C_ADDR);

        int actual = 0;
        hex_dump("TX RD-REQ", frame.data(), frame.size());
        int r = libusb_bulk_transfer(dev_handle, EP_I2C_OUT, frame.data(),
                                     (int)frame.size(), &actual, 1000);
        if (r != 0 || actual != (int)frame.size()) return false;

        std::vector<uint8_t> tmp(resp_len + 1, 0);
        r = libusb_bulk_transfer(dev_handle, EP_I2C_IN, tmp.data(), (int)tmp.size(),
                                 &actual, 1000);
        if (r != 0 || actual != (int)tmp.size()) return false;
        hex_dump("RX IN", tmp.data(), tmp.size());
        std::copy(tmp.begin() + 1, tmp.end(), resp);
        return true;
    }

    // Eagle RPC checksum from MxLWare_EAGLE_UTILS_CheckSumCalc. The routine is
    // endianness-independent: the BE-host branch sums natively loaded dwords,
    // while the LE-host branch does `rev w10, w10` after the load, so BOTH sum
    // each 32-bit word of the frame interpreted big-endian (byte 0 is the MSB).
    // The byte-sliced carry dance in the loop is just a plain 32-bit running
    // sum. Result is XORed with 0xDEADBEEF; SendAndReceive stores it so that
    // frame bytes 4..7 hold it big-endian. Partial trailing words are
    // zero-padded, matching the memset work buffer native reads from.
    uint32_t eagle_rpc_checksum(std::vector<uint8_t> frame) {
        if (frame.size() < 8) return 0;
        frame[4] = frame[5] = frame[6] = frame[7] = 0;
        while (frame.size() % 4 != 0) frame.push_back(0);

        uint32_t sum = 0;
        for (size_t i = 0; i < frame.size(); i += 4)
            sum += be32_read(frame.data() + i);
        return sum ^ 0xDEADBEEF;
    }

    // Wire layout of checksum field after the post-pack word-swap: bytes 4..7
    // hold the value in big-endian order.
    bool eagle_rpc_checksum_ok(const std::vector<uint8_t>& frame) {
        if (frame.size() < 8) return false;
        uint32_t expected = be32_read(frame.data() + 4);
        return expected == eagle_rpc_checksum(frame);
    }

    // Native MxLWare applies command-specific endian conversion before the
    // checksum. On the little-endian Android build, that conversion reverses
    // selected 16/32-bit payload fields in-place.
    void eagle_transmit_swap(uint8_t cmd, std::vector<uint8_t>& frame) {
        auto reverse_field = [&](size_t off, size_t len) {
            if (off + len <= frame.size())
                std::reverse(frame.begin() + off, frame.begin() + off + len);
        };

        switch (cmd) {
        case 0x05:
        case 0x0A: // CfgTunerChannelTune: uint32_t frequency_hz at payload +0
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

    // MxLWare_EAGLE_ENDIAN_ReceiveSwap is the RX counterpart of TransmitSwap and
    // runs after checksum validation. Its jump table only covers cmds
    // 0x0C..0x26; everything below 0x0C returns untouched. Of the commands
    // Hibiki issues, only 0x0F (ReqAtscDemodStatus) has a case, which reverses
    // the u16 at frame +8, the u16 at +10 and the u32 at +12. Cmds 0x0D and
    // 0x0E map to the plain-return handler, so no-op is correct for them too.
    void eagle_receive_swap(uint8_t cmd, std::vector<uint8_t>& frame) {
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

    std::vector<uint8_t> eagle_pack_rpc(uint8_t cmd,
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

    bool eagle_read_rpc_response(size_t expected_payload_len,
                                 std::vector<uint8_t>& response) {
        size_t frame_len = 8 + expected_payload_len;
        size_t padded_len = (frame_len + 3) & ~size_t(3);
        uint8_t req[2] = {MXL_OP_RPC_READ, 0x00};

        response.assign(padded_len, 0);
        if (!i2c_tunnel_write(req, sizeof(req)))
            return false;

        for (size_t off = 0; off < padded_len; off += 4) {
            if (!i2c_tunnel_read(nullptr, 0, response.data() + off, 4))
                return false;
        }

        swap_words_be(response.data(), response.size());
        response.resize(frame_len);
        return true;
    }

    bool eagle_send_and_receive(uint8_t cmd, const std::vector<uint8_t>& payload,
                                size_t expected_payload_len = 0,
                                std::vector<uint8_t>* payload_out = nullptr) {
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

        uint8_t seq = next_rpc_seq;
        next_rpc_seq++;
        if (next_rpc_seq == 0) next_rpc_seq = 1;

        std::vector<uint8_t> packet = eagle_pack_rpc(cmd, payload, seq);
        if (!i2c_tunnel_write(packet.data(), packet.size())) {
            std::cerr << "[-] Eagle RPC write failed for cmd 0x"
                      << std::hex << (int)cmd << std::dec << "\n";
            return false;
        }

        for (int attempt = 0; attempt < g_rpc_poll_retries; ++attempt) {
            std::vector<uint8_t> rx;
            if (!eagle_read_rpc_response(expected_payload_len, rx)) {
                msleep(5);
                continue;
            }
            if (rx.size() < 8) {
                msleep(5);
                continue;
            }

            uint8_t rx_cmd = rx[0];
            uint8_t rx_seq = rx[1];
            uint8_t rx_len = rx[2];
            uint8_t rx_status = rx[3];
            uint32_t rx_checksum = be32_read(rx.data() + 4);

            // Firmware returns an all-zero-ish frame while the ThreadX task is
            // still working. MxLWare polls past that until a real response.
            if (rx_seq == 0 && rx_checksum == 0) {
                msleep(5);
                continue;
            }

            if (rx_cmd != cmd || rx_seq != seq || rx_len != expected_payload_len) {
                std::cerr << "[-] Eagle RPC response mismatch for cmd 0x"
                          << std::hex << (int)cmd << std::dec
                          << " (got cmd=0x" << std::hex << (int)rx_cmd
                          << " seq=0x" << (int)rx_seq << std::dec
                          << " len=" << (int)rx_len << ")\n";
                return false;
            }
            if (rx_status != 0) {
                std::cerr << "[-] Eagle firmware status 0x"
                          << std::hex << (int)rx_status << std::dec
                          << " for cmd 0x" << std::hex << (int)cmd
                          << std::dec << "\n";
                return false;
            }
            if (!eagle_rpc_checksum_ok(rx)) {
                std::cerr << "[-] Eagle RPC checksum mismatch for cmd 0x"
                          << std::hex << (int)cmd << std::dec << "\n";
                return false;
            }

            eagle_receive_swap(cmd, rx);

            if (payload_out && expected_payload_len != 0) {
                payload_out->assign(rx.begin() + 8, rx.end());
            }
            return true;
        }

        std::cerr << "[-] Eagle RPC timeout for cmd 0x"
                  << std::hex << (int)cmd << std::dec << "\n";
        return false;
    }

    // Write a raw MxL memory block (<=48 bytes). EnhanceI2cMemWrite stores
    // the destination address as a native little-endian uint32_t; callers are
    // responsible for any protocol-specific data byte order.
    bool mxl_mem_write_chunk(uint32_t addr, const uint8_t* data, size_t n) {
        if (n == 0 || n > MXL_CHUNK_MAX) return false;
        std::vector<uint8_t> buf;
        buf.reserve(2 + 4 + n);
        buf.push_back(MXL_OP_MEM_WRITE);
        buf.push_back(static_cast<uint8_t>(n + 4)); // payload length incl. addr
        buf.push_back(addr & 0xFF);
        buf.push_back((addr >> 8) & 0xFF);
        buf.push_back((addr >> 16) & 0xFF);
        buf.push_back((addr >> 24) & 0xFF);
        buf.insert(buf.end(), data, data + n);
        return i2c_tunnel_write(buf.data(), buf.size());
    }

    // Public helper: write an arbitrary-length block, chunking at 48 bytes.
    bool mxl_mem_write(uint32_t addr, const uint8_t* data, size_t n) {
        size_t off = 0;
        while (off < n) {
            size_t step = std::min(MXL_CHUNK_MAX, n - off);
            if (!mxl_mem_write_chunk(addr + off, data + off, step))
                return false;
            off += step;
        }
        return true;
    }

    // Write a single 32-bit register value. Native EnhanceI2cMemWrite stores
    // the value in host-native order (little-endian on ARM64/x86).
    bool mxl_mem_write_u32(uint32_t addr, uint32_t value) {
        uint8_t v[4];
        le32_pack(v, value);
        return mxl_mem_write_chunk(addr, v, 4);
    }

    // Read a single 32-bit value (best-effort, for sanity checks only).
    bool mxl_mem_read_u32(uint32_t addr, uint32_t& out) {
        uint8_t req[6];
        req[0] = MXL_OP_MEM_READ;
        req[1] = 0x04;
        req[2] = addr & 0xFF;
        req[3] = (addr >> 8) & 0xFF;
        req[4] = (addr >> 16) & 0xFF;
        req[5] = (addr >> 24) & 0xFF;
        uint8_t resp[4] = {};
        if (!i2c_tunnel_read(req, sizeof(req), resp, sizeof(resp)))
            return false;
        out = le32_read(resp);
        return true;
    }

    bool mxl_mem_read(uint32_t addr, uint8_t* data, size_t n) {
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
            if (!i2c_tunnel_read(req, sizeof(req), data + off, step))
                return false;
            off += step;
        }
        return true;
    }

    bool mxl_update_u32(uint32_t addr, uint32_t clear_mask,
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

    bool cfg_dev_power_regulators(bool enable) {
        // Native CfgDevPowerRegulators(enable=1): set byte 3 of 0x90000000
        // to 0x10, then assert bit 0x800 at 0x90000018.
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

    bool cfg_dev_xtal_a692() {
        // A692 native attach builds the XTAL parameter block as:
        //   {26, 0, 0, 0, 1}
        // and passes it to CfgDevXtal before firmware download.
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

        if (!mxl_update_u32(MXL_REG_XTAL_CTL, 0x00000032u, 0x00000030u,
                            "xtal mode"))
            return false;
        if (!mxl_update_u32(MXL_REG_TOP_CLK, 0, 0x00000008u,
                            "top clock gate"))
            return false;
        if (!mxl_update_u32(MXL_REG_CPU_GO, 0, 0x00000010u,
                            "cpu clock gate"))
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

    bool cfg_dev_loopthrough(bool enable) {
        return mxl_update_u32(MXL_REG_RF_CTL, enable ? 0u : 0x00002000u,
                              enable ? 0x00002000u : 0u,
                              "loopthrough");
    }

    bool cfg_pre_fw_native_a692() {
        std::cout << "[*] Applying native pre-FW power/XTAL config...\n";
        if (!cfg_dev_power_regulators(true)) return false;
        if (!cfg_dev_xtal_a692()) return false;
        if (!cfg_dev_loopthrough(true)) return false;
        return true;
    }

    std::vector<uint8_t> mxl_fw_segment_image(const uint8_t* data, size_t n) {
        std::vector<uint8_t> swapped(data, data + n);
        for (size_t i = 0; i + 4 <= swapped.size(); i += 4) {
            std::swap(swapped[i + 0], swapped[i + 3]);
            std::swap(swapped[i + 1], swapped[i + 2]);
        }
        return swapped;
    }

    // FW segment payload is stored big-endian per 32-bit word in the M1 file;
    // DownloadFwSegment byte-reverses each word before handing it to I2C.
    bool mxl_fw_segment_write(uint32_t addr, const uint8_t* data, size_t n) {
        if (n % 4 != 0) return false;
        std::vector<uint8_t> swapped = mxl_fw_segment_image(data, n);
        return mxl_mem_write(addr, swapped.data(), swapped.size());
    }

    bool mxl_verify_mem_window(uint32_t addr, const uint8_t* expected,
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

    // Validate the M1 firmware header and walk 'S' segments starting at 0x10.
    // Each 'S' segment layout, as used by DownloadFwSegment:
    //   offset 0:   'S'
    //   offset 1-3: length (BE24)
    //   offset 4-7: load address (BE32)
    //   offset 8..: payload, padded to a 4-byte boundary on the wire
    bool eagle_load_fw(const std::string& path) {
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
        // Native ValidateFwHeader: exact magic bytes 4D 31 10 02 40 00 00 80.
        static constexpr uint8_t MAGIC[8] =
            {0x4D, 0x31, 0x10, 0x02, 0x40, 0x00, 0x00, 0x80};
        if (std::memcmp(fw.data(), MAGIC, sizeof(MAGIC)) != 0) {
            std::cerr << "[-] bad fw magic\n";
            return false;
        }
        // Payload length: BE24 at bytes 8..10, equals fw.size() - 0x10.
        uint32_t hdr_len = ((uint32_t)fw[8] << 16) |
                           ((uint32_t)fw[9] << 8)  |
                           (uint32_t)fw[10];
        if (hdr_len != fw.size() - 0x10) {
            std::cerr << "[-] fw header length mismatch: hdr=" << hdr_len
                      << " file-16=" << (fw.size() - 0x10) << "\n";
            return false;
        }
        std::cout << "[+] FW loaded " << fw.size() << " bytes\n";

        // 0. HW reset via FX2 GPIO — otherwise the Eagle never responds to
        //    I2C tunnel traffic and chip-ID reads come back as zero.
        if (!eagle_hw_reset()) {
            std::cerr << "[-] HW reset failed\n";
            return false;
        }

        // 1. CfgDevReset. MxL_EAGLE_Oem_DevReset is a no-op stub, so the real
        // sequence is sleep(100) *then* write 0x80000100 = 2. Native does not
        // sleep after the write.
        if (g_skip_soft_reset) {
            std::cout << "[*] Skipping MxL soft reset before FW load\n";
        } else {
            msleep(100);
            if (!mxl_mem_write_u32(MXL_REG_RESET, 2)) {
                std::cerr << "[-] reset write failed\n";
                return false;
            }
        }

        // 2. Chip-ID read. CfgDevReset compares this against ctx[8] and returns
        // status 5 on mismatch, but mxl692_attach discards that status.
        uint32_t chip_id = 0;
        if (mxl_mem_read_u32(MXL_REG_CHIP_ID, chip_id)) {
            std::cout << "[+] MxL chip ID: 0x" << std::hex << chip_id
                      << " (native expects 0x" << MXL_EXPECTED_CHIP_ID
                      << std::dec << ")\n";
            if (chip_id != MXL_EXPECTED_CHIP_ID) {
                std::cerr << "[!] chip ID mismatch (native ignores this)\n";
            }
        }

        // 3. Native mxl692_attach configures regulators, XTAL, and RF
        // loopthrough before CfgDevFwDownload. Without this, RAM upload can
        // verify while the Eagle CPU/ThreadX mailbox remains silent.
        if (g_skip_pre_fw_config) {
            std::cout << "[*] Skipping native pre-FW power/XTAL config\n";
        } else if (!cfg_pre_fw_native_a692()) {
            std::cerr << "[-] native pre-FW config failed\n";
            return false;
        }

        // 4. Walk segments
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
                               (uint32_t)fw[off + 2] << 8  |
                               (uint32_t)fw[off + 3];
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
            if (!mxl_fw_segment_write(load_addr, seg_data.data(),
                                      seg_data.size())) {
                std::cerr << "[-] seg " << seg << " upload failed\n";
                return false;
            }
            if (g_verify_fw) {
                std::vector<uint8_t> expected =
                    mxl_fw_segment_image(seg_data.data(), seg_data.size());
                size_t head_len = std::min<size_t>(16, expected.size());
                if (!mxl_verify_mem_window(load_addr, expected.data(), head_len,
                                           "seg " + std::to_string(seg) + " head")) {
                    return false;
                }
                size_t tail_len = std::min<size_t>(16, expected.size());
                uint32_t tail_addr =
                    load_addr + static_cast<uint32_t>(expected.size() - tail_len);
                if (!mxl_verify_mem_window(tail_addr,
                                           expected.data() + expected.size() - tail_len,
                                           tail_len,
                                           "seg " + std::to_string(seg) + " tail")) {
                    return false;
                }
            }
            off += 8 + aligned_len;
            seg++;
        }

        // 5. Release CPU
        if (!mxl_mem_write_u32(MXL_REG_CPU_GO, 1)) {
            std::cerr << "[-] CPU release failed\n";
            return false;
        }
        msleep(g_fw_boot_delay_ms);

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

    bool tune_atsc_frequency(uint32_t freq_hz) {
        std::cout << "[*] Configuring Eagle ATSC demod...\n";

        // CfgDevDemodType: 2 = ATSC/8-VSB
        if (!eagle_send_and_receive(0x00, {0x02})) return false;

        // CfgDevPowerMode: 1 = active
        if (!eagle_send_and_receive(0x02, {0x01})) return false;

        // CfgDevMpegOutParams: mxl692_attach builds this 12-byte blob at
        // runtime by comparing the USB PID (held in a libPadTV global) against
        // the Conexant 0xA682 reference design. On 0xA682 bytes 0 and 7 are
        // zero; on every other device they are 1 and 3. Bytes 1,2,5,6 are 1,
        // bytes 3,4 are 0 and bytes 8..11 are 3. The attach-time and
        // SetPara-time blobs are identical.
        const bool is_a682 = (PID == 0xA682);
        if (!eagle_send_and_receive(0x01, {
                static_cast<uint8_t>(!is_a682), 0x01, 0x01, 0x00,
                0x00, 0x01, 0x01, static_cast<uint8_t>(is_a682 ? 0 : 3),
                0x03, 0x03, 0x03, 0x03
            })) {
            return false;
        }

        std::cout << "[*] Tuning Eagle RF frontend to " << freq_hz << " Hz\n";
        std::vector<uint8_t> tune_payload(6, 0);
        le32_pack(tune_payload.data(), freq_hz);
        tune_payload[4] = 0x00; // bandwidth: A692 wrapper zeroes this field
        tune_payload[5] = 0x00; // tune mode: A692 wrapper zeroes this field
        if (!eagle_send_and_receive(0x0A, tune_payload)) return false;

        // MxLWare waits 200 ms after ChanTune before kicking the demod.
        msleep(200);

        std::cout << "[*] Starting ATSC carrier acquisition...\n";
        if (!eagle_send_and_receive(0x0D, {})) return false;
        if (!eagle_send_and_receive(0x0E, {})) return false;

        std::cout << "[+] ATSC tune sequence complete\n";
        return wait_atsc_lock();
    }

    // ReqAtscDemodStatus (cmd 0x0F) returns an 11-byte status block. The A692
    // SetPara path declares lock only when payload bytes 8, 9 and 10 are all 1;
    // it then ORs 0x1f into the flag word that libA692_IsLock compares to 0x1f.
    // ReceiveSwap only touches payload bytes 0..7 for this command, so the three
    // lock flags are read as-is.
    bool wait_atsc_lock(int attempts = 10, int interval_ms = 500) {
        for (int i = 0; i < attempts && g_running; ++i) {
            std::vector<uint8_t> st;
            if (!eagle_send_and_receive(0x0F, {}, 11, &st) || st.size() < 11) {
                std::cerr << "[-] ATSC demod status query failed\n";
                return false;
            }
            bool locked = (st[8] == 1 && st[9] == 1 && st[10] == 1);
            std::cout << "[*] ATSC status:";
            for (uint8_t b : st)
                std::cout << " " << std::hex << std::setw(2)
                          << std::setfill('0') << (int)b;
            std::cout << std::dec << std::setfill(' ') << " | lock flags "
                      << (int)st[8] << (int)st[9] << (int)st[10] << " -> "
                      << (locked ? "LOCKED" : "no lock") << "\n";
            if (locked) return true;
            msleep(interval_ms);
        }
        std::cerr << "[-] ATSC never locked (no signal on this frequency?)\n";
        return false;
    }

    // Pull bulk-in from the TS endpoint, write to a single client socket.
    void stream_to_client(int client_fd) {
        std::cout << "[+] client connected, streaming TS from EP 0x"
                  << std::hex << (int)ts_ep << std::dec << "\n";

        constexpr int XFER = 16384;
        std::vector<uint8_t> buf(XFER);

        while (g_running) {
            int actual = 0;
            int r = libusb_bulk_transfer(dev_handle, ts_ep, buf.data(),
                                         XFER, &actual, 1000);
            if (r == LIBUSB_ERROR_TIMEOUT) continue;
            if (r != 0) {
                std::cerr << "[-] TS bulk read: " << libusb_error_name(r)
                          << "\n";
                break;
            }
            if (actual <= 0) continue;
            ssize_t sent_total = 0;
            while (sent_total < actual) {
                ssize_t s = ::send(client_fd, buf.data() + sent_total,
                                   actual - sent_total, 0);
                if (s <= 0) { sent_total = -1; break; }
                sent_total += s;
            }
            if (sent_total < 0) {
                std::cerr << "[-] client disconnected\n";
                break;
            }
        }
        ::close(client_fd);
    }

    // Minimal HTTP/1.0 server that emits MPEG-TS forever.
    // VLC: Media > Open Network Stream > http://127.0.0.1:<port>
    void serve_http(int port) {
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

        while (g_running) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int cfd = ::accept(srv, (sockaddr*)&peer, &plen);
            if (cfd < 0) continue;

            // Eat the HTTP request line + headers (best effort).
            char junk[1024];
            ::recv(cfd, junk, sizeof(junk), MSG_DONTWAIT);

            const char* hdr =
                "HTTP/1.0 200 OK\r\n"
                "Content-Type: video/MP2T\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n";
            ::send(cfd, hdr, std::strlen(hdr), 0);

            stream_to_client(cfd);
        }
        ::close(srv);
    }

private:
    libusb_context* ctx = nullptr;
    libusb_device_handle* dev_handle = nullptr;
    uint8_t ts_ep = EP_TS_FALLBACK;
    uint8_t next_rpc_seq = 0;
    std::mutex i2c_mu;
};

// -------- main --------
int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    std::string fw_path = "fw/mbin_mxl_eagle_fw.bin";
    uint32_t freq_hz = 473000000;
    int port = 8080;
    enum Stage { STAGE_LOAD_ONLY, STAGE_TUNE_ONLY, STAGE_FULL };
    Stage stage = STAGE_FULL;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fw" && i + 1 < argc) fw_path = argv[++i];
        else if (a == "--freq" && i + 1 < argc)
            freq_hz = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--trace") g_trace = true;
        else if (a == "--load-only") stage = STAGE_LOAD_ONLY;
        else if (a == "--tune-only") stage = STAGE_TUNE_ONLY;
        else if (a == "--skip-soft-reset") g_skip_soft_reset = true;
        else if (a == "--skip-pre-fw-config") g_skip_pre_fw_config = true;
        else if (a == "--verify-fw") g_verify_fw = true;
        else if (a == "--fw-boot-delay" && i + 1 < argc)
            g_fw_boot_delay_ms = std::atoi(argv[++i]);
        else if (a == "--rpc-initial-seq" && i + 1 < argc)
            g_initial_rpc_seq = std::atoi(argv[++i]);
        else if (a == "--rpc-retries" && i + 1 < argc)
            g_rpc_poll_retries = std::atoi(argv[++i]);
        else {
            std::cerr << "usage: " << argv[0]
                      << " [--fw <path>] [--freq <hz>] [--port <n>]"
                         " [--trace] [--load-only|--tune-only]"
                         " [--skip-soft-reset] [--skip-pre-fw-config]"
                         " [--verify-fw]"
                         " [--fw-boot-delay <ms>] [--rpc-initial-seq <n>]"
                         " [--rpc-retries <n>]\n";
            return 2;
        }
    }
    if (g_fw_boot_delay_ms < 0) g_fw_boot_delay_ms = 0;
    if (g_initial_rpc_seq < 0) g_initial_rpc_seq = 0;
    if (g_initial_rpc_seq > 255) g_initial_rpc_seq = 255;
    if (g_rpc_poll_retries < 1) g_rpc_poll_retries = 1;

    try {
        HibikiTuner t;
        if (!t.eagle_load_fw(fw_path)) {
            std::cerr << "Fatal: FW load failed\n";
            return 1;
        }
        if (stage == STAGE_LOAD_ONLY) return 0;
        if (!t.tune_atsc_frequency(freq_hz)) {
            std::cerr << "Fatal: ATSC tune failed\n";
            return 1;
        }
        if (stage == STAGE_TUNE_ONLY) return 0;
        t.serve_http(port);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
