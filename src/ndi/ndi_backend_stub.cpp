// Non-Windows default backend: the real NDI Runtime is only ever dynamically loaded on Windows
// (see ndi_backend_win.cpp). Everywhere else, defaultBackend() is this permanently-unavailable,
// fully safe no-op backend -- never a null pointer dereference, never a build-time dependency on
// anything NDI-related.
#include "ndi/ndi.hpp"
#include "ndi/unavailable_backend.hpp"

namespace hb::ndi {

std::shared_ptr<NdiBackend> defaultBackend() {
    static const std::shared_ptr<NdiBackend> instance =
        std::make_shared<UnavailableNdiBackend>("NDI só é suportado no Windows nesta versão do HB Studio.");
    return instance;
}

}  // namespace hb::ndi
