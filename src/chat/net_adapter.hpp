#pragma once
// Adapters of the chat module's seams (IChatHttp, key/value ISecretStore) onto the shared src/net layer.
// Portable, so they are unit-tested with hb::net::FakeHttpClient / FakeSecretStore. The Twitch WebSocket
// (IChatSocket) stays in net_win.cpp: it needs WinHttpWebSocket*, which the shared client does not cover.
#include <memory>
#include <string>

#include "chat/net.hpp"
#include "net/http.hpp"
#include "net/secret_store.hpp"

namespace hb::chat {

std::unique_ptr<IChatHttp> makeChatHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client);

// One "<sanitized key>.bin" file per secret under `dir`, each holding the blob protected by `protector`.
std::unique_ptr<ISecretStore> makeFileSecretStore(const std::string& dir, std::shared_ptr<hb::net::ISecretStore> protector);

}  // namespace hb::chat
