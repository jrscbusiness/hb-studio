#include "cloud/file_system.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace hb::cloud {

namespace fs = std::filesystem;

namespace {

// UTF-8 std::string -> fs::path without the deprecated u8path (MSVC decodes u8string correctly).
fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string toUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::int64_t toEpoch(fs::file_time_type ft) {
    using namespace std::chrono;
    const auto s = time_point_cast<system_clock::duration>(ft - fs::file_time_type::clock::now() + system_clock::now());
    return duration_cast<seconds>(s.time_since_epoch()).count();
}

class StdFs : public IFileSystem {
public:
    FileStat stat(const std::string& p) override {
        FileStat s;
        std::error_code ec;
        const fs::path path = toPath(p);
        const auto st = fs::status(path, ec);
        if (ec || !fs::exists(st)) return s;
        s.exists = true;
        s.isDir = fs::is_directory(st);
        if (!s.isDir) {
            const auto sz = fs::file_size(path, ec);
            if (!ec) s.size = sz;
            ec.clear();
            const auto t = fs::last_write_time(path, ec);
            if (!ec) s.mtime = toEpoch(t);
        }
        return s;
    }

    std::vector<std::string> listFiles(const std::string& dir) override {
        std::vector<std::string> out;
        std::error_code ec;
        fs::directory_iterator it(toPath(dir), fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2) && !e2) out.push_back(toUtf8(it->path()));
        }
        return out;
    }

    bool read(const std::string& p, std::uint64_t offset, std::size_t len, std::string& out) override {
        out.clear();
        std::ifstream f(toPath(p), std::ios::binary);
        if (!f) return false;
        f.seekg(static_cast<std::streamoff>(offset));
        if (!f) return false;
        out.resize(len);
        f.read(out.data(), static_cast<std::streamsize>(len));
        const auto got = static_cast<std::size_t>(f.gcount());
        out.resize(got);
        return got == len || f.eof();  // a short read at EOF is the caller's to judge
    }

    bool remove(const std::string& p) override {
        std::error_code ec;
        return fs::remove(toPath(p), ec) && !ec;
    }
};

}  // namespace

std::shared_ptr<IFileSystem> makeStdFileSystem() { return std::make_shared<StdFs>(); }

std::string baseName(const std::string& p) {
    const auto pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string fileExtension(const std::string& p) {
    const std::string b = baseName(p);
    const auto dot = b.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= b.size()) return {};
    std::string e = b.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

}  // namespace hb::cloud
