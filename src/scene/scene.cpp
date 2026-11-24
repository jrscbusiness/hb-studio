#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hb::scene {

using core::json::Value;

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

double clampd(double v, double lo, double hi) {
    if (!(v == v)) return lo;  // NaN
    return std::min(hi, std::max(lo, v));
}

Value num(double d) {
    Value v;
    v.t = Value::T::Num;
    v.n = std::round(d * 1e6) / 1e6;
    return v;
}
Value str(const std::string& s) {
    Value v;
    v.t = Value::T::Str;
    v.s = s;
    return v;
}
Value arr() {
    Value v;
    v.t = Value::T::Arr;
    return v;
}
Value obj() {
    Value v;
    v.t = Value::T::Obj;
    return v;
}

bool getNum(const Value& o, const char* key, double& out) {
    const Value* v = o.get(key);
    if (!v || !v->isNum()) return false;
    out = v->n;
    return true;
}

bool getColor(const Value& o, const char* key, Color& out) {
    const Value* v = o.get(key);
    return v && v->isStr() && parseColor(v->s, out);
}

}  // namespace

bool parseColor(const std::string& t, Color& out) {
    if (t.size() < 2 || t[0] != '#') return false;
    const std::string h = t.substr(1);
    for (char c : h)
        if (hexVal(c) < 0) return false;
    Color c;
    if (h.size() == 3) {
        c.r = static_cast<std::uint8_t>(hexVal(h[0]) * 17);
        c.g = static_cast<std::uint8_t>(hexVal(h[1]) * 17);
        c.b = static_cast<std::uint8_t>(hexVal(h[2]) * 17);
    } else if (h.size() == 6 || h.size() == 8) {
        c.r = static_cast<std::uint8_t>(hexVal(h[0]) * 16 + hexVal(h[1]));
        c.g = static_cast<std::uint8_t>(hexVal(h[2]) * 16 + hexVal(h[3]));
        c.b = static_cast<std::uint8_t>(hexVal(h[4]) * 16 + hexVal(h[5]));
        if (h.size() == 8) c.a = static_cast<std::uint8_t>(hexVal(h[6]) * 16 + hexVal(h[7]));
    } else {
        return false;
    }
    out = c;
    return true;
}

std::string colorToString(const Color& c) {
    char b[16];
    if (c.a == 255) std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(b, sizeof b, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return b;
}

const char* fitName(FitMode m) {
    switch (m) {
        case FitMode::Fit: return "fit";
        case FitMode::Fill: return "fill";
        case FitMode::Crop: return "crop";
    }
    return "fill";
}

bool parseFit(const std::string& s, FitMode& out) {
    if (s == "fit") out = FitMode::Fit;
    else if (s == "fill") out = FitMode::Fill;
    else if (s == "crop") out = FitMode::Crop;
    else return false;
    return true;
}

std::string sanitizeText(const std::string& s, std::size_t maxLen) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F || c == '|') continue;  // '|' is the command separator of the UI bridge
        o += static_cast<char>(c);
    }
    // trim
    while (!o.empty() && o.back() == ' ') o.pop_back();
    std::size_t b = 0;
    while (b < o.size() && o[b] == ' ') ++b;
    o.erase(0, b);
    if (o.size() > maxLen) {
        std::size_t cut = maxLen;
        while (cut > 0 && (static_cast<unsigned char>(o[cut]) & 0xC0) == 0x80) --cut;  // never split a UTF-8 sequence
        o.resize(cut);
    }
    return o;
}

std::string sanitizeId(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            o += static_cast<char>(c);
        if (o.size() >= 64) break;
    }
    return o;
}

static bool parseLayer(const Value& v, Layer& l, std::string& err) {
    if (!v.isObj()) { err = "camada inválida"; return false; }
    if (const Value* r = v.get("role"); r && r->isStr()) l.role = sanitizeText(r->s, 48);
    double d = 0;
    if (getNum(v, "slot", d)) {
        const int s = static_cast<int>(std::floor(d));
        l.slot = (s >= 0 && s <= kMaxSlot) ? s : -1;
    }
    if (const Value* r = v.get("rect")) {
        if (!r->isArr() || r->a.size() != 4) { err = "rect deve ter 4 números [x,y,w,h]"; return false; }
        double q[4];
        for (int i = 0; i < 4; ++i) {
            if (!r->a[static_cast<std::size_t>(i)].isNum()) { err = "rect deve ter 4 números [x,y,w,h]"; return false; }
            q[i] = r->a[static_cast<std::size_t>(i)].n;
        }
        l.rect.x = clampd(q[0], -1.0, 2.0);
        l.rect.y = clampd(q[1], -1.0, 2.0);
        l.rect.w = clampd(q[2], 0.01, 3.0);
        l.rect.h = clampd(q[3], 0.01, 3.0);
    }
    if (const Value* f = v.get("fit")) {
        if (!f->isStr() || !parseFit(f->s, l.fit)) { err = "fit deve ser fit, fill ou crop"; return false; }
    }
    if (getNum(v, "zoom", d)) l.zoom = clampd(d, 1.0, 4.0);
    if (const Value* c = v.get("crop"); c && c->isArr() && c->a.size() == 2 && c->a[0].isNum() && c->a[1].isNum()) {
        l.cropX = clampd(c->a[0].n, 0, 1);
        l.cropY = clampd(c->a[1].n, 0, 1);
    }
    if (getNum(v, "radius", d)) l.radius = clampd(d, 0, 0.5);
    if (const Value* b = v.get("border"); b && b->isObj()) {
        if (getNum(*b, "width", d)) l.border.width = clampd(d, 0, 0.1);
        getColor(*b, "color", l.border.color);
    }
    if (const Value* s = v.get("shadow"); s && s->isObj()) {
        if (getNum(*s, "blur", d)) l.shadow.blur = clampd(d, 0, 0.2);
        if (getNum(*s, "dx", d)) l.shadow.dx = clampd(d, -0.2, 0.2);
        if (getNum(*s, "dy", d)) l.shadow.dy = clampd(d, -0.2, 0.2);
        if (getNum(*s, "opacity", d)) l.shadow.opacity = clampd(d, 0, 1);
    }
    if (getNum(v, "opacity", d)) l.opacity = clampd(d, 0, 1);
    if (getNum(v, "z", d)) l.z = static_cast<int>(clampd(d, -1000, 1000));
    return true;
}

bool sceneFromJson(const Value& v, Scene& out, std::string& err) {
    if (!v.isObj()) { err = "cena inválida"; return false; }
    Scene s;
    if (const Value* i = v.get("id"); i && i->isStr()) s.id = sanitizeId(i->s);
    if (const Value* n = v.get("name"); n && n->isStr()) s.name = sanitizeText(n->s, 60);
    if (s.name.empty()) s.name = s.id.empty() ? "Cena" : s.id;
    if (const Value* b = v.get("background")) {
        if (!b->isObj()) { err = "background inválido"; return false; }
        std::string type = "solid";
        if (const Value* t = b->get("type"); t && t->isStr()) type = t->s;
        if (type == "solid") {
            s.bg.type = Background::Type::Solid;
            getColor(*b, "color", s.bg.from);
            s.bg.to = s.bg.from;
        } else if (type == "gradient") {
            s.bg.type = Background::Type::Gradient;
            getColor(*b, "from", s.bg.from);
            getColor(*b, "to", s.bg.to);
            double d;
            if (getNum(*b, "angle", d)) s.bg.angle = clampd(d, -360, 720);
        } else if (type == "image") {
            s.bg.type = Background::Type::Image;
            getColor(*b, "color", s.bg.from);  // shown if the image cannot be loaded
            s.bg.to = s.bg.from;
            if (const Value* p = b->get("path"); p && p->isStr()) {
                s.bg.imagePath = p->s;
                if (s.bg.imagePath.size() > 1024) s.bg.imagePath.clear();
                for (char c : s.bg.imagePath)
                    if (static_cast<unsigned char>(c) < 0x20) { s.bg.imagePath.clear(); break; }
            }
        } else {
            err = "tipo de fundo desconhecido: " + sanitizeText(type, 20);
            return false;
        }
    }
    if (const Value* ls = v.get("layers")) {
        if (!ls->isArr()) { err = "layers deve ser uma lista"; return false; }
        if (ls->a.size() > kMaxLayers) { err = "camadas demais (máximo " + std::to_string(kMaxLayers) + ")"; return false; }
        for (const auto& lv : ls->a) {
            Layer l;
            if (!parseLayer(lv, l, err)) return false;
            s.layers.push_back(std::move(l));
        }
    }
    if (const Value* os = v.get("overlays")) {
        if (!os->isArr() || os->a.size() > kMaxOverlays) { err = "overlays inválidos"; return false; }
        for (const auto& o : os->a) {
            if (!o.isObj()) { err = "overlay inválido"; return false; }
            const Value* t = o.get("type");
            if (!t || !t->isStr() || t->s.empty() || t->s.size() > 32) { err = "overlay sem type"; return false; }
            s.overlays.push_back(o);
        }
    }
    out = std::move(s);
    return true;
}

Value sceneToJson(const Scene& s) {
    Value o = obj();
    o.o["id"] = str(s.id);
    o.o["name"] = str(s.name);
    Value bg = obj();
    switch (s.bg.type) {
        case Background::Type::Solid:
            bg.o["type"] = str("solid");
            bg.o["color"] = str(colorToString(s.bg.from));
            break;
        case Background::Type::Gradient:
            bg.o["type"] = str("gradient");
            bg.o["from"] = str(colorToString(s.bg.from));
            bg.o["to"] = str(colorToString(s.bg.to));
            bg.o["angle"] = num(s.bg.angle);
            break;
        case Background::Type::Image:
            bg.o["type"] = str("image");
            bg.o["path"] = str(s.bg.imagePath);
            bg.o["color"] = str(colorToString(s.bg.from));
            break;
    }
    o.o["background"] = bg;
    Value ls = arr();
    for (const auto& l : s.layers) {
        Value lo = obj();
        if (!l.role.empty()) lo.o["role"] = str(l.role);
        lo.o["slot"] = num(l.slot);
        Value r = arr();
        r.a = {num(l.rect.x), num(l.rect.y), num(l.rect.w), num(l.rect.h)};
        lo.o["rect"] = r;
        lo.o["fit"] = str(fitName(l.fit));
        if (l.fit == FitMode::Crop) {
            lo.o["zoom"] = num(l.zoom);
            Value c = arr();
            c.a = {num(l.cropX), num(l.cropY)};
            lo.o["crop"] = c;
        }
        if (l.radius > 0) lo.o["radius"] = num(l.radius);
        if (l.border.width > 0) {
            Value b = obj();
            b.o["width"] = num(l.border.width);
            b.o["color"] = str(colorToString(l.border.color));
            lo.o["border"] = b;
        }
        if (l.shadow.enabled()) {
            Value sh = obj();
            sh.o["blur"] = num(l.shadow.blur);
            sh.o["dx"] = num(l.shadow.dx);
            sh.o["dy"] = num(l.shadow.dy);
            sh.o["opacity"] = num(l.shadow.opacity);
            lo.o["shadow"] = sh;
        }
        if (l.opacity < 1) lo.o["opacity"] = num(l.opacity);
        if (l.z != 0) lo.o["z"] = num(l.z);
        ls.a.push_back(std::move(lo));
    }
    o.o["layers"] = ls;
    if (!s.overlays.empty()) {
        Value os = arr();
        os.a = s.overlays;
        o.o["overlays"] = os;
    }
    return o;
}

std::vector<std::string> rolesOf(const Scene& s) {
    std::vector<std::string> r;
    for (const auto& l : s.layers)
        if (!l.role.empty() && std::find(r.begin(), r.end(), l.role) == r.end()) r.push_back(l.role);
    return r;
}

std::vector<std::string> rolesOf(const std::vector<Scene>& scenes) {
    std::vector<std::string> r;
    for (const auto& s : scenes)
        for (const auto& n : rolesOf(s))
            if (std::find(r.begin(), r.end(), n) == r.end()) r.push_back(n);
    return r;
}

Scene applyRoles(const Scene& s, const std::vector<std::pair<std::string, int>>& roleMap) {
    Scene o = s;
    for (auto& l : o.layers) {
        if (l.role.empty()) continue;
        for (const auto& [role, slot] : roleMap)
            if (role == l.role) { l.slot = (slot >= 0 && slot <= kMaxSlot) ? slot : -1; break; }
    }
    return o;
}

bool templateFromJson(const Value& v, Template& out, std::string& err) {
    if (!v.isObj()) { err = "template inválido"; return false; }
    Template t;
    if (const Value* i = v.get("id"); i && i->isStr()) t.id = sanitizeId(i->s);
    if (t.id.empty()) { err = "template sem id"; return false; }
    if (const Value* n = v.get("name"); n && n->isStr()) t.name = sanitizeText(n->s, 60);
    if (t.name.empty()) t.name = t.id;
    if (const Value* c = v.get("category"); c && c->isStr()) t.category = sanitizeId(c->s);
    if (t.category.empty()) t.category = "meus";
    if (const Value* d = v.get("description"); d && d->isStr()) t.description = sanitizeText(d->s, 200);
    const Value* ss = v.get("scenes");
    if (!ss || !ss->isArr() || ss->a.empty() || ss->a.size() > kMaxScenesPerTemplate) {
        err = "template precisa de 1 a " + std::to_string(kMaxScenesPerTemplate) + " cenas";
        return false;
    }
    for (const auto& sv : ss->a) {
        Scene s;
        if (!sceneFromJson(sv, s, err)) return false;
        if (s.id.empty()) s.id = "cena" + std::to_string(t.scenes.size() + 1);
        for (const auto& e : t.scenes)
            if (e.id == s.id) { err = "ids de cena repetidos: " + s.id; return false; }
        t.scenes.push_back(std::move(s));
    }
    t.roles = rolesOf(t.scenes);
    // An explicit "roles" list may add roles / fix their order, but never drop a role a layer uses.
    if (const Value* rs = v.get("roles"); rs && rs->isArr()) {
        std::vector<std::string> ordered;
        for (const auto& r : rs->a) {
            if (!r.isStr()) continue;
            const std::string n = sanitizeText(r.s, 48);
            if (!n.empty() && std::find(ordered.begin(), ordered.end(), n) == ordered.end()) ordered.push_back(n);
            if (ordered.size() >= 32) break;
        }
        for (const auto& n : t.roles)
            if (std::find(ordered.begin(), ordered.end(), n) == ordered.end()) ordered.push_back(n);
        t.roles = std::move(ordered);
    }
    out = std::move(t);
    return true;
}

Value templateToJson(const Template& t) {
    Value o = obj();
    o.o["id"] = str(t.id);
    o.o["name"] = str(t.name);
    o.o["category"] = str(t.category);
    if (!t.description.empty()) o.o["description"] = str(t.description);
    Value rs = arr();
    for (const auto& r : t.roles) rs.a.push_back(str(r));
    o.o["roles"] = rs;
    Value ss = arr();
    for (const auto& s : t.scenes) ss.a.push_back(sceneToJson(s));
    o.o["scenes"] = ss;
    return o;
}

}  // namespace hb::scene
