#include "em28xx_transport.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>
#include <vector>

namespace hibiki::em28xx {
namespace {
constexpr uint16_t kVid = 0xeb1a, kPid = 0x5902;
constexpr unsigned kTimeoutMs = 250;
void usb_error(int code, const char* operation) {
    throw Error(ErrorKind::Usb, std::string(operation) + ": " + libusb_error_name(code), code);
}
}

void UsbDevice::open(const OpenOptions& options) {
    if (handle_) throw Error(ErrorKind::Argument, "USB device is already open");
    if (options.bus < -1 || options.bus > 255 || options.address < -1 || options.address > 127)
        throw Error(ErrorKind::Argument, "Invalid USB bus/address selector");
    int r = libusb_init(&context_);
    if (r < 0) { context_ = nullptr; usb_error(r, "libusb_init"); }
    libusb_device** devices = nullptr;
    ssize_t count = libusb_get_device_list(context_, &devices);
    if (count < 0) { close(); usb_error(static_cast<int>(count), "USB enumeration"); }
    libusb_device* selected = nullptr;
    int matches = 0;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(devices[i], &desc) != 0 ||
            desc.idVendor != kVid || desc.idProduct != kPid) continue;
        if (options.bus >= 0 && libusb_get_bus_number(devices[i]) != options.bus) continue;
        if (options.address >= 0 && libusb_get_device_address(devices[i]) != options.address) continue;
        selected = devices[i]; ++matches;
    }
    if (matches != 1) {
        libusb_free_device_list(devices, 1); close();
        throw Error(matches ? ErrorKind::Ambiguous : ErrorKind::NotOpen,
                    matches ? "Multiple eb1a:5902 devices; select bus/address" : "No matching eb1a:5902 device");
    }
    bus_ = libusb_get_bus_number(selected);
    address_ = libusb_get_device_address(selected);
    r = libusb_open(selected, &handle_);
    libusb_free_device_list(devices, 1);
    if (r < 0) { close(); usb_error(r, "Open eb1a:5902"); }
    try {
        int config = 0;
        r = libusb_get_configuration(handle_, &config);
        if (r < 0) usb_error(r, "Read USB configuration");
        if (config != 1) throw Error(ErrorKind::Identity, "Expected active USB configuration 1; no configuration changed");
        if (options.detach_kernel_driver) {
            r = libusb_set_auto_detach_kernel_driver(handle_, 1);
            if (r < 0) usb_error(r, "Enable explicit kernel-driver detachment");
        }
        r = libusb_claim_interface(handle_, 0);
        if (r < 0) usb_error(r, "Claim interface 0 (kernel driver left attached by default)");
        claimed_ = true;
    } catch (...) { close(); throw; }
}

void UsbDevice::close() {
    if (handle_) {
        if (claimed_) libusb_release_interface(handle_, 0);
        libusb_close(handle_);
    }
    handle_ = nullptr; claimed_ = false;
    if (context_) libusb_exit(context_);
    context_ = nullptr; bus_ = address_ = -1;
}

int UsbDevice::transfer(uint8_t type, uint8_t request, uint16_t value,
                        uint16_t index, uint8_t* data, uint16_t length, unsigned timeout) {
    if (!handle_) throw Error(ErrorKind::NotOpen, "USB device is closed");
    return libusb_control_transfer(handle_, type, request, value, index, data, length, timeout);
}

void Bridge::exact(uint8_t type, uint8_t request, uint16_t index, uint8_t* data, size_t size) {
    int count = io_.transfer(type, request, 0, index, data, static_cast<uint16_t>(size), kTimeoutMs);
    if (count < 0) usb_error(count, "USB control transfer");
    if (count != static_cast<int>(size))
        throw Error(ErrorKind::ShortTransfer, "USB control transfer returned an incomplete payload", count);
}
uint8_t Bridge::read_reg(uint16_t reg) {
    uint8_t value = 0;
    exact(0xc0, 0, reg, &value, 1);
    return value;
}
void Bridge::write_reg(uint16_t reg, uint8_t value) { exact(0x40, 0, reg, &value, 1); }
uint8_t Bridge::read_register(uint16_t reg) {
    std::lock_guard<std::mutex> lock(mutex_);
    return read_reg(reg);
}
uint8_t Bridge::identify() {
    std::lock_guard<std::mutex> lock(mutex_);
    identified_ = false;
    uint8_t chip = read_reg(0x0a);
    if (chip != 0x71) throw Error(ErrorKind::Identity, "I2C transport requires the observed EM28174 chip ID 0x71", chip);
    identified_ = true;
    return chip;
}
void Bridge::validate(unsigned bus, uint8_t addr, const void* data, size_t size) {
    if (!identified_) throw Error(ErrorKind::Identity, "Identify the EM28174 before I2C operations");
    if (bus > 1 || addr < 0x08 || addr > 0x77 || !data || size < 1 || size > 64)
        throw Error(ErrorKind::Argument, "I2C requires bus 0/1, a non-reserved 7-bit address, and 1..64 bytes");
}
void Bridge::status() {
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint8_t value = read_reg(0x05);
        if (value == 0) return;
        if (value == 0x10) throw Error(ErrorKind::Nack, "I2C slave did not acknowledge", value);
        if (value != 0x02 && value != 0x04)
            throw Error(ErrorKind::Status, "Unknown I2C completion status", value);
        if (attempt != 7) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw Error(ErrorKind::Timeout, "I2C completion polling timed out");
}
void Bridge::transaction(unsigned bus, const std::function<void()>& operation) {
    uint8_t original = read_reg(0x06);
    uint8_t selected = (original & ~0x04) | (bus ? 0x04 : 0);
    std::exception_ptr failure;
    try {
        if (selected != original) write_reg(0x06, selected);
        operation();
    } catch (...) { failure = std::current_exception(); }
    if (selected != original) {
        try { write_reg(0x06, original); }
        catch (const std::exception& error) {
            throw Error(ErrorKind::Restore, std::string("Could not restore I2C bus-selection register: ") + error.what());
        }
    }
    if (failure) std::rethrow_exception(failure);
}
void Bridge::write(uint8_t addr, const uint8_t* data, size_t size, bool stop) {
    std::vector<uint8_t> bytes(data, data + size);
    exact(0x40, stop ? 2 : 3, static_cast<uint16_t>(addr) << 1, bytes.data(), size);
    status();
}
void Bridge::read(uint8_t addr, uint8_t* data, size_t size) {
    std::vector<uint8_t> bytes(size);
    int count = io_.transfer(0xc0, 2, 0, static_cast<uint16_t>(addr) << 1,
                             bytes.data(), static_cast<uint16_t>(size), kTimeoutMs);
    if (count < 0) usb_error(count, "I2C read transfer");
    // Some bridges return zero bytes on an absent slave; check NACK first.
    status();
    if (count != static_cast<int>(size)) throw Error(ErrorKind::ShortTransfer, "Incomplete I2C read", count);
    std::copy(bytes.begin(), bytes.end(), data);
}
void Bridge::write_i2c(unsigned bus, uint8_t addr, const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    validate(bus, addr, data, size);
    transaction(bus, [&] { write(addr, data, size, true); });
}
void Bridge::read_i2c(unsigned bus, uint8_t addr, uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    validate(bus, addr, data, size);
    std::vector<uint8_t> result(size);
    transaction(bus, [&] { read(addr, result.data(), size); });
    std::copy(result.begin(), result.end(), data);
}
void Bridge::write_read_i2c(unsigned bus, uint8_t addr, const uint8_t* prefix,
                           size_t prefix_size, uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    validate(bus, addr, prefix, prefix_size);
    validate(bus, addr, data, size);
    std::vector<uint8_t> result(size);
    transaction(bus, [&] { write(addr, prefix, prefix_size, false); read(addr, result.data(), size); });
    std::copy(result.begin(), result.end(), data);
}

} // namespace hibiki::em28xx
