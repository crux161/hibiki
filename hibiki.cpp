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

// MxL internal register map
static constexpr uint32_t MXL_REG_RESET    = 0x80000100; // write 2 before FW load
static constexpr uint32_t MXL_REG_CHIP_ID  = 0x70000188; // sanity read
static constexpr uint32_t MXL_REG_CPU_GO   = 0x70000018; // write 1 to release

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

// Eagle RPC frames are DMA-fed as 32-bit lanes. Keep this isolated to the
// 0xFE/0xFD mailbox path; raw 0xFC firmware memory writes are not swapped.
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
        int r = libusb_bulk_transfer(dev_handle, EP_I2C_OUT, frame.data(),
                                     (int)frame.size(), &actual, 1000);
        if (r != 0 || actual != (int)frame.size()) return false;

        std::vector<uint8_t> tmp(resp_len + 1, 0);
        r = libusb_bulk_transfer(dev_handle, EP_I2C_IN, tmp.data(), (int)tmp.size(),
                                 &actual, 1000);
        if (r != 0 || actual != (int)tmp.size()) return false;
        std::copy(tmp.begin() + 1, tmp.end(), resp);
        return true;
    }

    // Eagle RPC checksum from MxLWare_EAGLE_UTILS_CheckSumCalc:
    // sum big-endian 32-bit protocol words with the checksum field cleared,
    // then XOR with 0xDEADBEEF. Partial trailing words are zero-padded.
    uint32_t eagle_rpc_checksum(std::vector<uint8_t> frame) {
        if (frame.size() < 8) return 0;
        frame[4] = frame[5] = frame[6] = frame[7] = 0;
        while (frame.size() % 4 != 0) frame.push_back(0);

        uint32_t sum = 0;
        for (size_t i = 0; i < frame.size(); i += 4)
            sum += be32_read(frame.data() + i);
        return sum ^ 0xDEADBEEF;
    }

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

        for (int attempt = 0; attempt < MXL_RPC_POLL_RETRIES; ++attempt) {
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

    // Write a single 32-bit value.
    bool mxl_mem_write_u32(uint32_t addr, uint32_t value) {
        uint8_t v[4];
        be32_pack(v, value);
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
        out = be32_read(resp);
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
        if (fw.size() < 0x10 || fw[0] != 'M' || fw[1] != '1') {
            std::cerr << "[-] bad fw header (expected 'M1')\n";
            return false;
        }
        std::cout << "[+] FW loaded " << fw.size() << " bytes\n";

        // 1. Reset MxL CPU
        if (!mxl_mem_write_u32(MXL_REG_RESET, 2)) {
            std::cerr << "[-] reset write failed\n";
            return false;
        }
        msleep(100);

        // 2. Chip-ID sanity read (non-fatal)
        uint32_t chip_id = 0;
        if (mxl_mem_read_u32(MXL_REG_CHIP_ID, chip_id)) {
            std::cout << "[+] MxL chip ID: 0x" << std::hex << chip_id
                      << std::dec << "\n";
        }

        // 3. Walk segments
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
            if (!mxl_mem_write(load_addr, seg_data.data(), seg_data.size())) {
                std::cerr << "[-] seg " << seg << " upload failed\n";
                return false;
            }
            off += 8 + aligned_len;
            seg++;
        }

        // 4. Release CPU
        if (!mxl_mem_write_u32(MXL_REG_CPU_GO, 1)) {
            std::cerr << "[-] CPU release failed\n";
            return false;
        }
        msleep(50);

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

        // CfgDevMpegOutParams: exact A692 MPEG/TS output bytes from libPadTV.
        if (!eagle_send_and_receive(0x01, {
                0x00, 0x01, 0x01, 0x00,
                0x00, 0x01, 0x01, 0x00,
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
        return true;
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
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fw" && i + 1 < argc) fw_path = argv[++i];
        else if (a == "--freq" && i + 1 < argc)
            freq_hz = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else {
            std::cerr << "usage: " << argv[0]
                      << " [--fw <path>] [--freq <hz>] [--port <n>]\n";
            return 2;
        }
    }

    try {
        HibikiTuner t;
        if (!t.eagle_load_fw(fw_path)) {
            std::cerr << "Fatal: FW load failed\n";
            return 1;
        }
        if (!t.tune_atsc_frequency(freq_hz)) {
            std::cerr << "Fatal: ATSC tune failed\n";
            return 1;
        }
        t.serve_http(port);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
