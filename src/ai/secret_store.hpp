#pragma once
// Encryption-at-rest for API keys, behind an interface. Windows: DPAPI (CryptProtectData, bound to
// the current Windows user; implemented once in src/net/secret_store_win.cpp). Tests: a reversible fake.
#include <memory>
#include <string>

#include "net/secret_store.hpp"

namespace hb::ai {

// The interface and the fake are the shared ones from src/net (one DPAPI implementation for the whole app).
using ISecretStore = hb::net::ISecretStore;
using FakeSecretStore = hb::net::FakeSecretStore;

// DPAPI (entropy "HBStudio.ai.v1") on Windows; nullptr elsewhere (keys are then kept in memory only, never written to disk).
std::unique_ptr<ISecretStore> makeDefaultSecretStore();

std::string toHex(const std::string& bytes);
bool fromHex(const std::string& hex, std::string& bytes);

}  // namespace hb::ai
