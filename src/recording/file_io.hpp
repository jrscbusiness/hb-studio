#pragma once
// Small portable helpers shared by the writers: UTF-8 paths, buffered output file with
// patch-in-place, little-endian byte buffers.  Nothing here throws.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hb::rec::detail {

inline std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// Output file: large buffer, tracks the logical size, supports patching earlier bytes.
class OutFile {
public:
    OutFile() = default;
    OutFile(const OutFile&) = delete;
    OutFile& operator=(const OutFile&) = delete;
    ~OutFile() {
        std::string e;
        close(e);
    }

    bool open(const std::string& pathUtf8, std::string& error) {
        if (open_) {
            error = "arquivo ja aberto";
            return false;
        }
        buf_.assign(std::size_t(1) << 18, '\0');
        f_.rdbuf()->pubsetbuf(buf_.data(), static_cast<std::streamsize>(buf_.size()));
        f_.open(pathFromUtf8(pathUtf8), std::ios::binary | std::ios::out | std::ios::trunc);
        if (!f_.is_open()) {
            error = "nao foi possivel criar o arquivo de saida";
            return false;
        }
        open_ = true;
        size_ = 0;
        return true;
    }
    bool isOpen() const { return open_; }
    // FEATURE: multitrack - pushes buffered bytes to the OS (used to keep a crash-recoverable header).
    bool flush() {
        if (!open_) return false;
        f_.flush();
        return static_cast<bool>(f_);
    }
    std::uint64_t size() const { return size_; }

    bool write(const void* data, std::size_t n, std::string& error) {
        if (!open_) {
            error = "arquivo fechado";
            return false;
        }
        if (n == 0) return true;
        f_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        if (!f_) {
            error = "falha de escrita (disco cheio ou removido?)";
            return false;
        }
        size_ += n;
        return true;
    }
    bool write(const std::vector<std::uint8_t>& v, std::string& error) { return write(v.data(), v.size(), error); }

    // Overwrites `n` bytes at `offset` (which must already be inside the file).
    bool patch(std::uint64_t offset, const void* data, std::size_t n, std::string& error) {
        if (!open_ || offset + n > size_) {
            error = "patch fora do arquivo";
            return false;
        }
        f_.seekp(static_cast<std::streamoff>(offset));
        f_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        f_.seekp(static_cast<std::streamoff>(size_));
        if (!f_) {
            error = "falha ao atualizar o cabecalho do arquivo";
            return false;
        }
        return true;
    }

    bool close(std::string& error) {
        if (!open_) return true;
        open_ = false;
        f_.flush();
        const bool ok = static_cast<bool>(f_);
        f_.close();
        if (!ok || f_.fail()) {
            error = "falha ao finalizar o arquivo (disco cheio ou removido?)";
            return false;
        }
        return true;
    }

private:
    std::ofstream f_;
    std::vector<char> buf_;
    bool open_ = false;
    std::uint64_t size_ = 0;
};

// Little-endian byte builder.
struct ByteBuf {
    std::vector<std::uint8_t> v;
    void u8(unsigned x) { v.push_back(static_cast<std::uint8_t>(x)); }
    void u16(unsigned x) {
        u8(x & 0xFF);
        u8((x >> 8) & 0xFF);
    }
    void u32(std::uint32_t x) {
        u16(x & 0xFFFF);
        u16(x >> 16);
    }
    void fourcc(const char* s) {
        for (int i = 0; i < 4; ++i) u8(static_cast<unsigned char>(s[i]));
    }
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        v.insert(v.end(), b, b + n);
    }
    std::size_t size() const { return v.size(); }
};

inline void putLe32(std::uint8_t* p, std::uint32_t x) {
    p[0] = static_cast<std::uint8_t>(x);
    p[1] = static_cast<std::uint8_t>(x >> 8);
    p[2] = static_cast<std::uint8_t>(x >> 16);
    p[3] = static_cast<std::uint8_t>(x >> 24);
}

}  // namespace hb::rec::detail
