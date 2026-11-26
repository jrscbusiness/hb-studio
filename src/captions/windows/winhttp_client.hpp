#pragma once
// Windows implementation of the two network seams of the captions module (IDownloader and IHttpPoster):
// the portable adapter (captions/net_adapter.cpp) over the shared WinHTTP client of src/net.
// NOT VALIDATED on Windows: written without a Windows compiler (see docs/LEGENDAS.md).
#ifdef _WIN32
#include "captions/net_adapter.hpp"

namespace hb::captions::win {

class WinHttpClient : public NetBackedHttp {
public:
    WinHttpClient();
};

}  // namespace hb::captions::win
#endif
