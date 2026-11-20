// hb_pack: builds a .hbplugin package from a folder.
//
//   hb_pack <folder> <output.hbplugin>
//
// The folder must contain manifest.json at its root. For "source" and "effect" plugins the tool
// computes the sha256 of every file and writes the "sha256" table into the packaged manifest (the file
// on disk is not modified). The resulting package is validated with the same code the application uses.
#include <miniz.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include "core/json.hpp"
#include "core/sha256.hpp"
#include "plugins/fs_util.hpp"
#include "plugins/manifest.hpp"
#include "plugins/package.hpp"

namespace fs = std::filesystem;
using namespace hb;

static bool readAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "uso: hb_pack <pasta> <saida.hbplugin>\n";
        return 2;
    }
    const fs::path dir = plugins::pathFromUtf8(argv[1]);
    std::string manifestText;
    if (!readAll(dir / "manifest.json", manifestText)) {
        std::cerr << "erro: manifest.json não encontrado em " << argv[1] << "\n";
        return 1;
    }
    core::json::Value manifest;
    if (!core::json::parse(manifestText, manifest) || !manifest.isObj()) {
        std::cerr << "erro: manifest.json não é um JSON válido\n";
        return 1;
    }
    const auto* type = manifest.get("type");
    const bool native = type && type->isStr() && (type->s == "source" || type->s == "effect");

    std::map<std::string, std::string> files;   // relative name -> content
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = plugins::pathToUtf8(e.path().lexically_relative(dir)).c_str();
        std::string name;
        for (char c : rel) name += (c == '\\') ? '/' : c;
        if (name == "manifest.json") continue;
        std::string data;
        if (!readAll(e.path(), data)) {
            std::cerr << "erro: não foi possível ler " << name << "\n";
            return 1;
        }
        files[name] = std::move(data);
    }
    if (native) {
        core::json::Value sums;
        sums.t = core::json::Value::T::Obj;
        for (const auto& [name, data] : files) {
            core::json::Value h;
            h.t = core::json::Value::T::Str;
            h.s = core::Sha256::hex(data);
            sums.o[name] = std::move(h);
        }
        manifest.o["sha256"] = std::move(sums);
    }
    const std::string packagedManifest = core::json::dump(manifest, true);

    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 1 << 20)) {
        std::cerr << "erro: falha ao iniciar o zip\n";
        return 1;
    }
    bool ok = mz_zip_writer_add_mem(&z, "manifest.json", packagedManifest.data(), packagedManifest.size(), MZ_BEST_COMPRESSION) != 0;
    for (const auto& [name, data] : files) ok = ok && mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
    if (!ok) {
        std::cerr << "erro: falha ao montar o pacote\n";
        mz_zip_writer_end(&z);
        return 1;
    }
    {
        std::ofstream out(plugins::pathFromUtf8(argv[2]), std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(buf), static_cast<std::streamsize>(size));
        if (!out) {
            std::cerr << "erro: não foi possível gravar " << argv[2] << "\n";
            mz_zip_writer_end(&z);
            return 1;
        }
    }
    mz_zip_writer_end(&z);

#ifdef _WIN32
    const char* platform = "windows-x64";
#else
    const char* platform = "linux-x64";
#endif
    auto check = plugins::inspectPackage(argv[2], platform);
    if (!check.ok) {
        std::cerr << "erro: o pacote gerado não passou na validação: " << check.error << "\n";
        return 1;
    }
    std::cout << "ok: " << check.info.manifest.name << " " << check.info.manifest.version << " (" << check.info.fileCount
              << " arquivos, sha256 do pacote " << check.info.packageSha256.substr(0, 16) << "...)\n";
    return 0;
}
