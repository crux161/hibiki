#pragma once
#include <libusb.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>

namespace hibiki::em28xx {

enum class ErrorKind { Argument, Usb, ShortTransfer, Nack, Timeout, Status,
                       Identity, NotOpen, Ambiguous, Restore };
class Error : public std::runtime_error {
public:
    Error(ErrorKind kind, const std::string& message, int detail = 0)
        : std::runtime_error(message), kind(kind), detail(detail) {}
    ErrorKind kind;
    int detail;
};

// Injectable USB boundary; transfer returns the byte count or a libusb error.
class ControlIo {
public:
    virtual ~ControlIo() = default;
    virtual int transfer(uint8_t type, uint8_t request, uint16_t value,
                         uint16_t index, uint8_t* data, uint16_t length,
                         unsigned timeout_ms) = 0;
};

struct OpenOptions {
    int bus = -1;
    int address = -1;
    bool detach_kernel_driver = false;
};

class UsbDevice final : public ControlIo {
public:
    UsbDevice() = default;
    ~UsbDevice() override { close(); }
    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;
    void open(const OpenOptions& options = {});
    void close();
    int bus() const { return bus_; }
    int address() const { return address_; }
    int transfer(uint8_t, uint8_t, uint16_t, uint16_t, uint8_t*, uint16_t, unsigned) override;
private:
    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool claimed_ = false;
    int bus_ = -1, address_ = -1;
};

// EM28174's EM28xx I2C engine, not the legacy EM2800 or EM25xx bus-B engine.
// Protocol reference: Linux v6.12 drivers/media/usb/em28xx/em28xx-i2c.c.
class Bridge {
public:
    explicit Bridge(ControlIo& io) : io_(io) {}
    uint8_t identify();
    uint8_t read_register(uint16_t reg);
    void write_i2c(unsigned bus, uint8_t address7, const uint8_t* data, size_t size);
    void read_i2c(unsigned bus, uint8_t address7, uint8_t* data, size_t size);
    void write_read_i2c(unsigned bus, uint8_t address7, const uint8_t* prefix,
                       size_t prefix_size, uint8_t* data, size_t size);
private:
    void validate(unsigned bus, uint8_t addr, const void* data, size_t size);
    uint8_t read_reg(uint16_t reg);
    void write_reg(uint16_t reg, uint8_t value);
    void exact(uint8_t type, uint8_t request, uint16_t index, uint8_t* data, size_t size);
    void status();
    void transaction(unsigned bus, const std::function<void()>& operation);
    void write(uint8_t addr, const uint8_t* data, size_t size, bool stop);
    void read(uint8_t addr, uint8_t* data, size_t size);
    ControlIo& io_;
    bool identified_ = false;
    std::mutex mutex_;
};

} // namespace hibiki::em28xx
