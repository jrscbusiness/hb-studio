#pragma once
// Builds the single text file the user attaches to a bug report ("Exportar diagnóstico").
// Pure: everything it prints comes from DiagnosticsInput (filled by the app / Windows glue) and the
// WHOLE output goes through the Redactor as the last step, so a secret that slips into any field
// (device name, log line, setting value...) is still removed.
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "core/logger.hpp"
#include "diagnostics/redactor.hpp"

namespace hb::diag {

struct DeviceLine {
    std::string name;
    std::string status;   // e.g. "online", "sem sinal", "erro", "não atribuído"
    std::string detail;   // optional (slot, native resolution, last error...)
};

struct KeyValue {
    std::string key;
    std::string value;
    bool secret = false;   // true: the value is NEVER printed, only "(definido)" / "(vazio)"
};

struct DestinationLine {
    std::string name, platform, protocol;
    std::string serverUrl;   // only scheme://host is printed (see urlSchemeHost); path/query may hold a key
    bool enabled = false, urlSet = false, keySet = false;
    int videoKbps = 0, audioKbps = 0;
    std::string status;      // last status text of its sender, "" if not running
};

struct ModuleError {
    std::string module;
    std::string message;
    std::string when;   // "HH:MM:SS" or ""
};

struct DiagnosticsInput {
    std::string appVersion;          // e.g. "0.3.0+abc1234" (commit hash included)
    std::string windowsVersion;      // e.g. "Windows 10.0 build 19045"
    std::string cpuName;
    unsigned logicalCores = 0;
    double ramGiB = 0;
    std::vector<std::string> gpus;
    std::vector<std::string> monitors;   // "1920x1080 @ (0,0) principal"
    std::vector<DeviceLine> videoDevices, audioDevices;
    std::vector<KeyValue> features;      // name -> "ligado"/"desligado"
    std::vector<KeyValue> settings;
    std::vector<DestinationLine> destinations;
    std::string previewQuality;          // effective preset id, e.g. "alta"
    std::string outputFormat;            // e.g. "1920x1080 @ 30 fps"
    std::vector<core::LogRecord> log;    // in-memory log tail (oldest first)
    std::size_t logTailLines = 300;
    std::vector<ModuleError> extraErrors;   // errors known from live status (camera/stream/cloud/chat...)
};

// Last Warning-or-worse record of every log module, newest first.
std::vector<ModuleError> lastErrorsPerModule(const std::vector<core::LogRecord>& log);

std::string buildReport(const DiagnosticsInput& in, const Redactor& redactor,
                        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

// Writes `text` to `path` (UTF-8, truncating). `path` MUST come from a native save dialog, never from a
// UI message. Returns false and fills `error` on failure.
bool writeTextFile(const std::string& path, const std::string& text, std::string& error);

}  // namespace hb::diag
