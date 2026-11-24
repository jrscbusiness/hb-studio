#pragma once
// Persisted operator choices for "Legendas ao vivo" (captions.json in %LOCALAPPDATA%\HBStudio).
// Loading is strict about values (unknown engine/model/language fall back to defaults) and never throws.
// The cloud API key is stored in this file in plain text (same as the streaming keys); it is never
// logged and never sent back to the UI (only a "has key" flag).
#include <string>

namespace hb::captions {

struct CaptionSettings {
    std::string engine = "whisper";     // "whisper" | "cloud"
    std::string model = "leve";         // catalogue id
    std::string language = "pt";        // ISO-639-1 or "auto"
    bool profanityFilter = false;
    bool saveSrt = true;
    int maxCharsPerLine = 42;           // 20..80
    int maxLines = 2;                   // 1..3
    std::string cloudBaseUrl = "https://api.openai.com/v1";
    std::string cloudModel = "whisper-1";
    std::string cloudKey;
};

bool isSupportedLanguage(const std::string& code);
const char* const* supportedLanguages(std::size_t& count);   // ISO codes incl. "auto"

// Applies one "key=value" change from the UI. Returns false (and leaves s untouched) if invalid.
bool applySetting(CaptionSettings& s, const std::string& key, const std::string& value);
std::string settingsToJson(const CaptionSettings& s);          // for the file (includes the key)
bool settingsFromJson(const std::string& json, CaptionSettings& out);   // tolerant, validated
bool loadSettings(const std::string& pathUtf8, CaptionSettings& out);  // missing file -> defaults, true
bool saveSettings(const std::string& pathUtf8, const CaptionSettings& s);

}  // namespace hb::captions
