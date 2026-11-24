#pragma once
// Pairing + session authentication for remote devices.
//   * A 6-digit PIN (CSPRNG) is shown on the desktop. Entering it on the phone mints a per-device
//     session token (32 random bytes, hex). Only the SHA-256 of a token is kept in memory.
//   * Brute force: after `maxFailuresPerPin` wrong PINs the PIN is REPLACED by a fresh one and
//     pairing is locked out for an exponentially growing time (even for the right PIN).
//   * Sessions expire (absolute and idle), can be revoked individually or all at once.
//   * All secret comparisons are constant-time. Time is injected (tests never sleep).
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "remote/random.hpp"

namespace hb::remote {

using TimePoint = std::chrono::steady_clock::time_point;

struct AuthConfig {
    int maxFailuresPerPin = 5;
    std::chrono::seconds lockoutBase{30};
    std::chrono::seconds lockoutMax{15 * 60};
    std::chrono::minutes sessionAbsolute{12 * 60};  // hard lifetime of a token
    std::chrono::minutes sessionIdle{4 * 60};       // lifetime without any message from the device
    std::size_t maxSessions = 16;
    bool rotatePinOnPair = true;                    // a PIN is single-use: each pairing shows a new one
};

struct SessionInfo {
    std::uint64_t id = 0;
    std::string name;
    std::string ip;
    std::int64_t ageSeconds = 0;
    std::int64_t idleSeconds = 0;
};

enum class PairResult { Ok, BadPin, Locked, TooManySessions, RandomFailure };

struct PairOutcome {
    PairResult result = PairResult::BadPin;
    std::string token;                  // Ok only: shown to the device exactly once
    std::uint64_t sessionId = 0;        // Ok only
    std::chrono::milliseconds retryAfter{0};  // Locked only
    int attemptsLeft = 0;               // BadPin only (before rotation + lockout)
};

class AuthManager {
public:
    explicit AuthManager(IRandom& rng = systemRandom(), AuthConfig cfg = {});

    std::string pin() const;     // current PIN ("" if the RNG failed)
    void newPin();               // user pressed "new PIN" (also resets the failure counter)
    unsigned pinVersion() const; // bumps whenever the PIN changes (UI refresh)
    // Is pairing currently locked out? (for UI)
    std::chrono::milliseconds lockedFor(TimePoint now) const;

    PairOutcome pair(const std::string& pin, const std::string& deviceName, const std::string& ip, TimePoint now);

    // Token login. Returns the session id (and refreshes its idle timer) or nullopt.
    std::optional<std::uint64_t> authenticate(const std::string& token, TimePoint now);
    // Still valid (not revoked / expired)? Does not refresh.
    bool valid(std::uint64_t sessionId, TimePoint now);
    void touch(std::uint64_t sessionId, TimePoint now);

    bool revoke(std::uint64_t sessionId);
    void revokeAll();
    std::vector<SessionInfo> sessions(TimePoint now);

    // Constant-time string compare (length leaks, content does not).
    static bool constantTimeEquals(const std::string& a, const std::string& b);
    // Cleans a device name from the network into something safe to display (valid UTF-8,
    // no control characters, <= 40 bytes). "Dispositivo" if nothing usable is left.
    static std::string sanitizeName(const std::string& raw);

private:
    struct Session {
        std::uint64_t id;
        std::string name, ip, tokenHash;
        TimePoint created, lastSeen;
    };
    void expireLocked(TimePoint now);
    void newPinLocked();

    IRandom& rng_;
    AuthConfig cfg_;
    mutable std::mutex mu_;
    std::string pin_;
    unsigned pinVersion_ = 1;
    int failures_ = 0;
    int lockoutLevel_ = 0;
    TimePoint lockedUntil_{};
    std::vector<Session> sessions_;
    std::uint64_t nextId_ = 1;
};

}  // namespace hb::remote
