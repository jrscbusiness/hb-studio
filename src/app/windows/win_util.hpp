#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace hb::win {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

// Returns a pointer/size to an embedded RCDATA resource of this module.
bool loadResource(int id, const void** data, std::size_t* size);
std::string loadResourceText(int id);

std::string base64(const std::uint8_t* data, std::size_t size);
std::string jsonEscape(const std::string& s);   // same as hb::app::jsonEscape (portable, unit-tested)

// %LOCALAPPDATA% (known folder FOLDERID_LocalAppData), no trailing backslash; empty if the lookup
// fails. Callers append e.g. L"\\HBStudio\\ptz.json".
std::wstring localAppDataPath();

// Real Windows build number (not subject to manifest shimming); 0 if unknown.
unsigned windowsBuild();

// The system-wide "choose your default app mode" setting (Settings > Personalization > Colors),
// i.e. what "Sistema" should follow for native dark mode. Defaults to false (light) if the key is
// missing or unreadable (e.g. a locked-down machine) — never throws.
bool systemPrefersDark();

// Appends a timestamped line to the file named by the HB_TRACE environment variable (no-op if unset).
void trace(const char* msg);

}  // namespace hb::win
