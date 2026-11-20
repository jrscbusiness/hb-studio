#pragma once
// File-name policy of the recorder: template expansion, sanitising and never-overwrite selection.
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>

namespace hb::rec {

// Makes `name` (UTF-8, no directory) safe as a single path component on Windows and POSIX: control
// characters and < > : " / \ | ? * become '_', ".." is removed, trailing dots/spaces and leading dots are
// dropped, reserved Windows device names (CON, NUL, COM1...) get a '_' prefix, the length is capped at 120
// bytes on a UTF-8 boundary. Never returns an empty string ("HBStudio" is the fallback).
std::string sanitizeFileName(const std::string& name);

// Replaces {date} (2026-10-21), {time} (14-05-33) and {n}; other text is kept verbatim.
std::string expandNameTemplate(const std::string& tmpl, const std::tm& local, int n);

// folder/name[.ext]; if it already exists appends " (2)", " (3)"... so nothing is ever overwritten.
// `ext` without dot; empty means no extension (used for folders).
std::string uniqueOutputPath(const std::string& folderUtf8, const std::string& name, const std::string& ext);

// Picks the first n >= 1 whose expansion does not exist yet when the template contains {n}.
std::string firstFreeName(const std::string& folderUtf8, const std::string& tmpl, const std::tm& local,
                          const std::string& ext);

bool pathExists(const std::string& pathUtf8);

}  // namespace hb::rec
