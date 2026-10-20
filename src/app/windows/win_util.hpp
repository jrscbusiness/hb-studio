#pragma once
#include <windows.h>

#include <string>

namespace hb::win {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

// Returns a pointer/size to an embedded RCDATA resource of this module.
bool loadResource(int id, const void** data, std::size_t* size);
std::string loadResourceText(int id);

std::string base64(const std::uint8_t* data, std::size_t size);
std::string jsonEscape(const std::string& s);

// Real Windows build number (not subject to manifest shimming); 0 if unknown.
unsigned windowsBuild();

}  // namespace hb::win
