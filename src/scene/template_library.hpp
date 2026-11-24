#pragma once
// Template library as data: a versioned JSON document ("hb-templates", version 1) embedded in the
// binary (resources/templates/builtin.json) plus user templates imported/exported as files.
#include <string>
#include <string_view>
#include <vector>

#include "scene/scene.hpp"

namespace hb::scene {

inline constexpr int kTemplateFormatVersion = 1;

// The built-in library, parsed once. Never empty in a correct build (a unit test enforces it).
const std::vector<Template>& builtinTemplates();
std::string_view builtinTemplatesJson();

// Parses a "hb-templates" document. Valid templates are appended to `out`; a bad template does not
// stop the others (its pt-BR reason goes to `errors`). Returns false if the document itself is
// unusable (not JSON, wrong format, newer version, too big).
bool parseTemplateDocument(std::string_view text, std::vector<Template>& out, std::vector<std::string>& errors,
                           bool builtin = false);

// Serialises templates into a shareable document (pretty-printed when `pretty`).
std::string templateDocument(const std::vector<Template>& templates, bool pretty = true);

}  // namespace hb::scene
