#pragma once
// Saved AI providers (up to kMax, one active). Keys are encrypted with an ISecretStore before being
// written to disk, and the UI only ever receives a masked hint (see uiJson()).
#include <mutex>
#include <string>
#include <vector>

#include "ai/secret_store.hpp"
#include "ai/types.hpp"

namespace hb::ai {

class ProviderStore {
public:
    static constexpr int kMax = 16;

    // `secrets` may be null (no encryption available): keys then live in memory only and are not saved.
    // The store does not own `secrets`; it must outlive the store.
    explicit ProviderStore(ISecretStore* secrets) : secrets_(secrets) {}

    std::vector<Provider> list() const;
    Provider get(const std::string& id) const;  // empty() if absent
    Provider active() const;                    // empty() if none
    std::string activeId() const;

    // Adds (id empty) or updates (id exists). keepKey: ignore p.apiKey and keep the stored one.
    // The first provider ever added becomes active. Returns the id, or "" with `err` (pt-BR) set.
    std::string upsert(Provider p, bool keepKey, std::string* err = nullptr);
    bool remove(const std::string& id);   // removing the active one activates the first remaining
    bool activate(const std::string& id);

    std::string toJson() const;           // file format: keys as hex of the encrypted blob
    bool fromJson(const std::string& json);
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    bool lastLoadLostKeys() const;        // some key could not be decrypted (other Windows user/profile)

    // {"t":"ai_state","active":..,"configured":..,"canPersistKeys":..,"list":[{id,name,kind,baseUrl,model,keySet,keyHint}]}
    // The key itself is never included.
    std::string uiJson() const;

    // Decodes a UI JSON object {id,name,kind,baseUrl,apiKey,model,...}. hasKey: a non-empty apiKey was sent.
    // No validation beyond JSON shape/types; upsert() sanitises.
    static bool parseProvider(const std::string& json, Provider& out, bool& hasKey);

private:
    ISecretStore* secrets_;
    mutable std::mutex mutex_;
    std::vector<Provider> p_;
    std::string active_;
    unsigned seq_ = 1;
    bool lostKeys_ = false;
};

}  // namespace hb::ai
