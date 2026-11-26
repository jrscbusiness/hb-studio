#pragma once
// Windows glue for diagnostics: OS/hardware info for the diagnostic export and the real probes of the
// pre-live checklist. Everything portable (report, redaction, check logic) lives one directory up.
#include <string>
#include <vector>

#include "diagnostics/checklist.hpp"

namespace hb::diag::sys {

struct SystemInfo {
    std::string windowsVersion;          // "Windows 10.0 build 19045" (never a dotted quad)
    std::string cpuName;
    std::vector<std::string> gpus;       // "NVIDIA ... (8192 MB VRAM)"
    std::vector<std::string> monitors;   // "1920x1080 (principal)"
};

// Never throws; fields stay empty when a lookup fails.
SystemInfo querySystemInfo();

// Real probes: GetDiskFreeSpaceEx, WinHTTP (reachability against the Windows connectivity-test host,
// bounded upload test to the user-chosen https endpoint), Media Foundation encoder enumeration,
// system clock. Blocking (seconds): call from a worker thread, never from the UI thread.
Probes makeProbes();

}  // namespace hb::diag::sys
