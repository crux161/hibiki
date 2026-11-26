#include "em28xx_transport.h"
#include "hibiki/tuner_backend.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

using namespace hibiki;
using namespace hibiki::em28xx;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Step {
    uint8_t type, request;
    uint16_t index;
    std::vector<uint8_t> bytes;
    int result = 999;
};
struct Script : ControlIo {
    std::vector<Step> steps;
    size_t position = 0;
    int transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                 uint8_t* data, uint16_t size, unsigned timeout) override {
        require(position < steps.size(), "Unexpected extra USB operation");
        const auto& s = steps[position++];
        require(type == s.type && request == s.request && value == 0 && index == s.index,
                "USB direction/request/address framing differs from protocol");
        require(size == s.bytes.size() && timeout == 250, "USB length/timeout mismatch");
        int result = s.result == 999 ? size : s.result;
        if ((type & 0x80) && result > 0)
            std::copy_n(s.bytes.data(), std::min<int>(size, result), data);
        else if (!(type & 0x80))
            require(std::equal(s.bytes.begin(), s.bytes.end(), data), "I2C write payload differs");
        return result;
    }
    void done() { require(position == steps.size(), "Expected USB transaction was omitted"); }
};
Step reg(uint16_t address, uint8_t value, int result = 999) { return {0xc0, 0, address, {value}, result}; }
template <typename F> void fails(ErrorKind kind, F operation) {
    try { operation(); }
    catch (const Error& error) { require(error.kind == kind, "Wrong structured error kind"); return; }
    throw std::runtime_error("Expected operation to fail");
}

void repeated_start_eeprom() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40),
                 {0x40, 3, 0xa0, {0, 0}}, reg(0x05, 0),
                 {0xc0, 2, 0xa0, {0x26, 0, 2, 0}}, reg(0x05, 0)};
    Bridge bridge(usb);
    bridge.identify();
    uint8_t offset[2]{}, data[4]{};
    bridge.write_read_i2c(0, 0x50, offset, 2, data, 4);
    require(data[0] == 0x26 && data[2] == 2, "EEPROM identity bytes lost");
    usb.done();
}
void bus_selection_and_restore() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0xc1), {0x40, 0, 0x06, {0xc5}},
                 {0x40, 2, 0xc0, {0x10, 0xff}}, reg(0x05, 0), {0x40, 0, 0x06, {0xc1}}};
    Bridge bridge(usb); bridge.identify();
    const uint8_t data[]{0x10, 0xff};
    bridge.write_i2c(1, 0x60, data, 2); usb.done();
}
void nack_restores_bus_and_preserves_output() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x44), {0x40, 0, 0x06, {0x40}},
                 {0xc0, 2, 0xa0, {0xcc}, 0}, reg(0x05, 0x10), {0x40, 0, 0x06, {0x44}}};
    Bridge bridge(usb); bridge.identify(); uint8_t data = 0xaa;
    fails(ErrorKind::Nack, [&] { bridge.read_i2c(0, 0x50, &data, 1); });
    require(data == 0xaa, "Failed I2C read published invalid data"); usb.done();
}
void short_read_is_not_success() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40), {0xc0, 2, 0xa0, {1, 2}, 1}, reg(0x05, 0)};
    Bridge bridge(usb); bridge.identify(); uint8_t data[]{0xaa, 0xaa};
    fails(ErrorKind::ShortTransfer, [&] { bridge.read_i2c(0, 0x50, data, 2); });
    require(data[0] == 0xaa && data[1] == 0xaa, "Short read modified output"); usb.done();
}
void no_read_after_pointer_nack() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40), {0x40, 3, 0xa0, {0, 0}}, reg(0x05, 0x10)};
    Bridge bridge(usb); bridge.identify(); uint8_t prefix[2]{}, data{};
    fails(ErrorKind::Nack, [&] { bridge.write_read_i2c(0, 0x50, prefix, 2, &data, 1); }); usb.done();
}
void busy_is_bounded() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40), {0x40, 2, 0xa0, {0}}};
    for (int i = 0; i < 8; ++i) usb.steps.push_back(reg(0x05, 0x02));
    Bridge bridge(usb); bridge.identify(); uint8_t data = 0;
    fails(ErrorKind::Timeout, [&] { bridge.write_i2c(0, 0x50, &data, 1); }); usb.done();
}
void busy_then_success() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40), {0x40, 2, 0xa0, {0}}, reg(0x05, 4), reg(0x05, 0)};
    Bridge bridge(usb); bridge.identify(); uint8_t data = 0;
    bridge.write_i2c(0, 0x50, &data, 1); usb.done();
}
void unknown_status_fails() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40), {0xc0, 2, 0xa0, {0}}, reg(0x05, 0x80)};
    Bridge bridge(usb); bridge.identify(); uint8_t data{};
    fails(ErrorKind::Status, [&] { bridge.read_i2c(0, 0x50, &data, 1); }); usb.done();
}
void usb_error_restores_bus() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x44), {0x40, 0, 0x06, {0x40}},
                 {0xc0, 2, 0xa0, {0}, LIBUSB_ERROR_NO_DEVICE}, {0x40, 0, 0x06, {0x44}}};
    Bridge bridge(usb); bridge.identify(); uint8_t data{};
    fails(ErrorKind::Usb, [&] { bridge.read_i2c(0, 0x50, &data, 1); }); usb.done();
}
void restoration_error_is_visible() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x44), {0x40, 0, 0x06, {0x40}},
                 {0xc0, 2, 0xa0, {0}}, reg(0x05, 0), {0x40, 0, 0x06, {0x44}, LIBUSB_ERROR_IO}};
    Bridge bridge(usb); bridge.identify(); uint8_t data = 0xaa;
    fails(ErrorKind::Restore, [&] { bridge.read_i2c(0, 0x50, &data, 1); });
    require(data == 0xaa, "Read result published before bus restoration succeeded"); usb.done();
}
void full_length_transfer() {
    Script usb;
    std::vector<uint8_t> expected(64, 0x47);
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40), {0xc0, 2, 0xa0, expected}, reg(0x05, 0)};
    Bridge bridge(usb); bridge.identify(); uint8_t data[64]{};
    bridge.read_i2c(0, 0x50, data, sizeof(data));
    require(std::equal(expected.begin(), expected.end(), data), "64-byte boundary transfer failed"); usb.done();
}
void short_write_aborts_without_polling() {
    Script usb;
    usb.steps = {reg(0x0a, 0x71), reg(0x06, 0x40), {0x40, 2, 0xa0, {0, 0}, 1}};
    Bridge bridge(usb); bridge.identify(); const uint8_t data[2]{};
    fails(ErrorKind::ShortTransfer, [&] { bridge.write_i2c(0, 0x50, data, 2); }); usb.done();
}
void invalid_input_never_transfers() {
    Script usb; usb.steps = {reg(0x0a, 0x71)};
    Bridge bridge(usb); uint8_t data[65]{};
    fails(ErrorKind::Identity, [&] { bridge.read_i2c(0, 0x50, data, 1); });
    bridge.identify();
    for (size_t size : {size_t(0), size_t(65)})
        fails(ErrorKind::Argument, [&] { bridge.read_i2c(0, 0x50, data, size); });
    fails(ErrorKind::Argument, [&] { bridge.read_i2c(2, 0x50, data, 1); });
    fails(ErrorKind::Argument, [&] { bridge.read_i2c(0, 0xa0, data, 1); });
    fails(ErrorKind::Argument, [&] { bridge.read_i2c(0, 0x07, data, 1); });
    fails(ErrorKind::Argument, [&] { bridge.read_i2c(0, 0x50, nullptr, 1); });
    usb.done();
}
void wrong_chip_and_closed_usb() {
    Script usb; usb.steps = {reg(0x0a, 0x41)};
    Bridge bridge(usb); uint8_t data{};
    fails(ErrorKind::Identity, [&] { bridge.identify(); });
    fails(ErrorKind::Identity, [&] { bridge.read_i2c(0, 0x50, &data, 1); }); usb.done();
    UsbDevice closed;
    fails(ErrorKind::NotOpen, [&] { closed.transfer(0xc0, 0, 0, 10, &data, 1, 250); });
    closed.close(); closed.close();
}

struct RecordingBackend : TunerBackend {
    uint32_t frequency = 0; bool opened = false; int closes = 0, last_length = 0;
    const char* name() const override { return "test-backend"; }
    int Init(const TunerConfig&) override { opened = true; return 0; }
    int OpenTransport(const TunerConfig&) override { return 9; }
    int Close() override { opened = false; ++closes; return 0; }
    int SetPara(uint32_t f) override { frequency = f; return 7; }
    int SetWorkMode(int mode) override { return mode; }
    int IsLock() override { return 1; }
    int GetQuality() override { return 123; }
    int GetStrength() override { return 456; }
    int ReadTS(uint8_t* p, uint32_t n) override { if (n) *p = 0x47; return n ? 1 : 0; }
    int AddPid(int p, int, int) override { return p; }
    int RemovePid(int p, int, int) override { return p; }
    int ResetPid() override { return 5; }
    int PIDOnOff(bool on) override { return on ? 8 : 3; }
    int GetSplitMode() override { return 2; }
    int i2c_read(uint8_t s, uint8_t r, uint8_t* p, int n) override { last_length = n; *p = s ^ r; return 0; }
    int i2c_write(uint8_t, uint8_t, uint8_t*, int n) override { last_length = n; return 4; }
    bool ReadStatus(AtscStatus& s) override { s.snr_db_tenths = 321; return true; }
    bool WaitLock(int ms, AtscStatus* s) override { if (s) s->fec_lock = true; return ms == 12; }
    uint8_t ts_endpoint() const override { return 0x85; }
    bool initialized() const override { return opened; }
};
void facade_preserves_contract() {
    auto backend = std::make_unique<RecordingBackend>(); auto* spy = backend.get();
    Tuner tuner(std::move(backend)); TunerConfig cfg;
    require(tuner.Init(cfg) == 0 && tuner.initialized(), "Init not delegated");
    require(std::string(tuner.backend_name()) == "test-backend", "Backend name lost");
    require(tuner.SetPara(177000000) == 7 && spy->frequency == 177000000, "Tune parameters changed");
    require(tuner.SetWorkMode(4) == 4 && tuner.IsLock() == 1, "Work mode/lock not delegated");
    require(tuner.GetQuality() == 123 && tuner.GetStrength() == 456, "Status units changed");
    uint8_t data{};
    require(tuner.ReadTS(&data, 1) == 1 && data == 0x47 && tuner.ts_endpoint() == 0x85, "TS path changed");
    require(tuner.AddPid(11, 2, 1) == 11 && tuner.RemovePid(12, 3, 0) == 12, "PID delegation failed");
    require(tuner.ResetPid() == 5 && tuner.PIDOnOff(true) == 8 && tuner.GetSplitMode() == 2, "PID flags changed");
    require(tuner.i2c_read(0x50, 3, &data, 1) == 0 && data == 0x53, "I2C arguments changed");
    require(tuner.i2c_write(0x50, 3, &data, 1) == 4 && spy->last_length == 1, "I2C write not delegated");
    AtscStatus status;
    require(tuner.ReadStatus(status) && status.snr_db_tenths == 321, "Shared status changed");
    require(tuner.WaitLock(12, &status) && status.fec_lock, "WaitLock not delegated");
    require(tuner.OpenTransport(cfg) == 9, "Diagnostic open not delegated");
    tuner.Close(); require(!tuner.initialized() && spy->closes == 1, "Close not delegated");
}
void experimental_backend_does_not_claim_tv_readiness() {
    Tuner tuner; TunerConfig cfg;
    require(std::string(tuner.backend_name()) == "fx2-eagle", "Default backend changed");
    cfg.backend = TunerBackendKind::Em28xx;
    require(tuner.Init(cfg) != 0 && !tuner.initialized() && tuner.ts_endpoint() == 0,
            "Transport-only backend claimed TV readiness");
    require(!tuner.last_error().empty(), "Experimental backend failure unexplained");
    require(tuner.SetPara(177000000) != 0 && tuner.IsLock() == 0, "Unimplemented tune/lock succeeded");
    cfg.backend = TunerBackendKind::Eagle;
    require(tuner.OpenTransport(cfg) != 0 && std::string(tuner.backend_name()) == "fx2-eagle",
            "Could not return to original backend without touching USB");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"repeated-start EEPROM", repeated_start_eeprom}, {"bus selection and restore", bus_selection_and_restore},
        {"NACK restoration", nack_restores_bus_and_preserves_output}, {"short read", short_read_is_not_success},
        {"pointer NACK", no_read_after_pointer_nack}, {"bounded polling", busy_is_bounded},
        {"busy recovery", busy_then_success}, {"unknown status", unknown_status_fails},
        {"USB failure", usb_error_restores_bus}, {"restoration failure", restoration_error_is_visible},
        {"64-byte boundary", full_length_transfer}, {"short write", short_write_aborts_without_polling},
        {"input validation", invalid_input_never_transfers}, {"identity and lifecycle", wrong_chip_and_closed_usb},
        {"facade compatibility", facade_preserves_contract}, {"experimental backend", experimental_backend_does_not_claim_tv_readiness}};
    for (const auto& test : tests) {
        try { test.second(); }
        catch (const std::exception& error) { std::cerr << test.first << ": " << error.what() << '\n'; return 1; }
    }
    std::cout << tests.size() << " transport/backend cases passed.\n";
}
