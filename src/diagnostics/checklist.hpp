#pragma once
// "Checklist antes de entrar ao vivo": pure check logic with injectable probes.
// The OS/network specifics (free disk space, HTTPS reachability, upload test, encoder enumeration,
// clock) are Probes the Windows glue fills in; tests inject fakes. A probe that is not set makes its
// check report Skip honestly instead of guessing.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hb::diag {

enum class CheckStatus { Ok, Warn, Fail, Skip };
const char* checkStatusName(CheckStatus s);   // "ok" "warn" "fail" "skip"

struct CheckResult {
    std::string id;        // stable: "disk" "net" "upload" "cameras" "audio" "encoder" "destinations" "clock"
    CheckStatus status = CheckStatus::Skip;
    std::string title;     // pt-BR
    std::string detail;    // pt-BR, what was found
    std::string hint;      // pt-BR, one-line fix (empty when Ok)
};

// ---- data the app already has (no probing) ----
struct ChecklistDestination {
    std::string name;
    bool enabled = false, urlSet = false, keySet = false;
    int videoKbps = 0, audioKbps = 0;
};
struct ChecklistCamera {
    int slot = 0;
    std::string name;
    bool enabled = true;
    bool online = false;
    std::string detail;   // "sem sinal", error text...
};
struct ChecklistAudioStrip {
    int index = 0;
    std::string name;
    bool assigned = false;     // an input device is assigned to this channel
    std::string state;         // "online" "connecting" "nosignal" "error" "off"
    double peakDb = -200;      // recent peak (dBFS), -200 = silence
};

struct ChecklistInput {
    std::string recordingFolder;
    int recordingVideoKbps = 0, recordingAudioKbps = 0;   // current recording bitrate
    std::int64_t minFreeBytes = 0;                        // the recorder's own "stop below" limit
    std::vector<ChecklistDestination> destinations;
    std::vector<ChecklistCamera> cameras;
    std::vector<ChecklistAudioStrip> audio;
    bool speedTestEnabled = false;                        // OFF by default (privacy)
    std::string speedTestUrl;                             // https:// endpoint the user configured
};

// ---- probes (Windows-specific in the app, fakes in tests) ----
struct NetProbeResult {
    bool reachable = false;
    std::int64_t serverEpochSec = 0;   // from the HTTP Date header; 0 = unknown
    std::string error;
};
struct UploadProbeResult {
    bool ok = false;
    double kbps = 0;
    int httpStatus = 0;
    std::string error;
};
struct EncoderProbeResult {
    bool known = false;                // enumeration worked
    bool hardwareH264 = false, softwareH264 = false;
    std::string names;                 // friendly names, informational
};

struct Probes {
    // Free bytes on the volume holding `folder` (nearest existing parent); nullopt/negative = unknown.
    std::function<std::int64_t(const std::string& folder)> freeDiskBytes;
    std::function<NetProbeResult()> reachability;
    // Uploads a small bounded payload to `url` (https only) and measures the rate.
    std::function<UploadProbeResult(const std::string& url)> uploadTest;
    std::function<EncoderProbeResult()> encoders;
    std::function<std::int64_t()> nowEpochSec;   // local clock
};

// Thresholds (documented in docs/DIAGNOSTICO.md).
constexpr double kDiskOkMinutes = 120;
constexpr double kDiskWarnMinutes = 30;
constexpr double kUploadHeadroom = 1.5;                  // measured >= 1.5x needed = Ok
constexpr std::int64_t kClockMaxSkewSec = 120;           // vs the HTTP Date header
constexpr std::int64_t kClockMinPlausibleEpoch = 1735689600;   // 2025-01-01
constexpr double kSilenceDb = -60;
constexpr std::size_t kUploadUrlMaxLen = 300;

// Only https:// with a host, no userinfo, no whitespace/control chars, bounded length.
bool isValidSpeedTestUrl(const std::string& url);

// Individual checks (exposed for tests).
CheckResult checkDisk(const ChecklistInput& in, const Probes& p);
CheckResult checkNetwork(const NetProbeResult& net, const Probes& p);
CheckResult checkUpload(const ChecklistInput& in, const NetProbeResult& net, const Probes& p);
CheckResult checkCameras(const ChecklistInput& in);
CheckResult checkAudio(const ChecklistInput& in);
CheckResult checkEncoder(const Probes& p);
CheckResult checkDestinations(const ChecklistInput& in);
CheckResult checkClock(const NetProbeResult& net, const Probes& p);

// Runs everything in a fixed order. Probes are invoked at most once each; never throws.
std::vector<CheckResult> runChecklist(const ChecklistInput& in, const Probes& p);

// {"t":"checklist","ok":N,"warn":N,"fail":N,"results":[{"id","s","title","detail","hint"}...]}
std::string checklistJson(const std::vector<CheckResult>& r);

}  // namespace hb::diag
