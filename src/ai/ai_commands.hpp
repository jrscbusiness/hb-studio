#pragma once
// UI command handler for CONFIG > Inteligência Artificial. Portable (no Windows APIs): the host
// wires `post` to WebView2's postQueued and `toast` to its toast helper.
//
// Commands (message = "name|payload", payload is one JSON object or an id):
//   aiget                      -> ai_state
//   aisave|{provider json}     -> ai_state (+ "savedId"); blank apiKey keeps the stored key
//   aiactivate|<id>, airemove|<id>  -> ai_state
//   aitest|{provider json}     -> ai_test   {ok, latencyMs, message, kind}
//   aimodels|{provider json}   -> ai_models {ok, error, id, models:[{id,name,free,ctx}]}
//   aisuggest|{"topic":".."}   -> ai_suggest {ok, title, description, error}
// "provider json" may carry an unsaved draft ({id?,kind,baseUrl,model,apiKey?}); with an id, missing
// fields and a blank apiKey fall back to the saved provider.
#include <functional>
#include <string>

#include "ai/ai_service.hpp"

namespace hb::ai {

class AiCommands {
public:
    using Post = std::function<void(const std::string& key, std::string json)>;
    using Toast = std::function<void(const std::string& text)>;

    // `path` is the providers file ("" = do not persist). All references must outlive this object.
    AiCommands(AiService& service, ProviderStore& store, std::string path, Post post, Toast toast);

    // Returns true if `msg` was an AI command (handled), false otherwise.
    bool handle(const std::string& msg);
    void postState();

private:
    bool resolve(const std::string& json, Provider& out) const;
    void persist();

    AiService& svc_;
    ProviderStore& store_;
    std::string path_;
    Post post_;
    Toast toast_;
};

}  // namespace hb::ai
