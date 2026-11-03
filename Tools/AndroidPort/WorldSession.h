#pragma once
#include <string>
#include "CharacterList.h"
#include "CharacterLogin.h"
#include "WorldAuthentication.h"
namespace wow::android {
// Worker-owned basic Grunt world authentication. DNS in begin is blocking;
// pump does no DNS and batches proof work. No original event/UI/gameplay dispatch.
class WorldSession {
  public:
    enum class State {
        Idle,
        Connecting,
        Challenge,
        Solving,
        Response,
        Queued,
        Ready,
        Failed,
        Cancelled
    };
    enum class Error { None, Network, Timeout, Protocol, Rejected, Unsupported, Resource, Crypto };
    enum class CharacterState { Empty, Pending, Ready };
    enum class PlayerState { Empty, Pending, Verified };
    WorldSession() = default;
    ~WorldSession();
    WorldSession(const WorldSession &) = delete;
    WorldSession &operator=(const WorldSession &) = delete;
    // Consumes ticket, clears previous attempt even on failure. Blocking DNS
    // cannot be interrupted. One total auth budget includes solving/queue wait.
    void begin(RealmSession ticket, uint32_t connectMs = 10000, uint32_t authMs = 30000);
    void pump(uint32_t waitMs = 0); // 0..100ms; <=256 SHA1 work attempts per call
    void cancel() noexcept;
    State state() const noexcept { return state_; }
    Error error() const noexcept { return error_; }
    int networkError() const noexcept { return networkError_; }
    // Where/why the session failed (check line or exception text, last packet seen).
    const std::string &diagnostic() const noexcept { return diagnostic_; }
    std::optional<uint8_t> serverResult() const noexcept { return serverResult_; }
    const WorldAuthStatus &status() const; // Queued/Ready only
    // Ready-only generic packet handoff; no opcode-specific gameplay meaning.
    // At most1024 packets and1MiB pending inbound/outbound; drain on worker.
    std::vector<WorldPacket> takePackets();
    void send(uint32_t opcode, const std::vector<uint8_t> &payload);
    // Explicit single outstanding request; a valid refresh clears the old list.
    // Timeout/malformed/unsolicited results terminate the world connection.
    void requestCharacters(uint32_t timeoutMs = 10000);
    CharacterState characterState() const noexcept { return characterState_; }
    const CharacterList &characters() const;
    void requestPlayerLogin(size_t index, uint32_t timeoutMs = 10000);
    PlayerState playerState() const noexcept { return playerState_; }
    std::optional<uint8_t> playerLoginResult() const noexcept { return playerLoginResult_; }
    const CharacterSummary &selectedCharacter() const; // Pending/Verified only
    const WorldEntry &worldEntry() const; // Protocol-confirmed only; no loaded world implied

  private:
    bool active() const noexcept;
    void clear() noexcept;
    void fail(Error, int networkError = 0) noexcept;
    void receive(WorldPacket);
    void solve();
    void queuePacket(uint32_t opcode, const std::vector<uint8_t> &payload);
    RealmSession ticket_;
    TcpEndpointConnector connection_;
    WorldWire wire_;
    WorldProofOfWork work_;
    WorldChallenge challenge_;
    WorldAuthStatus status_;
    std::vector<WorldPacket> packets_;
    size_t packetBytes_ = 0;
    uint32_t authMs_ = 0;
    std::chrono::steady_clock::time_point deadline_;
    State state_ = State::Idle;
    Error error_ = Error::None;
    int networkError_ = 0;
    std::string diagnostic_;
    uint32_t lastOpcode_ = 0, lastSize_ = 0;
    void note(const std::string &what) noexcept;
    std::optional<uint8_t> serverResult_;
    CharacterList characters_;
    CharacterState characterState_ = CharacterState::Empty;
    std::chrono::steady_clock::time_point characterDeadline_;
    uint64_t queuedBytes_ = 0, sentBytes_ = 0, characterRequestEnd_ = 0;
    PlayerState playerState_ = PlayerState::Empty;
    CharacterSummary selectedCharacter_;
    WorldEntry worldEntry_;
    std::optional<uint8_t> playerLoginResult_;
    std::chrono::steady_clock::time_point playerDeadline_;
    uint64_t playerRequestEnd_ = 0;
};
} // namespace wow::android
