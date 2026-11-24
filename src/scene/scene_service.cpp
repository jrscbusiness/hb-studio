#include "scene/scene_service.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "scene/template_library.hpp"

namespace hb::scene {

using core::json::Value;

namespace {

std::vector<std::string> splitN(const std::string& s, std::size_t maxParts) {
    std::vector<std::string> o;
    std::size_t pos = 0;
    while (o.size() + 1 < maxParts) {
        const auto q = s.find('|', pos);
        if (q == std::string::npos) break;
        o.push_back(s.substr(pos, q - pos));
        pos = q + 1;
    }
    o.push_back(s.substr(pos));
    return o;
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> o;
    std::string cur;
    for (char c : s) {
        if (c == ',') { if (!cur.empty()) o.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) o.push_back(cur);
    return o;
}

SceneService::Result fail(const std::string& m) { return {false, false, m}; }
SceneService::Result done(const std::string& m) { return {true, true, m}; }

bool readFile(const std::string& path, std::string& out, std::size_t maxBytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return out.size() <= maxBytes;
}

bool writeFileAtomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f.good()) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

SceneService::SceneService(std::string path, ApplyFn apply)
    : path_(std::move(path)), apply_(std::move(apply)), slots_(kSlots) {}

std::shared_ptr<const Scene> SceneService::sceneAt(int slot) const {
    std::lock_guard lock(mutex_);
    return (slot >= 0 && slot < kSlots) ? slots_[static_cast<std::size_t>(slot)] : nullptr;
}

std::vector<Template> SceneService::userTemplates() const {
    std::lock_guard lock(mutex_);
    return user_;
}

void SceneService::put(int slot, std::shared_ptr<const Scene> s) {
    slots_[static_cast<std::size_t>(slot)] = s;
    if (apply_) { try { apply_(slot, std::move(s)); } catch (...) {} }
}

int SceneService::slotOf(const std::string& id) const {
    for (int i = 0; i < kSlots; ++i)
        if (slots_[static_cast<std::size_t>(i)] && slots_[static_cast<std::size_t>(i)]->id == id) return i;
    return -1;
}
int SceneService::freeSlots() const {
    int n = 0;
    for (const auto& s : slots_) n += s ? 0 : 1;
    return n;
}
int SceneService::firstFree() const {
    for (int i = 0; i < kSlots; ++i)
        if (!slots_[static_cast<std::size_t>(i)]) return i;
    return -1;
}
std::string SceneService::uniqueSceneId(const std::string& base0) const {
    std::string base = sanitizeId(base0);
    if (base.empty()) base = "cena";
    std::string id = base;
    for (int n = 2; slotOf(id) >= 0; ++n) id = base + "-" + std::to_string(n);
    return id;
}
const Template* SceneService::findTemplate(const std::string& id) const {
    for (const auto& t : user_)
        if (t.id == id) return &t;
    for (const auto& t : builtinTemplates())
        if (t.id == id) return &t;
    return nullptr;
}

bool SceneService::save() const {
    if (path_.empty()) return true;
    Value doc;
    doc.t = Value::T::Obj;
    doc.o["version"].t = Value::T::Num;
    doc.o["version"].n = 1;
    auto& sl = doc.o["slots"];
    sl.t = Value::T::Arr;
    for (int i = 0; i < kSlots; ++i) {
        const auto& s = slots_[static_cast<std::size_t>(i)];
        if (!s) continue;
        Value e;
        e.t = Value::T::Obj;
        e.o["slot"].t = Value::T::Num;
        e.o["slot"].n = i;
        e.o["scene"] = sceneToJson(*s);
        sl.a.push_back(std::move(e));
    }
    auto& ut = doc.o["userTemplates"];
    ut.t = Value::T::Arr;
    for (const auto& t : user_) ut.a.push_back(templateToJson(t));
    return writeFileAtomic(path_, core::json::dump(doc, true) + "\n");
}

bool SceneService::load() {
    std::string text;
    if (path_.empty() || !readFile(path_, text, 4 * 1024 * 1024)) return false;
    core::json::Limits lim;
    lim.maxBytes = 4 * 1024 * 1024;
    lim.maxArray = 256;
    Value doc;
    if (!core::json::parse(text, doc, lim) || !doc.isObj()) return false;
    std::lock_guard lock(mutex_);
    if (const Value* ut = doc.get("userTemplates"); ut && ut->isArr()) {
        for (const auto& tv : ut->a) {
            Template t;
            std::string err;
            if (templateFromJson(tv, t, err)) { t.builtin = false; user_.push_back(std::move(t)); }
        }
    }
    if (const Value* sl = doc.get("slots"); sl && sl->isArr()) {
        for (const auto& e : sl->a) {
            if (!e.isObj()) continue;
            const Value* sn = e.get("slot");
            const Value* sc = e.get("scene");
            if (!sn || !sn->isNum() || !sc) continue;
            const int slot = static_cast<int>(sn->n);
            Scene s;
            std::string err;
            if (slot < 0 || slot >= kSlots || slots_[static_cast<std::size_t>(slot)] || !sceneFromJson(*sc, s, err)) continue;
            if (s.id.empty() || slotOf(s.id) >= 0) s.id = uniqueSceneId(s.id);
            put(slot, std::make_shared<const Scene>(std::move(s)));
        }
    }
    return true;
}

bool SceneService::isSceneCommand(const std::string& msg) const {
    const std::string c = msg.substr(0, msg.find('|'));
    return c == "scenesave" || c == "sceneapply" || c == "scenedelete" || c == "scenerename" || c == "scenetemplate" ||
           c == "templatedelete" || c == "sceneexport" || c == "sceneimport";
}

SceneService::Result SceneService::handle(const std::string& msg) {
    std::lock_guard lock(mutex_);
    const std::string c = msg.substr(0, msg.find('|'));
    Result r;
    if (c == "scenesave") r = doSave(msg.size() > c.size() ? msg.substr(c.size() + 1) : std::string());
    else if (c == "sceneapply") r = doApply(splitN(msg, 4));
    else if (c == "scenedelete") { auto p = splitN(msg, 2); r = p.size() >= 2 ? doDelete(p[1]) : fail("Comando inválido."); }
    else if (c == "scenerename") { auto p = splitN(msg, 3); r = p.size() >= 3 ? doRename(p[1], p[2]) : fail("Comando inválido."); }
    else if (c == "scenetemplate") { auto p = splitN(msg, 3); r = p.size() >= 3 ? doSaveTemplate(p[1], p[2]) : fail("Comando inválido."); }
    else if (c == "templatedelete") { auto p = splitN(msg, 2); r = p.size() >= 2 ? doDeleteTemplate(p[1]) : fail("Comando inválido."); }
    else if (c == "sceneexport") { auto p = splitN(msg, 3); r = p.size() >= 3 ? doExport(p[1], p[2]) : fail("Comando inválido."); }
    else if (c == "sceneimport") { auto p = splitN(msg, 2); r = p.size() >= 2 ? doImport(p[1]) : fail("Comando inválido."); }
    else r = fail("Comando de cena desconhecido.");
    if (r.changed && !save()) r.message += " (aviso: não foi possível gravar scenes.json)";
    return r;
}

SceneService::Result SceneService::doSave(const std::string& payload) {
    Value v;
    core::json::Limits lim;
    lim.maxBytes = 256 * 1024;
    lim.maxArray = 64;
    if (!core::json::parse(payload, v, lim) || !v.isObj()) return fail("Cena inválida.");
    Scene s;
    std::string err;
    if (!sceneFromJson(v, s, err)) return fail("Cena inválida: " + err);
    int slot = s.id.empty() ? -1 : slotOf(s.id);
    if (slot < 0) {
        slot = firstFree();
        if (const Value* sv = v.get("slot"); sv && sv->isNum() && sv->n >= 0 && sv->n < kSlots &&
                                              !slots_[static_cast<std::size_t>(sv->n)])
            slot = static_cast<int>(sv->n);
        if (slot < 0) return fail("Sem espaço: as 16 cenas estão em uso. Exclua uma cena primeiro.");
        s.id = uniqueSceneId(s.id.empty() ? s.name : s.id);
    }
    const std::string name = s.name;
    put(slot, std::make_shared<const Scene>(std::move(s)));
    return done("Cena \"" + name + "\" salva.");
}

SceneService::Result SceneService::doApply(const std::vector<std::string>& p) {
    if (p.size() < 3) return fail("Comando inválido.");
    const Template* t = findTemplate(sanitizeId(p[1]));
    if (!t) return fail("Template não encontrado.");
    Value mapV;
    if (!core::json::parse(p[2], mapV) || !mapV.isObj()) return fail("Mapeamento de papéis inválido.");
    std::vector<std::pair<std::string, int>> roleMap;
    for (const auto& [role, v] : mapV.o) {
        if (!v.isNum()) continue;
        const int slot = static_cast<int>(v.n);
        roleMap.emplace_back(role, (slot >= 0 && slot <= kMaxSlot) ? slot : -1);
    }
    std::vector<const Scene*> chosen;
    if (p.size() >= 4 && !p[3].empty()) {
        for (const auto& id : splitCsv(p[3]))
            for (const auto& s : t->scenes)
                if (s.id == id) chosen.push_back(&s);
    } else {
        for (const auto& s : t->scenes) chosen.push_back(&s);
    }
    if (chosen.empty()) return fail("Nenhuma cena selecionada.");
    if (static_cast<int>(chosen.size()) > freeSlots())
        return fail("Sem espaço: faltam " + std::to_string(static_cast<int>(chosen.size()) - freeSlots()) +
                    " lugar(es) no banco CENAS. Exclua cenas primeiro.");
    for (const Scene* src : chosen) {
        Scene s = applyRoles(*src, roleMap);
        s.id = uniqueSceneId(t->id + "." + src->id);
        put(firstFree(), std::make_shared<const Scene>(std::move(s)));
    }
    return done("Template \"" + t->name + "\" aplicado: " + std::to_string(chosen.size()) + " cena(s) no banco CENAS.");
}

SceneService::Result SceneService::doDelete(const std::string& id) {
    const int slot = slotOf(id);
    if (slot < 0) return fail("Cena não encontrada.");
    put(slot, nullptr);
    return done("Cena excluída.");
}

SceneService::Result SceneService::doRename(const std::string& id, const std::string& name0) {
    const int slot = slotOf(id);
    const std::string name = sanitizeText(name0, 60);
    if (slot < 0 || name.empty()) return fail("Cena não encontrada ou nome vazio.");
    Scene s = *slots_[static_cast<std::size_t>(slot)];
    s.name = name;
    put(slot, std::make_shared<const Scene>(std::move(s)));
    return done("Cena renomeada.");
}

SceneService::Result SceneService::doSaveTemplate(const std::string& ids, const std::string& name0) {
    const std::string name = sanitizeText(name0, 60);
    if (name.empty()) return fail("Dê um nome ao template.");
    Template t;
    t.name = name;
    t.category = "meus";
    t.description = "Template criado pelo usuário.";
    std::string base = sanitizeId("u-" + name);
    if (base.size() < 3) base = "u-template";
    t.id = base;
    for (int n = 2; findTemplate(t.id); ++n) t.id = base + "-" + std::to_string(n);
    for (const auto& id : splitCsv(ids)) {
        const int slot = slotOf(id);
        if (slot < 0) continue;
        Scene s = *slots_[static_cast<std::size_t>(slot)];
        for (auto& l : s.layers) {
            if (l.role.empty()) l.role = l.slot >= 0 ? "Câmera " + std::to_string(l.slot + 1) : "Fonte";
            l.slot = -1;  // templates are role based: the slot is chosen when applied
        }
        std::string sid = sanitizeId(s.id);
        const auto dot = sid.find('.');
        if (dot != std::string::npos) sid = sid.substr(dot + 1);
        if (sid.empty()) sid = "cena";
        for (int n = 2; std::any_of(t.scenes.begin(), t.scenes.end(), [&](const Scene& e) { return e.id == sid; }); ++n)
            sid = sid + "-" + std::to_string(n);
        s.id = sid;
        t.scenes.push_back(std::move(s));
    }
    if (t.scenes.empty()) return fail("Cena não encontrada.");
    t.roles = rolesOf(t.scenes);
    user_.push_back(std::move(t));
    return done("Template \"" + name + "\" salvo em Meus templates.");
}

SceneService::Result SceneService::doDeleteTemplate(const std::string& id) {
    auto it = std::find_if(user_.begin(), user_.end(), [&](const Template& t) { return t.id == id; });
    if (it == user_.end()) return fail("Só é possível excluir seus próprios templates.");
    user_.erase(it);
    return done("Template excluído.");
}

SceneService::Result SceneService::doExport(const std::string& id, const std::string& path) {
    const Template* t = findTemplate(sanitizeId(id));
    if (!t) return fail("Template não encontrado.");
    Template copy = *t;
    copy.builtin = false;
    if (path.empty() || !writeFileAtomic(path, templateDocument({copy}))) return fail("Não foi possível gravar o arquivo.");
    SceneService::Result r{true, false, "Template exportado."};
    return r;
}

SceneService::Result SceneService::doImport(const std::string& path) {
    std::string text;
    if (!readFile(path, text, 1024 * 1024)) return fail("Não foi possível ler o arquivo (ou é grande demais).");
    std::vector<Template> in;
    std::vector<std::string> errs;
    if (!parseTemplateDocument(text, in, errs, false)) return fail("Arquivo inválido: " + errs.front());
    int added = 0;
    for (auto& t : in) {
        bool clash = false;
        for (const auto& b : builtinTemplates()) clash = clash || b.id == t.id;
        if (clash) t.id = sanitizeId("u-" + t.id);
        t.builtin = false;
        auto it = std::find_if(user_.begin(), user_.end(), [&](const Template& e) { return e.id == t.id; });
        if (it != user_.end()) *it = std::move(t);
        else user_.push_back(std::move(t));
        ++added;
    }
    if (!added) return fail(errs.empty() ? "Nenhum template válido no arquivo." : "Nenhum template válido: " + errs.front());
    std::string m = std::to_string(added) + " template(s) importado(s).";
    if (!errs.empty()) m += " " + std::to_string(errs.size()) + " ignorado(s).";
    return done(m);
}

std::string SceneService::stateJson() const {
    std::lock_guard lock(mutex_);
    Value doc;
    doc.t = Value::T::Obj;
    doc.o["t"].t = Value::T::Str;
    doc.o["t"].s = "scenes_state";
    doc.o["max"].t = Value::T::Num;
    doc.o["max"].n = kSlots;
    auto& sc = doc.o["scenes"];
    sc.t = Value::T::Arr;
    for (int i = 0; i < kSlots; ++i) {
        const auto& s = slots_[static_cast<std::size_t>(i)];
        if (!s) continue;
        Value e;
        e.t = Value::T::Obj;
        e.o["slot"].t = Value::T::Num;
        e.o["slot"].n = i;
        e.o["scene"] = sceneToJson(*s);
        sc.a.push_back(std::move(e));
    }
    auto& ts = doc.o["templates"];
    ts.t = Value::T::Arr;
    auto add = [&](const Template& t) {
        Value v = templateToJson(t);
        v.o["builtin"].t = Value::T::Bool;
        v.o["builtin"].b = t.builtin;
        ts.a.push_back(std::move(v));
    };
    for (const auto& t : builtinTemplates()) add(t);
    for (const auto& t : user_) add(t);
    return core::json::dump(doc);
}

}  // namespace hb::scene
