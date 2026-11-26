#pragma once
#include "hibiki/tuner_backend.h"
#include "em28xx_transport.h"
#include <vector>

namespace hibiki {
// The transport is implemented in M1. Init must not imply a working tuner
// before board startup, firmware boot, tuning and TS have been implemented.
class Em28xxBackend final : public TunerBackend {
public:
    ~Em28xxBackend() override { Close(); }
    const char* name() const override { return "em28xx-msb1237-experimental"; }
    std::string last_error() const override { return error_; }
    int Init(const TunerConfig&) override {
        Close();
        error_ = "EM28xx transport is available; MSB1237 board startup/firmware/tuning are not implemented";
        return 1;
    }
    int OpenTransport(const TunerConfig& cfg) override {
        Close(); error_.clear();
        try {
            device_.open({cfg.usb_bus, cfg.usb_address, cfg.detach_kernel_driver});
            bridge_.identify();
            open_ = true;
            return 0;
        } catch (const std::exception& error) {
            error_ = error.what(); Close(); return 1;
        }
    }
    int Close() override { open_ = false; device_.close(); return 0; }
    em28xx::Bridge& bridge() {
        if (!open_) throw em28xx::Error(em28xx::ErrorKind::NotOpen, "EM28xx transport is closed");
        return bridge_;
    }
    int usb_bus() const { return device_.bus(); }
    int usb_address() const { return device_.address(); }
    int SetPara(uint32_t) override { return 1; }
    int SetWorkMode(int) override { return 1; }
    int IsLock() override { return 0; }
    int GetQuality() override { return 0; }
    int GetStrength() override { return 0; }
    int ReadTS(uint8_t*, uint32_t) override { return 0; }
    int AddPid(int, int, int) override { return 1; }
    int RemovePid(int, int, int) override { return 1; }
    int ResetPid() override { return 1; }
    int PIDOnOff(bool) override { return 1; }
    int GetSplitMode() override { return 0; }
    // Shared escape hatch: bus 0, 7-bit slave address, one-byte register prefix.
    int i2c_read(uint8_t slave, uint8_t reg, uint8_t* data, int len) override {
        if (!open_ || len < 1) return 1;
        try {
            bridge_.write_read_i2c(0, slave, &reg, 1, data, static_cast<size_t>(len));
            error_.clear(); return 0;
        } catch (const std::exception& error) { error_ = error.what(); return 1; }
    }
    int i2c_write(uint8_t slave, uint8_t reg, uint8_t* data, int len) override {
        if (!open_ || len < 0 || len > 63 || (len && !data)) return 1;
        std::vector<uint8_t> bytes{reg};
        if (len) bytes.insert(bytes.end(), data, data + len);
        try {
            bridge_.write_i2c(0, slave, bytes.data(), bytes.size());
            error_.clear(); return 0;
        } catch (const std::exception& error) { error_ = error.what(); return 1; }
    }
    bool ReadStatus(AtscStatus& out) override { out = {}; return false; }
    bool WaitLock(int, AtscStatus* out) override { if (out) *out = {}; return false; }
    uint8_t ts_endpoint() const override { return 0; }
    bool initialized() const override { return false; }
private:
    em28xx::UsbDevice device_;
    em28xx::Bridge bridge_{device_};
    bool open_ = false;
    std::string error_;
};
} // namespace hibiki
