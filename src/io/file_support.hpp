#pragma once
#include "json_support.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace telemetry::io::detail {
namespace fs = std::filesystem;
inline std::ofstream output(const fs::path& file) {
    std::ofstream stream{file,std::ios::binary}; if (!stream) { throw std::runtime_error("Cannot write "+file.string()); }
    stream.imbue(std::locale::classic()); stream << std::setprecision(std::numeric_limits<long double>::max_digits10); return stream;
}
inline void close(std::ofstream& stream) {
    stream.flush(); require(static_cast<bool>(stream)); stream.close(); require(!stream.fail());
}
inline void save(const fs::path& file,const Json& value) { auto stream = output(file); stream << value.dump(2) << '\n'; close(stream); }
inline Json load(const fs::path& file) {
    std::ifstream stream{file,std::ios::binary}; if (!stream) { throw std::runtime_error("Cannot read "+file.string()); }
    const std::string content{std::istreambuf_iterator<char>{stream},{}}; require(!stream.bad()); return parse(content);
}
// Stable FNV-1a-64 over file bytes. Integrity metadata, not a security primitive.
inline std::string checksum(const fs::path& file) {
    std::ifstream stream{file,std::ios::binary}; if (!stream) { throw std::runtime_error("Cannot checksum "+file.string()); }
    std::uint64_t hash = 14695981039346656037ULL; std::array<char,8192> buffer{};
    while (stream) {
        stream.read(buffer.data(),static_cast<std::streamsize>(buffer.size()));
        for (std::streamsize i = 0; i < stream.gcount(); ++i) { hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]); hash *= 1099511628211ULL; }
    }
    require(stream.eof() && !stream.bad()); std::ostringstream text; text << std::hex << std::setfill('0') << std::setw(16) << hash; return text.str();
}
}  // namespace telemetry::io::detail
