#include "scene/template_library.hpp"

#include <cstddef>

namespace hb::scene {

// Generated at configure time from resources/templates/builtin.json (see cmake/modules/scene.cmake).
extern const unsigned char kBuiltinTemplatesData[];
extern const std::size_t kBuiltinTemplatesSize;

std::string_view builtinTemplatesJson() {
    return std::string_view(reinterpret_cast<const char*>(kBuiltinTemplatesData), kBuiltinTemplatesSize);
}

const std::vector<Template>& builtinTemplates() {
    static const std::vector<Template> kLib = [] {
        std::vector<Template> v;
        std::vector<std::string> errs;
        parseTemplateDocument(builtinTemplatesJson(), v, errs, true);
        return v;
    }();
    return kLib;
}

bool parseTemplateDocument(std::string_view text, std::vector<Template>& out, std::vector<std::string>& errors, bool builtin) {
    core::json::Limits lim;
    lim.maxBytes = 1024 * 1024;
    lim.maxDepth = 16;
    lim.maxArray = 256;
    core::json::Value doc;
    if (!core::json::parse(text, doc, lim) || !doc.isObj()) {
        errors.push_back("arquivo de templates inválido (JSON malformado ou grande demais)");
        return false;
    }
    const auto* fmt = doc.get("format");
    if (!fmt || !fmt->isStr() || fmt->s != "hb-templates") {
        errors.push_back("não é um arquivo de templates do HB Studio (campo \"format\" deve ser \"hb-templates\")");
        return false;
    }
    const auto* ver = doc.get("version");
    if (!ver || !ver->isNum()) {
        errors.push_back("arquivo sem \"version\"");
        return false;
    }
    if (ver->n > kTemplateFormatVersion) {
        errors.push_back("arquivo de uma versão mais nova do HB Studio (version " + std::to_string(static_cast<int>(ver->n)) + ")");
        return false;
    }
    const auto* ts = doc.get("templates");
    if (!ts || !ts->isArr()) {
        errors.push_back("arquivo sem lista \"templates\"");
        return false;
    }
    for (const auto& tv : ts->a) {
        Template t;
        std::string err;
        if (!templateFromJson(tv, t, err)) {
            errors.push_back(err);
            continue;
        }
        t.builtin = builtin;
        out.push_back(std::move(t));
    }
    return true;
}

std::string templateDocument(const std::vector<Template>& templates, bool pretty) {
    core::json::Value doc;
    doc.t = core::json::Value::T::Obj;
    doc.o["format"].t = core::json::Value::T::Str;
    doc.o["format"].s = "hb-templates";
    doc.o["version"].t = core::json::Value::T::Num;
    doc.o["version"].n = kTemplateFormatVersion;
    auto& arr = doc.o["templates"];
    arr.t = core::json::Value::T::Arr;
    for (const auto& t : templates) arr.a.push_back(templateToJson(t));
    return core::json::dump(doc, pretty) + "\n";
}

}  // namespace hb::scene
