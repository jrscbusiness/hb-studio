#include "remote/auth.hpp"

#include <algorithm>

#include "core/sha256.hpp"
#include "remote/websocket.hpp"

namespace hb::remote {

namespace {
std::string hexOf(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        o += d[p[i] >> 4];
        o += d[p[i] & 15];
    }
    return o;
}
}  // namespace

AuthManager::AuthManager(IRandom& rng, AuthConfig cfg) : rng_(rng), cfg_(cfg) {
    std::lock_guard lock(mu_);
    newPinLocked();
}

void AuthManager::newPinLocked() {
    // Uniform 0..999999 by rejection sampling (no modulo bias).
    constexpr std::uint32_t kRange = 1000000u;
    constexpr std::uint32_t kLimit = (0xFFFFFFFFu / kRange) * kRange;
    pin_.clear();
    for (int tries = 0; tries < 64; ++tries) {
        std::uint8_t b[4];
        if (!rng_.bytes(b, 4)) break;
        const std::uint32_t v = (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
                                (static_cast<std::uint32_t>(b[2]) << 8) | b[3];
        if (v >= kLimit) continue;
        std::uint32_t n = v % kRange;
        char s[7];
        for (int i = 5; i >= 0; --i) {
            s[i] = static_cast<char>('0' + n % 10);
            n /= 10;
        }
        s[6] = 0;
        pin_ = s;
        break;
    }
    failures_ = 0;
    ++pinVersion_;
}

std::string AuthManager::pin() const {
    std::lock_guard lock(mu_);
    return pin_;
}
unsigned AuthManager::pinVersion() const {
    std::lock_guard lock(mu_);
    return pinVersion_;
}
void AuthManager::newPin() {
    std::lock_guard lock(mu_);
    newPinLocked();
}

std::chrono::milliseconds AuthManager::lockedFor(TimePoint now) const {
    std::lock_guard lock(mu_);
    if (now >= lockedUntil_) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(lockedUntil_ - now);
}

bool AuthManager::constantTimeEquals(const std::string& a, const std::string& b) {
    // Always walks max(len) bytes; the length difference is folded into the result.
    const std::size_t n = std::max(a.size(), b.size());
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
        const unsigned y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= x ^ y;
    }
    return diff == 0;
}

std::string AuthManager::sanitizeName(const std::string& raw) {
    std::string o;
    for (unsigned char c : raw) {
        if (c < 32 || c == 127) continue;
        if (c == '<' || c == '>' || c == '"' || c == '\\') continue;
        o += static_cast<char>(c);
        if (o.size() >= 40) break;
    }
    // A truncation or hostile input may have cut a multi-byte sequence: trim until valid.
    while (!o.empty() && !ws::validUtf8(o)) o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o.empty() ? std::string("Dispositivo") : o;
}

void AuthManager::expireLocked(TimePoint now) {
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [&](const Session& s) {
                                       return now - s.created >= cfg_.sessionAbsolute || now - s.lastSeen >= cfg_.sessionIdle;
                                   }),
                    sessions_.end());
}

PairOutcome AuthManager::pair(const std::string& pin, const std::string& deviceName, const std::string& ip, TimePoint now) {
    std::lock_guard lock(mu_);
    PairOutcome out;
    expireLocked(now);
    if (now < lockedUntil_) {
        out.result = PairResult::Locked;
        out.retryAfter = std::chrono::duration_cast<std::chrono::milliseconds>(lockedUntil_ - now);
        return out;  // the PIN is not even looked at while locked out
    }
    if (pin_.empty()) {
        out.result = PairResult::RandomFailure;
        return out;
    }
    // Compare against a fixed-length candidate so length probing reveals nothing either.
    const bool good = constantTimeEquals(pin, pin_) && pin.size() == pin_.size();
    if (!good) {
        ++failures_;
        out.result = PairResult::BadPin;
        out.attemptsLeft = std::max(0, cfg_.maxFailuresPerPin - failures_);
        if (failures_ >= cfg_.maxFailuresPerPin) {
            // Burn the PIN: whatever the attacker learned is worthless now, and nobody pairs for a while.
            auto secs = cfg_.lockoutBase;
            for (int i = 0; i < lockoutLevel_ && secs < cfg_.lockoutMax; ++i) secs *= 2;
            secs = std::min(secs, cfg_.lockoutMax);
            lockedUntil_ = now + secs;
            ++lockoutLevel_;
            newPinLocked();
            out.attemptsLeft = 0;
        }
        return out;
    }
    if (sessions_.size() >= cfg_.maxSessions) {
        out.result = PairResult::TooManySessions;
        return out;
    }
    std::uint8_t raw[32];
    if (!rng_.bytes(raw, sizeof raw)) {
        out.result = PairResult::RandomFailure;
        return out;
    }
    const std::string token = hexOf(raw, sizeof raw);
    Session s;
    s.id = nextId_++;
    s.name = sanitizeName(deviceName);
    s.ip = ip;
    s.tokenHash = core::Sha256::hex(token);
    s.created = s.lastSeen = now;
    sessions_.push_back(s);
    failures_ = 0;
    lockoutLevel_ = 0;
    if (cfg_.rotatePinOnPair) newPinLocked();
    out.result = PairResult::Ok;
    out.token = token;
    out.sessionId = s.id;
    return out;
}

std::optional<std::uint64_t> AuthManager::authenticate(const std::string& token, TimePoint now) {
    if (token.size() != 64) {
        // Still burn comparable time with a dummy hash so malformed input is not a timing oracle.
        (void)core::Sha256::hex(token.substr(0, 128));
        return std::nullopt;
    }
    const std::string h = core::Sha256::hex(token);
    std::lock_guard lock(mu_);
    expireLocked(now);
    std::optional<std::uint64_t> found;
    for (auto& s : sessions_) {
        if (constantTimeEquals(s.tokenHash, h) && !found) {  // no early exit: constant work per session
            s.lastSeen = now;
            found = s.id;
        }
    }
    return found;
}

bool AuthManager::valid(std::uint64_t id, TimePoint now) {
    std::lock_guard lock(mu_);
    expireLocked(now);
    return std::any_of(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.id == id; });
}

void AuthManager::touch(std::uint64_t id, TimePoint now) {
    std::lock_guard lock(mu_);
    for (auto& s : sessions_)
        if (s.id == id) s.lastSeen = now;
}

bool AuthManager::revoke(std::uint64_t id) {
    std::lock_guard lock(mu_);
    const auto before = sessions_.size();
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.id == id; }), sessions_.end());
    return sessions_.size() != before;
}

void AuthManager::revokeAll() {
    std::lock_guard lock(mu_);
    sessions_.clear();
    newPinLocked();  // old PIN may have been seen over a shoulder: replace it too
}

std::vector<SessionInfo> AuthManager::sessions(TimePoint now) {
    std::lock_guard lock(mu_);
    expireLocked(now);
    std::vector<SessionInfo> v;
    for (const auto& s : sessions_) {
        SessionInfo i;
        i.id = s.id;
        i.name = s.name;
        i.ip = s.ip;
        i.ageSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - s.created).count();
        i.idleSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - s.lastSeen).count();
        v.push_back(std::move(i));
    }
    return v;
}

}  // namespace hb::remote
