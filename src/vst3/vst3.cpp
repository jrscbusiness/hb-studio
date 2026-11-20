#include "vst3/vst3.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "pluginterfaces/vst/ivstaudioprocessor.h"  // kVstAudioEffectClass
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "vst3/detail.hpp"
#include "vst3/vst3_effect.hpp"

namespace fs = std::filesystem;

namespace hb::vst3 {
namespace {

bool hasVst3Extension(const fs::path& p) {
    // Compare case-sensitively on the raw extension; Windows installs always use ".vst3" exactly,
    // and matching loosely would risk picking up unrelated files on case-insensitive filesystems.
    return p.extension() == ".vst3";
}

// Appends every ".vst3" module found under `root` (a bundle directory or, on Windows, a single
// file) to `out`, scanning only the folder categories kVstAudioEffectClass. Recurses into
// sub-folders (vendors sometimes group plugins in a sub-folder of the VST3 directory) but never
// descends *into* a ".vst3" bundle itself. Never throws: any entry that can't be read, or isn't
// really a VST3 module, is silently skipped.
void scanFolder(const std::string& root, std::vector<Vst3PluginInfo>& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return;

    std::vector<fs::path> modulePaths;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::path& p = it->path();
        std::error_code entryEc;
        const bool isDir = it->is_directory(entryEc);
        if (!entryEc && hasVst3Extension(p)) {
            modulePaths.push_back(p);
            if (isDir) it.disable_recursion_pending();  // the bundle's own contents aren't modules
        }
        it.increment(ec);
    }

    for (const auto& modulePath : modulePaths) {
        std::string error;
        auto module = VST3::Hosting::Module::create(modulePath.string(), error);
        if (!module) continue;  // not a loadable VST3 module: skip, never fatal to the scan

        try {
            const auto& factory = module->getFactory();
            const auto info = factory.info();
            for (const auto& classInfo : factory.classInfos()) {
                if (classInfo.category() != kVstAudioEffectClass) continue;
                Vst3PluginInfo pi;
                pi.path = modulePath.string();
                pi.classId = detail::lowerHex(classInfo.ID());
                pi.name = classInfo.name();
                pi.vendor = info.vendor();
                pi.category = classInfo.subCategoriesString();
                out.push_back(std::move(pi));
            }
        } catch (...) {
            // A module whose factory throws while being inspected is skipped, same as one that
            // fails to load outright.
        }
        // `module` (a shared_ptr<Module>) goes out of scope here: the .vst3 is unloaded again.
    }
}

}  // namespace

std::vector<Vst3PluginInfo> scanPlugins(const std::vector<std::string>& extraFolders) {
    std::vector<Vst3PluginInfo> out;
    std::vector<std::string> folders;
    if (const char* common = std::getenv("COMMONPROGRAMFILES")) folders.push_back(std::string(common) + "/VST3");
    if (const char* local = std::getenv("LOCALAPPDATA")) folders.push_back(std::string(local) + "/Programs/Common/VST3");
    for (const auto& f : extraFolders) folders.push_back(f);

    for (const auto& folder : folders) scanFolder(folder, out);
    return out;
}

std::string typeIdFor(const Vst3PluginInfo& info) { return "vst3:" + info.classId; }

std::unique_ptr<hb::audio::Effect> createEffect(const Vst3PluginInfo& info, std::string& error) {
    error.clear();

    std::string moduleError;
    auto module = VST3::Hosting::Module::create(info.path, moduleError);
    if (!module) {
        error = "não foi possível carregar o módulo VST3 (" + moduleError + ")";
        return nullptr;
    }

    VST3::Hosting::ClassInfo classInfo;
    if (!detail::findAudioEffectClass(module->getFactory(), info.classId, classInfo)) {
        error = "a classe " + info.classId + " não foi encontrada no módulo";
        return nullptr;
    }

    Steinberg::Vst::PluginContextFactory::instance().setPluginContext(detail::hostContext());
    Steinberg::IPtr<Steinberg::Vst::PlugProvider> provider;
    try {
        provider = Steinberg::owned(new Steinberg::Vst::PlugProvider(module->getFactory(), classInfo, true));
        if (!provider->initialize()) {
            error = "falha ao inicializar o componente/controlador do plugin";
            return nullptr;
        }
    } catch (const std::exception& e) {
        error = std::string("o plugin lançou uma exceção ao inicializar: ") + e.what();
        return nullptr;
    } catch (...) {
        error = "o plugin lançou uma exceção desconhecida ao inicializar";
        return nullptr;
    }

    auto effect =
        std::make_unique<Vst3Effect>(std::move(module), std::move(provider), typeIdFor(info), info.name);

    // Prove the plugin can actually be activated (prepare() is also how the real session will
    // configure it later — see vst3.hpp): a sane, arbitrary default here, re-prepared for real by
    // the caller once the effect is wired into a strip.
    effect->prepare(48000, 2);
    if (!effect->activationOk()) {
        error = effect->lastError();
        if (error.empty()) error = "o plugin não pôde ser ativado para processamento de áudio";
        return nullptr;
    }

    return effect;
}

}  // namespace hb::vst3
