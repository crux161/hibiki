// M1 diagnostic: use the production USB/I2C transport, never boot a demodulator.
#include "em28xx_backend.h"
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
std::string json_string(const std::string& input) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
        else out << c;
    }
    out << '"'; return out.str();
}
}

int main(int argc, char** argv) {
    hibiki::em28xx::OpenOptions options;
    bool eeprom = false;
    std::string output;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument("Missing option value");
                return argv[++i];
            };
            if (arg == "--bus" || arg == "--address") {
                std::string text = value(); size_t used = 0;
                int n = std::stoi(text, &used);
                if (used != text.size() || n < 0) throw std::invalid_argument("Invalid USB selector");
                (arg == "--bus" ? options.bus : options.address) = n;
            } else if (arg == "--read-eeprom") eeprom = true;
            else if (arg == "--detach-kernel-driver") options.detach_kernel_driver = true;
            else if (arg == "--output") output = value();
            else if (arg == "--help") {
                std::cout << "usage: hibiki-em28xx-probe [--bus N] [--address N] [--read-eeprom]\n"
                             "       [--detach-kernel-driver] [--output FILE]\n"
                             "Claims interface 0. EEPROM mode reads 16 bytes at bus 0/address 0x50/offset 0.\n"
                             "It writes only the EEPROM address pointer and, if needed, restores bus selection.\n";
                return 0;
            } else throw std::invalid_argument("Unknown option: " + arg);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
    std::ofstream file;
    if (!output.empty()) {
        file.open(output);
        if (!file) { std::cerr << "Cannot open output report\n"; return 2; }
    }
    std::ostream& out = output.empty() ? std::cout : file;
    try {
        hibiki::Em28xxBackend backend;
        hibiki::TunerConfig cfg;
        cfg.backend = hibiki::TunerBackendKind::Em28xx;
        cfg.usb_bus = options.bus; cfg.usb_address = options.address;
        cfg.detach_kernel_driver = options.detach_kernel_driver;
        if (backend.OpenTransport(cfg) != 0) throw std::runtime_error(backend.last_error());
        auto& bridge = backend.bridge();
        uint8_t chip = bridge.read_register(0x0a);
        uint8_t before = bridge.read_register(0x06);
        std::array<uint8_t, 16> first{}, second{};
        if (eeprom) {
            const uint8_t offset[] = {0, 0}; // EM28174 uses 16-bit EEPROM offsets.
            bridge.write_read_i2c(0, 0x50, offset, 2, first.data(), first.size());
            bridge.write_read_i2c(0, 0x50, offset, 2, second.data(), second.size());
            if (first != second) throw std::runtime_error("Repeated EEPROM identity reads differ");
        }
        uint8_t after = bridge.read_register(0x06);
        if (before != after) throw std::runtime_error("I2C selection/clock register was not restored");
        bool signature = first[0] == 0x26 && first[3] == 0;
        out << "{\n  \"target\": \"eb1a:5902\",\n  \"bus\": " << backend.usb_bus()
            << ",\n  \"address\": " << backend.usb_address()
            << ",\n  \"interface_claimed\": 0,\n  \"chip_id\": " << (int)chip
            << ",\n  \"bridge\": \"EM28174\",\n  \"i2c_register_before\": " << (int)before
            << ",\n  \"i2c_register_after\": " << (int)after
            << ",\n  \"eeprom_read\": " << (eeprom ? "true" : "false");
        if (eeprom) {
            std::ostringstream hex;
            for (auto b : first) hex << std::hex << std::setw(2) << std::setfill('0') << (int)b;
            out << ",\n  \"i2c_bus\": 0,\n  \"i2c_address_7bit\": 80,\n  \"eeprom_offset\": 0,"
                   "\n  \"eeprom_hex\": " << json_string(hex.str())
                << ",\n  \"repeat_read_matches\": true,\n  \"known_eeprom_header\": " << (signature ? "true" : "false");
        }
        out << ",\n  \"firmware_uploaded\": false\n}\n";
        return eeprom && !signature ? 1 : 0;
    } catch (const hibiki::em28xx::Error& error) {
        out << "{\"error\": " << json_string(error.what()) << ", \"kind\": "
            << static_cast<int>(error.kind) << ", \"detail\": " << error.detail << "}\n";
        return 1;
    } catch (const std::exception& error) {
        out << "{\"error\": " << json_string(error.what()) << "}\n"; return 1;
    }
}
