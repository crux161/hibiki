// hibiki - EyeTV Stick (VID 0x1F4D / PID 0xE691) userspace tuner library.
//
// Core driver for the MyGica / Geniatech EyeTV Stick ATSC tuner. The FX2LP
// bridge must already be re-enumerated post 8051 firmware load (see
// wake_tuner.py). This library boots the MxL691 "Eagle" ThreadX firmware over
// the I2C tunnel, tunes ATSC, and streams MPEG-TS.
//
// The public surface mirrors the Android `a692` / `libA692_*` API recovered
// from libPadTV_jni.so (see developers/journal/journal_findings_04/05):
//   Init, Close, SetPara, SetWorkMode, IsLock, GetQuality, GetStrength,
//   ReadTS, AddPid, RemovePid, ResetPid, PIDOnOff, GetSplitMode,
//   i2c_read, i2c_write.
//
// TS integrity: the FX2 commits TS in 2048-byte blocks from a few-KB FIFO.
// The library keeps a queue of async bulk-IN transfers pending at all times
// and buffers them in a ring, so a slow consumer can never stall the endpoint
// and overrun the FIFO (which would slip the 188-byte TS grid).

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <libusb.h>

namespace hibiki {

// Set false to request shutdown (the CLI's signal handler clears it).
extern std::atomic<bool> g_running;

// -------- ATSC channel plan --------
constexpr int ATSC_CH_MIN = 2;
constexpr int ATSC_CH_MAX = 51;
constexpr int ATSC_CH_MAX_POST_REPACK = 36;

// US ATSC DTV centre frequencies (the 28615 Hz term is the standard centre
// offset from the channel edge). Returns 0 for an out-of-plan channel.
uint32_t atsc_channel_freq_hz(int ch);

// ReqAtscDemodStatus (RPC cmd 0x0F) payload, decoded with the field names the
// native driver prints: snrDbTenths / timingOffset / carrierOffsetHz and the
// three lock flags isFrameLock / isAtscLock / isFecLock.
struct AtscStatus {
    int16_t  snr_db_tenths     = 0;
    int16_t  timing_offset     = 0;
    int32_t  carrier_offset_hz = 0;
    bool     frame_lock = false;
    bool     atsc_lock  = false;
    bool     fec_lock   = false;

    // Native declares lock only when all three flags are set (the 0x1f word
    // that libA692_IsLock compares against).
    bool locked() const { return frame_lock && atsc_lock && fec_lock; }
};

// Boot / debug knobs (the former CLI globals). Passed to Init().
struct TunerConfig {
    std::string fw_path = "fw/mbin_mxl_eagle_fw.bin";
    bool trace               = false;
    bool skip_soft_reset     = false;
    bool skip_pre_fw_config  = false;
    bool verify_fw           = false;
    int  fw_boot_delay_ms    = 50;
    int  initial_rpc_seq     = 1;   // CfgDrvInit seeds ctx[0]=1
    int  rpc_poll_retries    = 20;
};

class Tuner {
public:
    Tuner();
    ~Tuner();
    Tuner(const Tuner&) = delete;
    Tuner& operator=(const Tuner&) = delete;

    // ---- Android-parity surface (a692 / libA692_*) ----
    // Open the device, boot the Eagle firmware, apply the attach config trio,
    // issue the FX2 TS-start command and start the TS reader. 0 on success.
    int Init(const TunerConfig& cfg = TunerConfig{});
    // Stop the reader and release the device. 0 on success.
    int Close();
    // Tune to a frequency (native SetPara takes dvb_frontend_parameters whose
    // first field is the frequency). Flushes stale TS. 0 on success.
    int SetPara(uint32_t frequency_hz);
    // Select the work mode (native stores a mode flag; this device is ATSC).
    int SetWorkMode(int mode);
    // 1 when frame+atsc+fec are all locked, else 0.
    int IsLock();
    // Signal quality: SNR in tenths of a dB.
    int GetQuality();
    // Signal strength (this demod reports SNR; see journal 05).
    int GetStrength();
    // Read up to len bytes of MPEG-TS into buf; returns the byte count (0 on
    // shutdown/timeout). Blocks briefly waiting for data.
    int ReadTS(uint8_t* buf, uint32_t len);
    // PID filtering. On the 0xE691 stick these are no-ops returning 0, exactly
    // as in the native driver (AddPid/RemovePid/PIDOnOff gate on device_pid==1,
    // ResetPid is a bare return 0).
    int AddPid(int pid, int type, int onoff);
    int RemovePid(int pid, int type, int onoff);
    int ResetPid();
    int PIDOnOff(bool on);
    // Split mode flag (native reads a global; this device does not split).
    int GetSplitMode();
    // Raw I2C escape hatches over the FX2 tunnel (MxL slave 0x60).
    int i2c_read(uint8_t slave, uint8_t reg, uint8_t* data, int len);
    int i2c_write(uint8_t slave, uint8_t reg, uint8_t* data, int len);

    // ---- hibiki extensions (used by the CLI / front-ends) ----
    // Full demod status. Returns false if the RPC failed.
    bool ReadStatus(AtscStatus& out);
    // Poll until locked or timeout_ms elapses. Returns true if locked.
    bool WaitLock(int timeout_ms, AtscStatus* out = nullptr);
    // The detected TS bulk-IN endpoint.
    uint8_t ts_endpoint() const { return ts_ep_; }
    bool initialized() const { return initialized_; }

private:
    // -- logging --
    void hex_dump(const char* tag, const uint8_t* p, size_t n);
    // -- device / endpoint --
    void discover_ts_endpoint();
    // -- FX2 raw commands --
    bool fx2_cmd_write(const uint8_t* frame, size_t len);
    bool eagle_hw_reset();
    bool fx2_start_ts();
    // -- I2C tunnel --
    bool i2c_tunnel_write(const uint8_t* payload, size_t len);
    bool i2c_tunnel_read(const uint8_t* req, size_t req_len,
                         uint8_t* resp, size_t resp_len);
    // -- Eagle RPC mailbox --
    uint32_t eagle_rpc_checksum(std::vector<uint8_t> frame);
    bool eagle_rpc_checksum_ok(const std::vector<uint8_t>& frame);
    void eagle_transmit_swap(uint8_t cmd, std::vector<uint8_t>& frame);
    void eagle_receive_swap(uint8_t cmd, std::vector<uint8_t>& frame);
    std::vector<uint8_t> eagle_pack_rpc(uint8_t cmd,
                                        const std::vector<uint8_t>& payload,
                                        uint8_t seq);
    bool eagle_read_rpc_response(size_t expected_payload_len,
                                 std::vector<uint8_t>& response);
    bool eagle_send_and_receive(uint8_t cmd, const std::vector<uint8_t>& payload,
                                size_t expected_payload_len = 0,
                                std::vector<uint8_t>* payload_out = nullptr);
    // -- MxL memory / registers --
    bool mxl_mem_write_chunk(uint32_t addr, const uint8_t* data, size_t n);
    bool mxl_mem_write(uint32_t addr, const uint8_t* data, size_t n);
    bool mxl_mem_write_u32(uint32_t addr, uint32_t value);
    bool mxl_mem_read_u32(uint32_t addr, uint32_t& out);
    bool mxl_mem_read(uint32_t addr, uint8_t* data, size_t n);
    bool mxl_update_u32(uint32_t addr, uint32_t clear_mask,
                        uint32_t set_mask, const char* label);
    bool cfg_dev_power_regulators(bool enable);
    bool cfg_dev_xtal_a692();
    bool cfg_dev_loopthrough(bool enable);
    bool cfg_pre_fw_native_a692();
    // -- firmware --
    std::vector<uint8_t> mxl_fw_segment_image(const uint8_t* data, size_t n);
    bool mxl_fw_segment_write(uint32_t addr, const uint8_t* data, size_t n);
    bool mxl_verify_mem_window(uint32_t addr, const uint8_t* expected,
                               size_t n, const std::string& label);
    bool eagle_load_fw(const std::string& path);
    // -- demod / tune --
    bool configure_atsc_demod();
    bool tune_channel(uint32_t freq_hz);
    // -- TS reader (async queue + ring) --
    void start_reader();
    void stop_reader();

    // -- state --
    TunerConfig cfg_;
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* dev_ = nullptr;
    uint8_t ts_ep_ = 0x82;
    uint8_t next_rpc_seq_ = 1;
    std::mutex i2c_mu_;
    bool initialized_ = false;
    int work_mode_ = 0;

    // TS reader internals (defined in tuner.cpp; ring is a private type).
    struct Reader;
    Reader* reader_ = nullptr;
};

} // namespace hibiki
