#include "plugins/package.hpp"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

#include "core/sha256.hpp"
#include "plugins/fs_util.hpp"

namespace hb::plugins {

namespace {

struct Zip {
    mz_zip_archive z{};
    bool open = false;
    ~Zip() {
        if (open) mz_zip_reader_end(&z);
    }
};

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Sink {
    std::uint64_t written = 0;
    std::uint64_t limit = 0;
    core::Sha256* hash = nullptr;
    std::string* mem = nullptr;
    std::ofstream* file = nullptr;
    bool failed = false;
};

size_t sinkWrite(void* opaque, mz_uint64, const void* buf, size_t n) {
    auto* s = static_cast<Sink*>(opaque);
    if (s->failed || s->written + n > s->limit) {   // more data than the entry declared: a bomb
        s->failed = true;
        return 0;
    }
    s->written += n;
    if (s->hash) s->hash->update(buf, n);
    if (s->mem) s->mem->append(static_cast<const char*>(buf), n);
    if (s->file) {
        s->file->write(static_cast<const char*>(buf), static_cast<std::streamsize>(n));
        if (!*s->file) {
            s->failed = true;
            return 0;
        }
    }
    return n;
}

struct Entry {
    mz_uint index;
    std::string name;
    std::uint64_t size;
};

bool extractEntry(Zip& zip, const Entry& e, core::Sha256* hash, std::string* mem, std::ofstream* file) {
    Sink s;
    s.limit = e.size;
    s.hash = hash;
    s.mem = mem;
    s.file = file;
    if (!mz_zip_reader_extract_to_callback(&zip.z, e.index, sinkWrite, &s, 0)) return false;
    return !s.failed && s.written == e.size;
}

PackageResult fail(const std::string& msg) {
    PackageResult r;
    r.error = msg;
    return r;
}

// Opens + validates the archive structure and returns the entry list.
bool openAndList(const std::vector<std::uint8_t>& bytes, Zip& zip, std::vector<Entry>& entries, std::string& err) {
    if (bytes.empty() || bytes.size() > kMaxPackageBytes) {
        err = "pacote vazio ou maior que 64 MB";
        return false;
    }
    if (!mz_zip_reader_init_mem(&zip.z, bytes.data(), bytes.size(), 0)) {
        err = "arquivo não é um pacote zip válido";
        return false;
    }
    zip.open = true;
    const mz_uint n = mz_zip_reader_get_num_files(&zip.z);
    if (n == 0 || n > kMaxEntries) {
        err = "pacote com número de arquivos inválido (máximo 200)";
        return false;
    }
    std::set<std::string> seen;
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip.z, i, &st)) {
            err = "pacote corrompido";
            return false;
        }
        const std::string name = st.m_filename;
        if (!safeEntryName(name)) {
            err = "nome de arquivo inseguro no pacote: " + name.substr(0, 40);
            return false;
        }
        if (st.m_is_encrypted || !st.m_is_supported) {
            err = "pacote usa criptografia ou compressão não suportada";
            return false;
        }
        if (st.m_is_directory || name.back() == '/') continue;
        if (st.m_uncomp_size > kMaxEntryBytes || (total += st.m_uncomp_size) > kMaxTotalBytes) {
            err = "pacote descompactado grande demais";
            return false;
        }
        if (name == ".pending-delete" || name == "state.json") {
            err = "nome de arquivo reservado no pacote";
            return false;
        }
        if (!seen.insert(lowerAscii(name)).second) {   // "A.dll" and "a.dll" are the same file on Windows
            err = "arquivos duplicados no pacote";
            return false;
        }
        entries.push_back({i, name, st.m_uncomp_size});
    }
    if (entries.empty()) {
        err = "pacote sem arquivos";
        return false;
    }
    return true;
}

}  // namespace

bool readFileBytes(const std::string& pathUtf8, std::uint64_t maxBytes, std::vector<std::uint8_t>& out, std::string& err) {
    std::ifstream f(pathFromUtf8(pathUtf8), std::ios::binary | std::ios::ate);
    if (!f) {
        err = "não foi possível abrir o arquivo";
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > maxBytes) {
        err = "arquivo vazio ou grande demais";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(out.data()), size)) {
        err = "falha ao ler o arquivo";
        return false;
    }
    return true;
}

PackageResult inspectPackageBytes(const std::vector<std::uint8_t>& bytes, const std::string& platformKey) {
    Zip zip;
    std::vector<Entry> entries;
    std::string err;
    try {
        if (!openAndList(bytes, zip, entries, err)) return fail(err);

        const auto it = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return e.name == "manifest.json"; });
        if (it == entries.end()) return fail("manifest.json não encontrado na raiz do pacote");
        if (it->size > kMaxManifestBytes) return fail("manifest.json grande demais");
        std::string manifestText;
        if (!extractEntry(zip, *it, nullptr, &manifestText, nullptr)) return fail("não foi possível ler o manifest.json");
        auto mr = parseManifest(manifestText);
        if (!mr.ok) return fail("manifesto inválido: " + mr.error);

        PackageInfo info;
        info.manifest = mr.manifest;
        // Every file named in the manifest must be present, at the root, with the declared hash.
        std::map<std::string, const Entry*> byName;
        for (const auto& e : entries) byName[e.name] = &e;
        for (const auto& [file, expected] : info.manifest.sha256) {
            auto f = byName.find(file);
            if (f == byName.end()) {
                // listed for another platform: allowed to be absent only if no library for THIS platform needs it
                if (info.manifest.libraryFor(platformKey) == file) return fail("biblioteca declarada não está no pacote: " + file);
                continue;
            }
            core::Sha256 h;
            if (!extractEntry(zip, *f->second, &h, nullptr, nullptr)) return fail("falha ao verificar " + file);
            if (h.hexDigest() != expected) return fail("o arquivo " + file + " não confere com o sha256 do manifesto (adulterado?)");
        }
        if (info.manifest.type == PluginType::Source) {
            const std::string lib = info.manifest.libraryFor(platformKey);
            info.supportedHere = !lib.empty() && byName.count(lib) != 0;
            // Native code that is not covered by a hash is never accepted.
            for (const auto& e : entries) {
                const std::string l = lowerAscii(e.name);
                const bool code = l.size() > 4 && (l.compare(l.size() - 4, 4, ".dll") == 0 || l.compare(l.size() - 3, 3, ".so") == 0 ||
                                                   l.compare(l.size() - 6, 6, ".dylib") == 0 || l.compare(l.size() - 4, 4, ".exe") == 0);
                if (code && !info.manifest.sha256.count(e.name)) return fail("arquivo executável sem sha256 no manifesto: " + e.name);
            }
        } else {
            for (const auto& e : entries) {
                const std::string l = lowerAscii(e.name);
                for (const char* ext : {".dll", ".exe", ".so", ".dylib", ".bat", ".cmd", ".ps1", ".js", ".msi", ".scr", ".com"})
                    if (l.size() > std::char_traits<char>::length(ext) &&
                        l.compare(l.size() - std::char_traits<char>::length(ext), std::string::npos, ext) == 0)
                        return fail("temas não podem conter arquivos executáveis: " + e.name);
            }
        }
        core::Sha256 ph;
        ph.update(bytes.data(), bytes.size());
        info.packageSha256 = ph.hexDigest();
        for (const auto& e : entries) {
            info.totalSize += e.size;
            info.files.push_back(e.name);
        }
        info.fileCount = static_cast<int>(entries.size());
        PackageResult r;
        r.ok = true;
        r.info = std::move(info);
        return r;
    } catch (const std::exception&) {
        return fail("erro inesperado ao ler o pacote");
    } catch (...) {
        return fail("erro inesperado ao ler o pacote");
    }
}

PackageResult inspectPackage(const std::string& zipPathUtf8, const std::string& platformKey) {
    std::vector<std::uint8_t> bytes;
    std::string err;
    if (!readFileBytes(zipPathUtf8, kMaxPackageBytes, bytes, err)) return fail(err);
    return inspectPackageBytes(bytes, platformKey);
}

bool extractPackageBytes(const std::vector<std::uint8_t>& bytes, const std::string& platformKey, const std::string& expectedSha,
                         const std::string& destDirUtf8, PackageInfo& info, std::string& err) {
    auto r = inspectPackageBytes(bytes, platformKey);
    if (!r.ok) {
        err = r.error;
        return false;
    }
    if (!expectedSha.empty() && r.info.packageSha256 != expectedSha) {
        err = "o pacote mudou depois da revisão; instalação cancelada";
        return false;
    }
    try {
        namespace fs = std::filesystem;
        const fs::path dest = pathFromUtf8(destDirUtf8);
        std::error_code ec;
        fs::create_directories(dest, ec);
        if (ec) {
            err = "não foi possível criar a pasta de instalação";
            return false;
        }
        if (!fs::is_empty(dest, ec)) {
            err = "pasta de instalação não está vazia";
            return false;
        }
        Zip zip;
        std::vector<Entry> entries;
        if (!openAndList(bytes, zip, entries, err)) return false;
        const fs::path root = fs::weakly_canonical(dest, ec);
        for (const auto& e : entries) {
            const fs::path target = (dest / pathFromUtf8(e.name)).lexically_normal();
            const std::string rel = pathToUtf8(target.lexically_relative(dest.lexically_normal()));
            if (rel.empty() || rel.rfind("..", 0) == 0) {   // belt and braces on top of safeEntryName()
                err = "caminho inseguro no pacote";
                return false;
            }
            (void)root;
            fs::create_directories(target.parent_path(), ec);
            if (ec) {
                err = "não foi possível criar pastas do plugin";
                return false;
            }
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) {
                err = "não foi possível gravar " + e.name;
                return false;
            }
            core::Sha256 h;
            if (!extractEntry(zip, e, &h, nullptr, &out)) {
                err = "falha ao extrair " + e.name;
                return false;
            }
            out.close();
            const auto want = r.info.manifest.sha256.find(e.name);
            if (want != r.info.manifest.sha256.end() && h.hexDigest() != want->second) {
                err = "o arquivo " + e.name + " mudou durante a extração";
                return false;
            }
        }
        info = r.info;
        return true;
    } catch (...) {
        err = "erro inesperado ao instalar o pacote";
        return false;
    }
}

}  // namespace hb::plugins
