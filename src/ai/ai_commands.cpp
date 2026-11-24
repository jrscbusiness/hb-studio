#include "ai/ai_commands.hpp"

#include <sstream>

#include "ai/assistant.hpp"
#include "ai/protocol.hpp"
#include "core/json.hpp"

namespace hb::ai {

namespace {

std::string esc(const std::string& s) { return hb::core::json::escape(s); }

}  // namespace

AiCommands::AiCommands(AiService& service, ProviderStore& store, std::string path, Post post, Toast toast)
    : svc_(service), store_(store), path_(std::move(path)), post_(std::move(post)), toast_(std::move(toast)) {}

void AiCommands::postState() {
    if (post_) post_("ai_state", store_.uiJson());
}

void AiCommands::persist() {
    if (!path_.empty() && !store_.save(path_) && toast_) toast_("Não foi possível salvar as configurações de IA em disco.");
}

// Draft + saved provider -> the provider to use for a test/model listing.
bool AiCommands::resolve(const std::string& json, Provider& out) const {
    Provider draft;
    bool hasKey = false;
    if (!ProviderStore::parseProvider(json, draft, hasKey)) return false;
    Provider base = store_.get(draft.id);
    if (base.empty()) {
        base = draft;
        if (base.baseUrl.empty()) base.baseUrl = defaultBaseUrl(base.kind);
    } else {
        if (!draft.baseUrl.empty()) base.baseUrl = draft.baseUrl;
        if (!draft.model.empty()) base.model = draft.model;
        if (hasKey) base.apiKey = draft.apiKey;
    }
    // Same cleaning as upsert() (whitespace in pasted keys etc.).
    std::string key;
    for (unsigned char c : base.apiKey)
        if (c > 0x20 && c != 0x7F) key += static_cast<char>(c);
    base.apiKey = key;
    out = std::move(base);
    return true;
}

bool AiCommands::handle(const std::string& msg) {
    const std::size_t bar = msg.find('|');
    const std::string c = msg.substr(0, bar);
    const std::string arg = bar == std::string::npos ? std::string() : msg.substr(bar + 1);

    if (c == "aiget") {
        postState();
    } else if (c == "aisave") {
        Provider p;
        bool hasKey = false;
        if (!ProviderStore::parseProvider(arg, p, hasKey)) {
            if (toast_) toast_("Dados de IA inválidos.");
            return true;
        }
        std::string err;
        const std::string id = store_.upsert(p, !hasKey, &err);
        if (id.empty()) {
            if (toast_) toast_(err.empty() ? "Não foi possível salvar o provedor." : err);
            postState();
            return true;
        }
        persist();
        std::string j = store_.uiJson();
        j.pop_back();  // reuse ai_state, add the saved id
        j += ",\"savedId\":\"" + esc(id) + "\"}";
        if (post_) post_("ai_state", std::move(j));
        if (toast_) toast_("Provedor de IA salvo.");
    } else if (c == "aiactivate") {
        if (store_.activate(arg)) {
            persist();
            if (toast_) toast_("Provedor de IA ativado.");
        }
        postState();
    } else if (c == "airemove") {
        if (store_.remove(arg)) {
            persist();
            if (toast_) toast_("Provedor de IA removido (a chave salva foi apagada).");
        }
        postState();
    } else if (c == "aitest") {
        Provider p;
        if (!resolve(arg, p)) return true;
        svc_.runAsync([this, p] {
            const ConnectionTest t = svc_.testConnection(p);
            std::ostringstream o;
            o << "{\"t\":\"ai_test\",\"ok\":" << (t.ok ? "true" : "false") << ",\"latencyMs\":" << t.latencyMs << ",\"kind\":\""
              << errorKindId(t.kind) << "\",\"message\":\"" << esc(t.message) << "\"}";
            if (post_) post_("ai_test", o.str());
        });
    } else if (c == "aimodels") {
        Provider p;
        if (!resolve(arg, p)) return true;
        svc_.runAsync([this, p] {
            const ModelsResult m = svc_.listModels(p);
            std::ostringstream o;
            o << "{\"t\":\"ai_models\",\"ok\":" << (m.ok ? "true" : "false") << ",\"id\":\"" << esc(p.id) << "\",\"error\":\""
              << esc(m.error) << "\",\"models\":[";
            for (std::size_t i = 0; i < m.models.size(); ++i) {
                const auto& mi = m.models[i];
                if (i) o << ",";
                o << "{\"id\":\"" << esc(mi.id) << "\",\"name\":\"" << esc(mi.name) << "\",\"free\":" << (mi.free ? "true" : "false")
                  << ",\"ctx\":" << mi.contextLength << "}";
            }
            o << "]}";
            if (post_) post_("ai_models", o.str());
        });
    } else if (c == "aisuggest") {
        hb::core::json::Value v;
        std::string topic;
        if (hb::core::json::parse(arg, v) && v.isObj())
            if (const auto* t = v.get("topic"); t && t->isStr()) topic = t->s;
        auto reply = [this](bool ok, const std::string& title, const std::string& desc, const std::string& err) {
            std::ostringstream o;
            o << "{\"t\":\"ai_suggest\",\"ok\":" << (ok ? "true" : "false") << ",\"title\":\"" << esc(title) << "\",\"description\":\""
              << esc(desc) << "\",\"error\":\"" << esc(err) << "\"}";
            if (post_) post_("ai_suggest", o.str());
        };
        if (topic.empty()) {
            reply(false, "", "", "Escreva o assunto da transmissão primeiro.");
            return true;
        }
        if (!svc_.configured()) {
            reply(false, "", "", "Configure e salve um provedor de IA (com modelo) acima para usar o assistente.");
            return true;
        }
        ChatOptions o;
        o.maxTokens = 400;
        svc_.completeAsync(suggestSystemPrompt(), suggestUserPrompt(topic), o, [reply](const ChatResult& r) {
            if (!r.ok) { reply(false, "", "", r.error); return; }
            const Suggestion s = parseSuggestion(r.text);
            if (!s.ok) { reply(false, "", "", "A IA respondeu em um formato que não consegui entender. Tente de novo ou troque de modelo."); return; }
            reply(true, s.title, s.description, "");
        });
    } else {
        return false;
    }
    return true;
}

}  // namespace hb::ai
