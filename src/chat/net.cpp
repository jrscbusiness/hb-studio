#include "chat/net.hpp"

namespace hb::chat {

bool MemorySecretStore::put(const std::string& key, const std::string& value) {
    if (key.empty()) return false;
    for (auto& kv : items_)
        if (kv.first == key) { kv.second = value; return true; }
    items_.emplace_back(key, value);
    return true;
}
bool MemorySecretStore::get(const std::string& key, std::string& out) const {
    for (const auto& kv : items_)
        if (kv.first == key) { out = kv.second; return true; }
    return false;
}
bool MemorySecretStore::erase(const std::string& key) {
    for (auto it = items_.begin(); it != items_.end(); ++it)
        if (it->first == key) { items_.erase(it); return true; }
    return false;
}

#ifndef _WIN32
std::unique_ptr<IChatHttp> makeDefaultChatHttp() { return nullptr; }
std::unique_ptr<IChatSocket> makeDefaultChatSocket() { return nullptr; }
std::unique_ptr<ISecretStore> makeDpapiSecretStore(const std::string&) { return nullptr; }
#endif

}  // namespace hb::chat
