#include "hibiki/firmware.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
extern "C" {
#include <libavutil/base64.h>
}

namespace fs = std::filesystem;
using namespace hibiki::firmware;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void write(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary); out << data;
}
std::string contents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char** argv) {
    char name[] = "/tmp/hibiki-firmware-tests-XXXXXX";
    auto* temporary = mkdtemp(name);
    if (!temporary) return 1;
    fs::path root = temporary;
    try {
        std::string message;
        auto dest = root / "installed";
        const auto& eagle = specs[0];
        require(!verify(root / "missing.bin", eagle).verified, "Missing file accepted");
        write(root / "wrong.bin", std::string(eagle.size, 'x'));
        require(!verify(root / "wrong.bin", eagle).verified, "Wrong hash accepted");
        require(!install({root / "wrong.bin"}, dest, message), "Unknown raw bytes installed");
        require(!fs::exists(dest), "Invalid source created destination");
        write(root / "bad.b64", "AA=A");
        require(!install({root / "bad.b64"}, dest, message), "Invalid base64 padding accepted");
        write(root / "bad.json", R"({"format":"hibiki-fw-bundle","version":1,"files":[{"path":"mbin_mxl_eagle_fw.bin","size":43997,"base64":"AAAA"}]})");
        require(!install({root / "bad.json"}, dest, message), "Corrupt bundle accepted");
        require(message.find("expected 43997") != std::string::npos, "Standard numeric metadata was not parsed");
        write(root / "bad.json", R"({"format":"hibiki-fw-bundle","version":1,"files":[]})");
        require(!install({root / "bad.json"}, dest, message), "Incomplete bundle accepted");
        write(root / "bad.json", R"({"format":"hibiki-fw-bundle","format":"other","version":1,"files":[]})");
        require(!install({root / "bad.json"}, dest, message), "Duplicate JSON key accepted");
        write(root / "bad.json", R"({"format":"hibiki-fw-bundle","version":1,"files":[{"path":"../../escape.bin","base64":"AAAA"}]})");
        require(!install({root / "bad.json"}, dest, message), "Unknown firmware path accepted");
        require(!fs::exists(root / "escape.bin"), "Bundle escaped destination");
        write(root / "bad.json", std::string(2 * 1024 * 1024 + 1, 'x'));
        require(!install({root / "bad.json"}, dest, message), "Oversized input accepted");

        if (argc > 1 && fs::exists(fs::path(argv[1]) / eagle.path)) {
            fs::path source = argv[1];
            require(install({source}, dest, message), message.c_str());
            for (const auto& spec : specs) require(verify(dest / spec.path, spec).verified, "Raw tree failed round trip");
            auto previous = contents(dest / eagle.path);
            require(!install({source / eagle.path, root / "bad.b64"}, dest, message), "Mixed valid/invalid import accepted");
            require(contents(dest / eagle.path) == previous, "Failed selection changed installed firmware");
            // Replacing a corrupt installed file is safe once the source is verified.
            write(dest / eagle.path, "corrupt");
            require(install({source / eagle.path}, dest, message), message.c_str());
            require(verify(dest / eagle.path, eagle).verified, "Verified replacement failed");
            require(install({source / eagle.path}, dest, message), "Repeated import failed");
            for (size_t i = 0; i < specs.size(); ++i) {
                auto data = contents(source / specs[i].path);
                std::string encoded(AV_BASE64_SIZE(data.size()), '\0');
                av_base64_encode(encoded.data(), static_cast<int>(encoded.size()),
                    reinterpret_cast<const uint8_t*>(data.data()), static_cast<int>(data.size()));
                encoded.resize(encoded.find('\0'));
                std::string snippet = "# Private test fixture; never written to the repository\n";
                for (size_t start = 0; start < encoded.size(); start += 76) snippet += encoded.substr(start, 76) + "\n";
                write(root / "snippets" / (std::to_string(i) + ".b64"), snippet);
            }
            require(install({root / "snippets"}, root / "decoded", message), message.c_str());
            for (const auto& spec : specs) require(verify(root / "decoded" / spec.path, spec).verified, "Base64 snippet failed round trip");
            if (argc > 2) {
                require(install({argv[2]}, root / "bundle", message), message.c_str());
                for (const auto& spec : specs) require(verify(root / "bundle" / spec.path, spec).verified, "Bundle failed round trip");
            }
            std::cout << "Raw/bundle round trips and unchanged-install rejection passed.\n";
        } else std::cout << "Private firmware fixtures unavailable; rejection tests passed.\n";
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; fs::remove_all(root); return 1;
    }
}
