#include "hibiki/firmware.h"

extern "C" {
#include <libavutil/base64.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <stdexcept>
#include <system_error>

namespace hibiki::firmware {
const std::array<Spec, 2> specs{{
    {"mbin_mxl_eagle_fw.bin", 43997,
     "8f77d5cb0de9111ed63c113cb2fd54a63e07a30040ae0b562cb025ab9c4e600d", "Eagle tuner"},
    {"PT930_MN88553/PT930_pre_symbol_8051_region.bin", 4212,
     "0c3348a7f30fba3c9d3a1463f2583ce64f6af7922da4e60531f05473ce711d2a", "FX2 bridge bootstrap"}
}};
namespace {
using Bytes = std::vector<uint8_t>;
constexpr size_t max_source_size = 2 * 1024 * 1024;
Bytes read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot read " + path.string());
    auto size = f.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(max_source_size))
        throw std::runtime_error("Firmware source exceeds 2 MiB: " + path.string());
    Bytes bytes(static_cast<size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}
std::string digest(const Bytes& bytes) {
    AVSHA* sha = av_sha_alloc();
    if (!sha) throw std::runtime_error("Cannot allocate SHA-256 context");
    av_sha_init(sha, 256);
    av_sha_update(sha, bytes.data(), bytes.size());
    uint8_t hash[32];
    av_sha_final(sha, hash);
    av_free(sha);
    std::string text;
    for (auto b : hash) { text += "0123456789abcdef"[b >> 4]; text += "0123456789abcdef"[b & 15]; }
    return text;
}
void check(const Bytes& data, const Spec& spec) {
    if (data.size() != spec.size)
        throw std::runtime_error(std::string(spec.label) + ": expected " +
            std::to_string(spec.size) + " bytes; got " + std::to_string(data.size()));
    if (digest(data) != spec.sha256)
        throw std::runtime_error(std::string(spec.label) + ": SHA-256 mismatch; nothing installed");
}
Bytes base64(const std::string& raw) {
    std::string compact;
    bool comment = false, line_start = true;
    for (unsigned char c : raw) {
        if (c == '\n' || c == '\r') { comment = false; line_start = true; continue; }
        if (std::isspace(c)) continue;
        if (line_start && c == '#') comment = true;
        line_start = false;
        if (!comment) compact += c;
    }
    if (compact.empty() || compact.size() % 4) throw std::runtime_error("Invalid base64 length");
    size_t padding = 0;
    for (size_t i = 0; i < compact.size(); ++i) {
        unsigned char c = compact[i];
        if (c == '=') { ++padding; if (i < compact.size() - 2 || padding > 2) throw std::runtime_error("Invalid base64 padding"); }
        else if (padding || !(std::isalnum(c) || c == '+' || c == '/')) throw std::runtime_error("Invalid base64 payload");
    }
    Bytes out(compact.size() / 4 * 3);
    int n = av_base64_decode(out.data(), compact.c_str(), static_cast<int>(out.size()));
    if (n < 0) throw std::runtime_error("Invalid base64 payload");
    out.resize(static_cast<size_t>(n));
    return out;
}

// A bounded JSON reader for the existing firmware bundle format. No bundle
// metadata is trusted for hashes, sizes, or destination paths.
struct Json {
    char kind = 0;
    std::string text;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    const Json& at(const std::string& key) const {
        auto it = object.find(key);
        if (it == object.end()) throw std::runtime_error("Bundle is missing '" + key + "'");
        return it->second;
    }
};
class Parser {
    const std::string& s;
    size_t pos = 0;
    void ws() { while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos; }
    char take() { if (pos == s.size()) throw std::runtime_error("Truncated JSON bundle"); return s[pos++]; }
    void expect(char c) { ws(); if (take() != c) throw std::runtime_error("Invalid JSON bundle"); }
    std::string string() {
        expect('"'); std::string out;
        for (;;) {
            unsigned char c = take();
            if (c == '"') return out;
            if (c < 32) throw std::runtime_error("Invalid JSON string");
            if (c != '\\') { out += c; continue; }
            c = take();
            switch (c) {
            case '"': case '\\': case '/': out += c; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = take();
                    auto digit = std::string("0123456789abcdef").find(static_cast<char>(std::tolower(static_cast<unsigned char>(h))));
                    if (digit == std::string::npos) throw std::runtime_error("Invalid JSON unicode escape");
                    value = value * 16 + static_cast<unsigned>(digit);
                }
                // Bundle keys/paths/payloads are ASCII. Other Unicode is retained
                // as a placeholder because descriptive metadata is never used.
                out += value < 128 ? static_cast<char>(value) : '?';
                break;
            }
            default: throw std::runtime_error("Invalid JSON escape");
            }
        }
    }
    Json value(int depth) {
        if (depth > 20) throw std::runtime_error("JSON bundle nested too deeply");
        ws(); Json v;
        if (pos == s.size()) throw std::runtime_error("Empty JSON value");
        v.kind = s[pos];
        if (v.kind == '"') v.text = string();
        else if (v.kind == '{' || v.kind == '[') {
            ++pos; ws(); char end = v.kind == '{' ? '}' : ']';
            if (pos < s.size() && s[pos] == end) { ++pos; return v; }
            for (;;) {
                if (v.kind == '{') {
                    std::string key = string(); expect(':');
                    if (!v.object.emplace(key, value(depth + 1)).second) throw std::runtime_error("Duplicate JSON key");
                } else v.array.push_back(value(depth + 1));
                ws(); char c = take(); if (c == end) break;
                if (c != ',') throw std::runtime_error("Invalid JSON separator");
            }
        } else {
            size_t start = pos;
            while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' && !std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            v.text = s.substr(start, pos - start);
            static const std::regex number(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");
            if (v.text != "true" && v.text != "false" && v.text != "null" && !std::regex_match(v.text, number))
                throw std::runtime_error("Invalid JSON value");
        }
        return v;
    }
public:
    explicit Parser(const std::string& input) : s(input) {}
    Json parse() { Json v = value(0); ws(); if (pos != s.size()) throw std::runtime_error("Trailing JSON data"); return v; }
};
using Selection = std::map<size_t, Bytes>;
void identify(const Bytes& bytes, Selection& selected) {
    auto sha = digest(bytes);
    for (size_t i = 0; i < specs.size(); ++i) {
        if (sha == specs[i].sha256) { check(bytes, specs[i]); selected[i] = bytes; return; }
    }
    throw std::runtime_error("File does not match either required firmware SHA-256");
}
void collect(const std::filesystem::path& path, Selection& selected) {
    if (std::filesystem::is_directory(path)) {
        auto bundle = path / "hibiki-fw-b64.json";
        if (std::filesystem::is_regular_file(bundle)) { collect(bundle, selected); return; }
        bool found = false;
        for (size_t i = 0; i < specs.size(); ++i) {
            auto file = path / specs[i].path;
            if (std::filesystem::exists(file)) {
                auto bytes = read(file); check(bytes, specs[i]); selected[i] = std::move(bytes); found = true;
            }
        }
        size_t visited = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (++visited > 2048) throw std::runtime_error("Choose a firmware folder with fewer than 2048 entries");
            if (entry.is_regular_file() && entry.path().extension() == ".b64") { collect(entry.path(), selected); found = true; }
        }
        if (!found) throw std::runtime_error("Folder contains no recognized firmware files");
        return;
    }
    auto bytes = read(path);
    std::string raw(bytes.begin(), bytes.end());
    size_t first = raw.find_first_not_of(" \r\n\t");
    if (first != std::string::npos && raw[first] == '{') {
        Json doc = Parser(raw).parse();
        if (doc.at("format").kind != '"' || doc.at("format").text != "hibiki-fw-bundle" ||
            doc.at("version").kind != '1' || doc.at("version").text != "1" || doc.at("files").kind != '[')
            throw std::runtime_error("Expected a hibiki-fw-bundle v1 bundle");
        Selection bundle;
        for (const auto& entry : doc.at("files").array) {
            const auto& name = entry.at("path");
            for (size_t i = 0; i < specs.size(); ++i) {
                if (name.kind != '"' || name.text != specs[i].path) continue;
                if (bundle.count(i)) throw std::runtime_error("Duplicate firmware bundle entry");
                const auto& payload = entry.at("base64");
                if (payload.kind != '"') throw std::runtime_error("Bundle payload must be base64 text");
                auto data = base64(payload.text); check(data, specs[i]); bundle[i] = std::move(data);
            }
        }
        if (bundle.size() != specs.size()) throw std::runtime_error("Bundle must contain both required firmware files");
        selected.insert(bundle.begin(), bundle.end());
    } else {
        for (size_t i = 0; i < specs.size(); ++i) {
            if (digest(bytes) == specs[i].sha256) { identify(bytes, selected); return; }
        }
        identify(base64(raw), selected);
    }
}
} // namespace

Status verify(const std::filesystem::path& path, const Spec& spec) {
    Status status{path, false, "Missing"};
    try { check(read(path), spec); status.verified = true; status.detail = "Verified SHA-256"; }
    catch (const std::exception& e) { status.detail = e.what(); }
    return status;
}

bool install(const std::vector<std::filesystem::path>& sources,
             const std::filesystem::path& destination, std::string& message) {
    std::vector<std::filesystem::path> staged;
    try {
        Selection selected;
        for (const auto& source : sources) collect(source, selected);
        if (selected.empty()) throw std::runtime_error("No firmware selected");
        // Verify every input before replacing any installed firmware.
        for (const auto& [i, bytes] : selected) {
            auto target = destination / specs[i].path;
            std::filesystem::create_directories(target.parent_path());
            auto temp = target; temp += ".importing";
            staged.push_back(temp);
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            if (!out) throw std::runtime_error("Cannot write " + temp.string());
            check(read(temp), specs[i]);
        }
        size_t n = 0;
        for (const auto& [i, bytes] : selected) std::filesystem::rename(staged[n++], destination / specs[i].path);
        message = "Verified and installed " + std::to_string(selected.size()) + " firmware file(s).";
        return true;
    } catch (const std::exception& e) {
        for (const auto& temp : staged) { std::error_code ec; std::filesystem::remove(temp, ec); }
        message = e.what();
        return false;
    }
}
} // namespace hibiki::firmware
