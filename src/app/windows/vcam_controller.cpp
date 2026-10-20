#include "app/windows/vcam_controller.hpp"

#include <mfapi.h>

#include "app/windows/win_util.hpp"
#include "virtual-camera/windows/vcam_protocol.hpp"

namespace hb::win {

namespace {
std::string explain(HRESULT hr) {
    char b[64];
    snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    switch (static_cast<unsigned long>(hr)) {
        case 0x80070005:
            return std::string("Acesso negado (") + b + "). Verifique Configurações › Privacidade e segurança › Câmera e permita apps da área de trabalho.";
        case 0x80040154: return std::string("Componente não registrado (") + b + "). Reinstale o HB Studio.";
        default: return std::string("Falha ") + b;
    }
}
}  // namespace

VirtualCameraController::VirtualCameraController(video::ProgramOutput& program, core::Logger& log)
    : program_(program), log_(log) {
    CoIncrementMTAUsage(nullptr);  // keep the process MTA alive for the worker-created COM objects
}

VirtualCameraController::~VirtualCameraController() { shutdown(); }

bool VirtualCameraController::isInstalled() {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = sizeof(path);
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + hb::vcam::kMediaSourceClsidString +
                             L"\\InprocServer32";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

VirtualCameraController::Status VirtualCameraController::status() {
    Status s;
    s.supported = windowsBuild() >= 22000;
    s.installed = isInstalled();
    s.busy = busy_.load();
    std::lock_guard lock(mutex_);
    s.active = active_;
    s.error = error_;
    s.message = message_;
    if (sender_) {
        s.connected = sender_->connected();
        s.sent = sender_->framesSent();
    }
    return s;
}

void VirtualCameraController::setMessage(std::string m, bool err) {
    std::lock_guard lock(mutex_);
    message_ = std::move(m);
    error_ = err;
}

void VirtualCameraController::join() {
    if (worker_.joinable()) worker_.join();
}

void VirtualCameraController::activate() {
    if (busy_.exchange(true)) return;
    join();
    worker_ = std::thread([this] {
        doActivate();
        busy_ = false;
    });
}

void VirtualCameraController::deactivate() {
    if (busy_.exchange(true)) return;
    join();
    worker_ = std::thread([this] {
        doDeactivate();
        busy_ = false;
    });
}

void VirtualCameraController::shutdown() {
    join();
    doDeactivate();
}

void VirtualCameraController::doActivate() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        std::lock_guard lock(mutex_);
        if (active_) { CoUninitialize(); return; }
    }
    if (windowsBuild() < 22000) {
        setMessage("A câmera virtual exige Windows 11.", true);
        CoUninitialize();
        return;
    }
    if (!isInstalled()) {
        setMessage("Componente não instalado. Execute o instalador do HB Studio.", true);
        CoUninitialize();
        return;
    }
    if (!mfStarted_) {
        if (FAILED(MFStartup(MF_VERSION))) {
            setMessage("Falha ao iniciar o Media Foundation.", true);
            CoUninitialize();
            return;
        }
        mfStarted_ = true;
    }
    // Resolved at run time: the export only exists on Windows 11 (22000+). A static import would
    // stop the whole application from starting on older Windows.
    using CreateFn = HRESULT(WINAPI*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, PCWSTR,
                                      PCWSTR, const GUID*, ULONG, IMFVirtualCamera**);
    HMODULE sg = LoadLibraryW(L"mfsensorgroup.dll");
    auto create = sg ? reinterpret_cast<CreateFn>(GetProcAddress(sg, "MFCreateVirtualCamera")) : nullptr;
    if (!create) {
        setMessage("Este Windows não oferece a API de câmera virtual (requer Windows 11).", true);
        CoUninitialize();
        return;
    }
    IMFVirtualCamera* cam = nullptr;
    HRESULT hr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                        MFVirtualCameraAccess_CurrentUser, L"HB Studio", hb::vcam::kMediaSourceClsidString, nullptr, 0,
                        &cam);
    if (SUCCEEDED(hr)) hr = cam->Start(nullptr);
    if (FAILED(hr)) {
        if (cam) cam->Release();
        const std::string m = explain(hr);
        log_.error("vcam", "ativação falhou: " + m);
        setMessage(m, true);
        CoUninitialize();
        return;
    }
    auto sender = std::make_unique<hb::vcam::PipeFrameSender>(program_);
    sender->start();
    {
        std::lock_guard lock(mutex_);
        vcam_ = cam;
        sender_ = std::move(sender);
        active_ = true;
        message_.clear();
        error_ = false;
    }
    log_.info("vcam", "câmera virtual ativa: HB Studio Windows Virtual Camera");
    CoUninitialize();
}

void VirtualCameraController::doDeactivate() {
    std::unique_ptr<hb::vcam::PipeFrameSender> sender;
    IMFVirtualCamera* cam = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!active_) return;
        sender = std::move(sender_);
        cam = vcam_;
        vcam_ = nullptr;
        active_ = false;
        message_.clear();
        error_ = false;
    }
    if (sender) sender->stop();
    if (cam) {
        cam->Shutdown();
        cam->Release();
    }
    log_.info("vcam", "câmera virtual desativada");
}

}  // namespace hb::win
