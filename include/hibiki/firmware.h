#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace hibiki::firmware {

struct Spec { const char* path; size_t size; const char* sha256; const char* label; };
extern const std::array<Spec, 2> specs;
struct Status {
    std::filesystem::path path;
    bool verified = false;
    std::string detail;
};

Status verify(const std::filesystem::path& path, const Spec& spec);
// Accept a raw file/tree, a v1 bundle, or base64 snippets. Validate the entire
// selection before installing any file. Destination paths always come from specs.
bool install(const std::vector<std::filesystem::path>& sources,
             const std::filesystem::path& destination, std::string& message);

} // namespace hibiki::firmware
