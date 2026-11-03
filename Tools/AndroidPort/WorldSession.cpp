#include <cstdio>
#include "WorldSession.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace wow::android {
WorldSession::~WorldSession() { clear(); }
void WorldSession::clear() noexcept {
    connection_.close();
    wire_.reset();
    work_.reset();
    diagnostic_.clear();
    lastOpcode_ = lastSize_ = 0;
    ticket_.reset();
    challenge_ = {};
    status_ = {};
    packets_.clear();
    packetBytes_ = 0;
    characters_.characters.clear();
    characters_.raceRestrictions.reset();
    characterState_ = CharacterState::Empty;
    queuedBytes_ = sentBytes_ = characterRequestEnd_ = 0;
    playerState_ = PlayerState::Empty;
    selectedCharacter_ = {};
    worldEntry_ = {};
    playerLoginResult_.reset();
    playerRequestEnd_ = 0;
    serverResult_.reset();
    state_ = State::Idle;
    error_ = Error::None;
    networkError_ = 0;
}
void WorldSession::cancel() noexcept {
    clear();
    state_ = State::Cancelled;
}
void WorldSession::fail(Error error, int networkError) noexcept {
    auto result = serverResult_;
    auto playerResult = playerLoginResult_;
    clear();
    serverResult_ = result;
    playerLoginResult_ = playerResult;
    error_ = error;
    networkError_ = networkError;
    state_ = State::Failed;
}
bool WorldSession::active() const noexcept {
    return state_ != State::Idle && state_ != State::Failed && state_ != State::Cancelled;
}
void WorldSession::begin(RealmSession ticket, uint32_t connectMs, uint32_t authMs) {
    clear();
    if (!ticket.valid() || !connectMs || connectMs > 60000 || !authMs || authMs > 60000) {
        { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
        throw std::invalid_argument("Invalid world session configuration");
    }
    ticket_ = std::move(ticket);
    authMs_ = authMs;
    try {
        auto endpoint = ticket_.resolveEndpoint();
        connection_.connect(endpoint, connectMs);
        state_ = State::Connecting;
    } catch (...) {
        fail(Error::Network);
        throw;
    }
}
void WorldSession::note(const std::string &what) noexcept {
    try {
        static const char *const names[] = {"Idle", "Connecting", "Challenge", "Solving", "Response",
                                            "Queued", "Ready", "Failed", "Cancelled"};
        char last[64];
        std::snprintf(last, sizeof(last), "; estado %s; último pacote 0x%x (%u bytes)",
                      names[int(state_)], lastOpcode_, lastSize_);
        diagnostic_ = what + last;
    } catch (...) {
    }
}
void WorldSession::receive(WorldPacket packet) {
    lastOpcode_ = packet.opcode;
    lastSize_ = uint32_t(packet.payload.size());
    if (state_ == State::Challenge) {
        challenge_ = decodeWorldChallenge(packet);
        if (challenge_.difficulty > 20) {
            fail(Error::Unsupported);
            return;
        }
        work_.begin(ticket_.account(), challenge_);
        state_ = State::Solving;
    } else if ((state_ == State::Response || state_ == State::Queued) && packet.opcode != 0x1ee) {
        return; // the original client only dispatches SMSG_AUTH_RESPONSE here
    } else if (state_ == State::Response || state_ == State::Queued) {
        if (connection_.pendingSend()) {
            { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
            return;
        }
        auto status = decodeWorldAuthStatus(packet);
        serverResult_ = status.result;
        if (status.result != 12 && status.result != 27) {
            fail(Error::Rejected);
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline_) {
            fail(Error::Timeout);
            return;
        }
        status_ = status;
        state_ = status.result == 12 ? State::Ready : State::Queued;
    } else if (state_ == State::Ready) {
        if (packet.opcode == 0x236 || packet.opcode == 0x41) {
            if (playerState_ != PlayerState::Pending || sentBytes_ < playerRequestEnd_) {
                { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
                return;
            }
            if (std::chrono::steady_clock::now() >= playerDeadline_) {
                fail(Error::Timeout);
                return;
            }
            if (packet.opcode == 0x41) {
                playerLoginResult_ = decodeCharacterLoginFailure(packet);
                fail(Error::Rejected);
                return;
            }
            auto entry = decodeWorldEntry(packet);
            if (std::chrono::steady_clock::now() >= playerDeadline_) {
                fail(Error::Timeout);
                return;
            }
            worldEntry_ = entry;
            playerState_ = PlayerState::Verified;
            return;
        }
        if (packet.opcode == 0x3b) {
            if (characterState_ != CharacterState::Pending || sentBytes_ < characterRequestEnd_) {
                { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
                return;
            }
            if (std::chrono::steady_clock::now() >= characterDeadline_) {
                fail(Error::Timeout);
                return;
            }
            auto list = decodeCharacterList(packet);
            if (std::chrono::steady_clock::now() >= characterDeadline_) {
                fail(Error::Timeout);
                return;
            }
            characters_ = std::move(list);
            characterState_ = CharacterState::Ready;
            return;
        }
        if (packet.opcode == 0x1ec || packet.opcode == 0x1ee) {
            { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
            return;
        }
        const size_t size = packet.payload.size() + 4;
        if (packets_.size() >= 16384 || size > 16 * 1024 * 1024 - packetBytes_) {
            fail(Error::Resource);
            return;
        }
        packets_.push_back(std::move(packet));
        packetBytes_ += size;
    } else
        { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
}
void WorldSession::solve() {
    if (state_ != State::Solving)
        return;
    std::optional<uint64_t> nonce;
    try {
        nonce = work_.step(256);
    } catch (...) {
        fail(Error::Resource);
        return;
    }
    if (!nonce)
        return;
    if (connection_.state() != NativeTcp::State::Connected) {
        fail(Error::Network, connection_.error());
        return;
    }
    try {
        const auto packet = encodeWorldAuthentication(ticket_, challenge_, *nonce);
        auto frame = wire_.encode(packet.opcode, packet.payload);
        connection_.queue(frame);
        queuedBytes_ += frame.size();
        wire_.enableEncryption(ticket_.sessionKey());
        work_.reset();
        state_ = State::Response;
    } catch (...) {
        fail(Error::Crypto);
    }
}
void WorldSession::pump(uint32_t waitMs) {
    if (waitMs > 100)
        throw std::invalid_argument("World pump wait exceeds100ms");
    if (!active())
        return;
    try {
        const auto now = std::chrono::steady_clock::now();
        if (state_ != State::Connecting && state_ != State::Ready) {
            if (now >= deadline_) {
                fail(Error::Timeout);
                return;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count();
            waitMs = std::min(waitMs, uint32_t(left));
        }
        if (state_ == State::Solving)
            waitMs = 0;
        if (characterState_ == CharacterState::Pending) {
            if (now >= characterDeadline_) {
                fail(Error::Timeout);
                return;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(characterDeadline_ - now)
                    .count();
            waitMs = std::min(waitMs, uint32_t(left));
        }
        if (playerState_ == PlayerState::Pending) {
            if (now >= playerDeadline_) {
                fail(Error::Timeout);
                return;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(playerDeadline_ - now)
                    .count();
            waitMs = std::min(waitMs, uint32_t(left));
        }
        const auto pendingBefore = connection_.pendingSend();
        connection_.pump(waitMs);
        const auto pendingAfter = connection_.pendingSend();
        if (pendingAfter > pendingBefore) {
            { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
            return;
        }
        sentBytes_ += pendingBefore - pendingAfter;
        if (state_ == State::Connecting && connection_.state() == NativeTcp::State::Connected) {
            state_ = State::Challenge;
            deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(authMs_);
        }
        if (state_ != State::Connecting && state_ != State::Ready &&
            std::chrono::steady_clock::now() >= deadline_) {
            fail(Error::Timeout);
            return;
        }
        auto bytes = connection_.takeReceived();
        if (!bytes.empty())
            wire_.feed(bytes);
        while (active()) {
            auto packet = wire_.next();
            if (!packet)
                break;
            receive(std::move(*packet));
        }
        if (!active())
            return;
        if (state_ == State::Solving && wire_.buffered()) {
            { note("verificação linha " + std::to_string(__LINE__)); fail(Error::Protocol); }
            return;
        }
        solve();
        if (!active())
            return;
        if (playerState_ == PlayerState::Pending &&
            std::chrono::steady_clock::now() >= playerDeadline_) {
            fail(Error::Timeout);
            return;
        }
        if (characterState_ == CharacterState::Pending &&
            std::chrono::steady_clock::now() >= characterDeadline_) {
            fail(Error::Timeout);
            return;
        }
        if (state_ != State::Connecting && state_ != State::Ready &&
            std::chrono::steady_clock::now() >= deadline_) {
            fail(Error::Timeout);
            return;
        }
        if (connection_.state() == NativeTcp::State::PeerClosed) {
            wire_.finish();
            fail(Error::Network);
        } else if (connection_.state() == NativeTcp::State::Failed ||
                   connection_.state() == NativeTcp::State::Closed)
            fail(Error::Network, connection_.error());
    } catch (const std::exception &error) {
        note(error.what());
        fail(Error::Protocol);
    } catch (...) {
        note("exceção desconhecida");
        fail(Error::Protocol);
    }
}
const WorldAuthStatus &WorldSession::status() const {
    if (state_ != State::Queued && state_ != State::Ready)
        throw std::logic_error("World auth status unavailable");
    return status_;
}
std::vector<WorldPacket> WorldSession::takePackets() {
    if (state_ != State::Ready)
        throw std::logic_error("World packets unavailable before authentication");
    std::vector<WorldPacket> result;
    result.swap(packets_);
    packetBytes_ = 0;
    return result;
}
void WorldSession::send(uint32_t opcode, const std::vector<uint8_t> &payload) {
    if (opcode == 0x37 || opcode == 0x3d)
        throw std::invalid_argument("Use typed character enumeration/login requests");
    queuePacket(opcode, payload);
}
void WorldSession::queuePacket(uint32_t opcode, const std::vector<uint8_t> &payload) {
    if (state_ != State::Ready)
        throw std::logic_error("World send requires authentication");
    if (opcode == 0x1ed || payload.size() > WorldWire::maxPacket - 4)
        throw std::invalid_argument("Invalid post-authentication world packet");
    const auto size = payload.size() + 4;
    const auto frame = size + (size > 0x7fff ? 3 : 2);
    if (frame > NativeTcp::capacity - connection_.pendingSend())
        throw std::length_error("World send queue full");
    if (frame > std::numeric_limits<uint64_t>::max() - queuedBytes_)
        throw std::length_error("World send byte counter exhausted");
    try {
        connection_.queue(wire_.encode(opcode, payload));
        queuedBytes_ += frame;
    } catch (...) {
        fail(Error::Resource);
        throw;
    }
}
void WorldSession::requestCharacters(uint32_t timeoutMs) {
    if (state_ != State::Ready || characterState_ == CharacterState::Pending ||
        playerState_ != PlayerState::Empty)
        throw std::logic_error("Character request requires authenticated idle enumeration state");
    if (!timeoutMs || timeoutMs > 60000)
        throw std::invalid_argument("Invalid character request timeout");
    queuePacket(0x37, {});
    characters_.characters.clear();
    characters_.raceRestrictions.reset();
    characterRequestEnd_ = queuedBytes_;
    characterDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    characterState_ = CharacterState::Pending;
}
const CharacterList &WorldSession::characters() const {
    if (state_ != State::Ready || characterState_ != CharacterState::Ready)
        throw std::logic_error("Verified character list unavailable");
    return characters_;
}
void WorldSession::requestPlayerLogin(size_t index, uint32_t timeoutMs) {
    if (state_ != State::Ready || characterState_ != CharacterState::Ready ||
        playerState_ != PlayerState::Empty)
        throw std::logic_error("Player login requires a current idle character list");
    if (!timeoutMs || timeoutMs > 60000)
        throw std::invalid_argument("Invalid player-login timeout");
    auto request = prepareCharacterLogin(characters_, index);
    queuePacket(request.packet.opcode, request.packet.payload);
    selectedCharacter_ = std::move(request.character);
    worldEntry_ = {};
    playerLoginResult_.reset();
    playerRequestEnd_ = queuedBytes_;
    playerDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    playerState_ = PlayerState::Pending;
}
const CharacterSummary &WorldSession::selectedCharacter() const {
    if (state_ != State::Ready || playerState_ == PlayerState::Empty)
        throw std::logic_error("Selected character unavailable");
    return selectedCharacter_;
}
const WorldEntry &WorldSession::worldEntry() const {
    if (state_ != State::Ready || playerState_ != PlayerState::Verified)
        throw std::logic_error("World-entry confirmation unavailable");
    return worldEntry_;
}
} // namespace wow::android
