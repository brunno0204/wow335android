#include "AuthSession.h"
#include "WorldSession.h"
#include "WorldLink.h"
#include "CharacterCustomization.h"
#include "AssetLock.h"
#include "WorldObjects.h"
#include "PlayerState.h"
#include "ServerEvents.h"
#include <array>
#include <chrono>
#include <jni.h>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
namespace {
using namespace wow::android;
// All calls come from the Java network executor, including cancellation. No GL
// or asset lock is held. Passwords/keys never appear in the UI snapshot or logs.
AuthSession login;
WorldSession world;
WorldPresence presence;
WorldObjects objects;
PlayerState player;
ServerEvents serverEvents;
uint32_t selfActions = 0; // animations received for the player (diagnostic line)
uint32_t interactSerial = 0;
PlayerFieldsView playerFieldsView;
uint32_t presenceSerial = 0, targetSerial = 0;
uint64_t steadyMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
uint32_t nowMs() {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
void endWorldLink() {
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    const auto serial = state.nextSerial;
    state = WorldLinkState{};
    state.nextSerial = serial; // serials stay monotonic across sessions
}
// Mirror a new server placement into the shared link (network executor only).
void publishPlacement() {
    if (presence.placement().serial == presenceSerial)
        return;
    presenceSerial = presence.placement().serial;
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    state.request = presence.placement();
    state.request.serial = ++state.nextSerial;
    state.requiredSerial = state.request.serial;
}
enum class Stage { Idle, Login, Realms, World, Characters, Player, Confirmed, Failed };
Stage stage = Stage::Idle;
std::string message = "Informe o servidor de autenticação, conta e senha.";
std::mutex mutex;
std::string realmDiagnostic, realmName;
// Character create/delete (CCharacterCreation::CreateCharacter / ClientServices).
enum class CharacterRequest { None, Create, Delete };
CharacterRequest characterRequest = CharacterRequest::None;
uint32_t characterResultSerial = 0;
int characterResult = -1;
std::string characterResultText(int code) {
    switch (code) {
    case 47: return "Personagem criado.";
    case 48: return "Erro ao criar o personagem.";
    case 49: return "Falha ao criar o personagem.";
    case 50: return "Esse nome já está em uso.";
    case 51: return "A criação de personagens está desativada.";
    case 52: return "Você não pode ter personagens da Aliança e da Horda neste reino PvP.";
    case 53: return "O reino atingiu o limite de personagens.";
    case 54: return "Você atingiu o limite de personagens da conta.";
    case 55: return "O servidor está com fila; tente novamente.";
    case 56: return "Só é possível criar personagens em reinos onde você já tem um.";
    case 57: return "Sua conta não tem a expansão necessária para esta raça.";
    case 58: return "Sua conta não tem a expansão necessária para esta classe.";
    case 59: return "Cavaleiro da Morte exige um personagem de nível 55 nesta conta.";
    case 60: return "Limite de personagens desta classe atingido.";
    case 62: return "Esta combinação de raça e classe está restrita.";
    case 71: return "Personagem apagado.";
    case 72: return "Falha ao apagar o personagem.";
    case 73: return "Personagem bloqueado para transferência.";
    case 74: return "Líderes de guilda não podem ser apagados.";
    case 75: return "Capitães de arena não podem ser apagados.";
    case 88: return "Nome inválido.";
    case 89: return "Digite um nome.";
    case 90: return "Nome curto demais.";
    case 91: return "Nome longo demais.";
    case 92: return "O nome só pode ter letras.";
    case 93: return "O nome mistura idiomas.";
    case 94: return "Nome não permitido.";
    case 95: return "Nome reservado.";
    case 98: return "O nome não pode repetir a mesma letra três vezes.";
    default: return "Resposta do servidor: " + std::to_string(code) + ".";
    }
}
constexpr std::array<uint8_t, 20> build12340WinHash{0xcd, 0xcb, 0xbd, 0x51, 0x88, 0x31, 0x5e,
                                                    0x6b, 0x4d, 0x19, 0x44, 0x9d, 0x49, 0x2d,
                                                    0xbc, 0xfa, 0xf1, 0x56, 0xa3, 0x47};
// Public protocol compatibility fingerprint from TrinityCore's build_executable_hash
// entry12340/Win. This is not a hash calculation/verification of installed files.
void wipe(void *data, size_t size) {
    auto *p = static_cast<volatile uint8_t *>(data);
    while (size--)
        *p++ = 0;
}
struct Wipe {
    void *data;
    size_t size;
    ~Wipe() { wipe(data, size); }
};
std::string input(JNIEnv *env, jstring value, size_t maximum) {
    if (!value || env->GetStringUTFLength(value) > jsize(maximum))
        throw std::invalid_argument("Campo ausente ou muito longo");
    const char *p = env->GetStringUTFChars(value, nullptr);
    if (!p)
        throw std::runtime_error("Falha ao ler campo");
    std::string text(p);
    env->ReleaseStringUTFChars(value, p);
    return text;
}
std::string quote(const std::string &value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += char(c);
    }
    return out + '"';
}
jstring utf8(JNIEnv *env, const std::string &value) {
    auto bytes = env->NewByteArray(jsize(value.size()));
    if (!bytes)
        return nullptr;
    env->SetByteArrayRegion(bytes, 0, jsize(value.size()),
                            reinterpret_cast<const jbyte *>(value.data()));
    auto type = env->FindClass("java/lang/String");
    auto constructor = env->GetMethodID(type, "<init>", "([BLjava/lang/String;)V");
    auto encoding = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(type, constructor, bytes, encoding));
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(encoding);
    env->DeleteLocalRef(type);
    return result;
}
// SMSG_MESSAGECHAT (0x96) / SMSG_GM_MESSAGECHAT (0x3b3) layout of the 3.3.5 server
// (ChatHandler::BuildChatPacket); SMSG_MOTD (0x33d) lines become system messages.
bool parseChat(const WorldPacket &packet, std::vector<ChatLine> &lines) {
    const auto &p = packet.payload;
    size_t o = 0;
    auto need = [&](size_t n) {
        if (o + n > p.size())
            throw std::invalid_argument("short chat packet");
    };
    auto u8 = [&]() { need(1); return p[o++]; };
    auto u32 = [&]() {
        need(4);
        const uint32_t v = uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 3]) << 24;
        o += 4;
        return v;
    };
    auto u64 = [&]() { const uint64_t low = u32(); return low | uint64_t(u32()) << 32; };
    auto cstr = [&]() {
        const size_t start = o;
        while (o < p.size() && p[o])
            ++o;
        need(1);
        std::string text(p.begin() + long(start), p.begin() + long(o));
        ++o;
        return text;
    };
    try {
        if (packet.opcode == 0x33d) {
            const uint32_t count = u32();
            for (uint32_t i = 0; i < count && i < 32; ++i) {
                ChatLine line;
                line.text = cstr();
                lines.push_back(std::move(line));
            }
            return true;
        }
        if (packet.opcode != 0x96 && packet.opcode != 0x3b3)
            return false;
        ChatLine line;
        line.type = u8();
        line.language = u32();
        line.sender = u64();
        u32();
        switch (line.type) {
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x29: case 0x2a: case 0x2f: {
            u32();
            line.author = cstr();
            const uint64_t target = u64();
            if (target && (target >> 48) != 0 && (target >> 48) != 0xf140) {
                u32();
                cstr();
            }
            break;
        }
        case 0x08:
            u32();
            line.author = cstr();
            u64();
            break;
        case 0x24: case 0x25: case 0x26: {
            const uint64_t target = u64();
            if (target && (target >> 48) != 0) {
                u32();
                cstr();
            }
            break;
        }
        case 0x30: case 0x31:
            u64();
            break;
        default:
            if (packet.opcode == 0x3b3) {
                u32();
                line.author = cstr();
            }
            if (line.type == 0x11)
                line.channel = cstr();
            u64();
        }
        u32();
        line.text = cstr();
        if (line.author.empty() && line.sender)
            line.author = objects.nameOf(line.sender);
        lines.push_back(std::move(line));
        return true;
    } catch (const std::exception &) {
        return true; // malformed: dropped
    }
}
// What the original client sends when the player interacts with an object (CGUnit_C::OnRightClick,
// CGGameObject_C::OnRightClick): loot a lootable corpse, talk to an NPC by its flags, use a game
// object, or start attacking anything else that is not a player.
std::vector<WorldPacket> interactPackets(uint64_t guid) {
    std::vector<WorldPacket> packets;
    WorldObjects::Interaction info;
    if (!objects.describe(guid, info))
        return packets;
    auto make = [&](uint32_t opcode) {
        WorldPacket packet{opcode, {}};
        for (unsigned i = 0; i < 8; ++i)
            packet.payload.push_back(uint8_t(guid >> (8 * i)));
        packets.push_back(std::move(packet));
    };
    if (info.type == ObjectType::GameObject) {
        make(0x0b1); // CMSG_GAMEOBJ_USE
        return packets;
    }
    if (info.type != ObjectType::Unit && info.type != ObjectType::Player)
        return packets;
    if (info.dead) {
        if (info.dynamicFlags & 0x1) // UNIT_DYNFLAG_LOOTABLE
            make(0x15d);             // CMSG_LOOT
        return packets;
    }
    const uint32_t npc = info.npcFlags;
    if (npc & 0x1) // UNIT_NPC_FLAG_GOSSIP: the server offers every other service from the menu
        make(0x17b);
    else if (npc & 0x2) // QUESTGIVER
        make(0x184);
    else if (npc & (0x80 | 0x1000)) // VENDOR / REPAIR
        make(0x19e);
    else if (npc & 0x10) // TRAINER
        make(0x1b0);
    else if (npc & 0x4000) // SPIRIT_HEALER
        make(0x21c);
    else if (npc & 0x20000) // BANKER
        make(0x1b7);    // CMSG_BANKER_ACTIVATE
    else if (info.type == ObjectType::Unit && !npc)
        make(0x141); // CMSG_ATTACK_SWING
    return packets;
}
void cancel() {
    login.cancel();
    world.cancel();
    presence.reset();
    objects.reset(0);
    player.reset();
    serverEvents.reset(0);
    playerFieldsView = {};
    presenceSerial = 0;
    targetSerial = 0;
    characterRequest = CharacterRequest::None;
    endWorldLink();
    stage = Stage::Idle;
    realmDiagnostic.clear();
}
void failed(const std::string &reason) {
    const auto context = realmDiagnostic;
    cancel();
    stage = Stage::Failed;
    message = reason + (context.empty() ? "" : "\n" + context);
}
std::string snapshot() {
    std::ostringstream out;
    out << "{\"stage\":" << int(stage) << ",\"message\":" << quote(message) << ",\"realms\":[";
    if (stage == Stage::Realms) {
        bool first = true;
        for (const auto &r : login.realms().realms) {
            if (!first)
                out << ',';
            first = false;
            out << quote(r.name);
        }
    }
    out << "],\"characters\":[";
    if (stage == Stage::Characters) {
        bool first = true;
        for (const auto &c : world.characters().characters) {
            if (!first)
                out << ',';
            first = false;
            out << quote(c.name + " — nível " + std::to_string(c.level));
        }
    }
    out << "]";
    if (stage == Stage::Characters) {
        out << ",\"looks\":[";
        bool first = true;
        for (const auto &c : world.characters().characters) {
            if (!first)
                out << ',';
            first = false;
            out << '[' << int(c.race) << ',' << int(c.classId) << ',' << int(c.sex) << ','
                << int(c.skin) << ',' << int(c.face) << ',' << int(c.hairStyle) << ','
                << int(c.hairColor) << ',' << int(c.facialHair);
            for (size_t slot = 0; slot < 19; ++slot)
                out << ',' << c.equipment[slot].display;
            for (size_t slot = 15; slot < 18; ++slot)
                out << ',' << int(c.equipment[slot].inventoryType);
            out << ']';
        }
        const auto &status = world.status();
        out << "],\"expansion\":" << int(status.hasBilling ? status.expansion : 2)
            << ",\"pending\":" << (characterRequest != CharacterRequest::None ? "true" : "false");
    }
    out << ",\"result\":" << characterResult << ",\"resultSerial\":" << characterResultSerial;
    if (stage == Stage::Confirmed) {
        const size_t work = worldAssetWork(); // takes the link lock itself
        std::lock_guard<std::mutex> link(worldLinkMutex());
        const auto &state = worldLink();
        out << ",\"world\":{\"serial\":" << state.request.serial
            << ",\"loaded\":" << state.loadedSerial << ",\"adopted\":" << state.adoptedSerial
            << ",\"terrain\":" << quote(state.terrainStatus) << ",\"work\":" << work
            << ",\"hp\":" << state.hud.health << ",\"maxhp\":" << state.hud.maxHealth
            << ",\"power\":" << state.hud.power << ",\"maxpower\":" << state.hud.maxPower
            << ",\"powertype\":" << int(state.hud.powerType) << ",\"level\":" << state.hud.level
            << ",\"name\":" << quote(world.state() == WorldSession::State::Ready &&
                                              world.playerState() != WorldSession::PlayerState::Empty
                                          ? world.selectedCharacter().name
                                          : std::string())
            << '}';
    }
    out << '}';
    return out.str();
}
std::string worldAuthResultText(uint8_t result) {
    std::string text = std::to_string(result);
    if (result == 14) text += " AUTH_REJECT (recusa genérica)";
    else if (result == 20) text += " AUTH_VERSION_MISMATCH";
    else if (result == 13) text += " AUTH_FAILED";
    return text;
}
void poll() {
    if (stage == Stage::Login) {
        login.pump(0);
        if (login.state() == AuthSession::State::Failed) {
            auto result = login.serverResult();
            failed("Autenticação recusada/falhou. Erro " + std::to_string(int(login.error())) +
                   (result ? "; código do servidor " + std::to_string(*result) : "") +
                   ". Confirme endereço, conta e cliente 3.3.5a12340.");
        } else if (login.state() == AuthSession::State::Ready) {
            stage = Stage::Realms;
            message = "Autenticado. Escolha um reino.";
        }
    } else if (stage == Stage::World || stage == Stage::Characters || stage == Stage::Player ||
               stage == Stage::Confirmed) {
        world.pump(0);
        if (world.state() == WorldSession::State::Failed) {
            auto result = world.playerLoginResult();
            auto auth = world.serverResult();
            if (world.error() == WorldSession::Error::Network && !auth) {
                failed("Não foi possível conectar ao servidor do reino. Se o endereço do reino for "
                       "127.0.0.1 ou localhost, o servidor está anunciando o próprio PC: troque "
                       "realmlist.address no banco acore_auth pelo IP do PC na rede (veja "
                       "local-server/LEIA-ME.md) e libere a porta 8085 no firewall.");
                return;
            }
            failed("Conexão com o mundo encerrada. Erro " + std::to_string(int(world.error())) +
                   (result ? "; personagem " + std::to_string(*result)
                           : (auth ? "; servidor " + worldAuthResultText(*auth) : "")) +
                   (world.diagnostic().empty() ? "" : " (" + world.diagnostic() + ")"));
            return;
        }
        if (stage == Stage::World && world.state() == WorldSession::State::Ready) {
            if (world.characterState() == WorldSession::CharacterState::Empty) {
                world.requestCharacters();
                message = "Autenticado no mundo. Buscando personagens…";
            }
            if (world.characterState() == WorldSession::CharacterState::Ready) {
                stage = Stage::Characters;
                if (characterResult != 47 && characterResult != 71)
                    message = "Escolha um personagem.";
            }
        }
        if (stage == Stage::Player && world.playerState() == WorldSession::PlayerState::Verified) {
            const auto &entry = world.worldEntry();
            const auto &hero = world.selectedCharacter();
            stage = Stage::Confirmed;
            presence.begin(hero.guid, entry, nowMs());
            objects.reset(hero.guid);
            player.reset();
            serverEvents.reset(hero.guid);
            playerFieldsView = {};
            {
                std::array<uint32_t, 19> displays{};
                for (size_t i = 0; i < 19; ++i)
                    displays[i] = hero.equipment[i].display;
                objects.setSelfEquipment(displays);
            }
            {
                std::lock_guard<std::mutex> link(worldLinkMutex());
                auto &state = worldLink();
                state.active = true;
                state.guid = hero.guid;
                state.race = hero.race;
                state.sex = hero.sex;
                state.skin = hero.skin;
                state.hairColor = hero.hairColor;
                state.cls = hero.classId;
                state.face = hero.face;
                state.hairStyle = hero.hairStyle;
                state.facialHair = hero.facialHair;
                for (size_t i = 0; i < 19; ++i) {
                    state.equipment[i] = hero.equipment[i].display;
                    state.equipmentTypes[i] = hero.equipment[i].inventoryType;
                }
                state.playerName = hero.name;
                state.realmName = realmName;
                state.zoneId = hero.zone;
                state.mapId = entry.map;
                state.terrainStatus = "Carregando o mapa…";
            }
            publishPlacement();
        }
        if (world.state() == WorldSession::State::Ready) {
            const auto now = nowMs();
            for (const auto &packet : world.takePackets()) {
                if (packet.opcode == 0x2e6) {
                    failed("O servidor exige Warden (anti-cheat), que este port não executa. "
                           "No seu servidor de teste use Warden.Enabled = 0 no worldserver.conf.");
                    return;
                }
                if (stage == Stage::Characters &&
                    (packet.opcode == 0x3a || packet.opcode == 0x3c) && packet.payload.size() == 1 &&
                    characterRequest == (packet.opcode == 0x3a ? CharacterRequest::Create
                                                               : CharacterRequest::Delete)) {
                    characterRequest = CharacterRequest::None;
                    characterResult = packet.payload[0];
                    ++characterResultSerial;
                    message = characterResultText(characterResult);
                    if (characterResult == 47 || characterResult == 71) {
                        world.requestCharacters();
                        stage = Stage::World; // back to Characters when the new list arrives
                    }
                    continue;
                }
                if (stage != Stage::Confirmed)
                    continue;
                for (const auto &reply : presence.handle(packet, now))
                    world.send(reply.opcode, reply.payload);
                std::vector<ChatLine> chat;
                if (parseChat(packet, chat) && !chat.empty()) {
                    std::lock_guard<std::mutex> link(worldLinkMutex());
                    auto &queue = worldLink().chat;
                    for (auto &line : chat)
                        if (queue.size() < 200)
                            queue.push_back(std::move(line));
                }
                std::vector<WorldPacket> queries;
                player.handle(packet, queries);
                serverEvents.handle(packet, [&](uint64_t guid) { return objects.nameOf(guid); }, queries);
                if (packet.opcode == 0x058) // SMSG_ITEM_QUERY_SINGLE_RESPONSE: full template for the UI
                    try {
                        player.storeItemTemplate(parseItemTemplate(packet.payload));
                    } catch (const std::exception &) {
                    }
                objects.handle(packet, steadyMs(), queries);
                for (const auto &query : queries)
                    world.send(query.opcode, query.payload);
            }
            if (stage == Stage::Confirmed) {
                // Quest giver marks (CGObject_C interact icons): the client asks for the status of
                // every visible quest giver (CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY).
                static uint64_t questStatusAt = 0;
                if (now - questStatusAt > 3000) {
                    questStatusAt = now;
                    world.send(0x417, {});
                }
                uint64_t target = 0, interact = 0;
                bool selection = false;
                // Player data for the original UI: immutable snapshots, built outside the lock.
                std::shared_ptr<const PlayerFieldsView> fieldsSnapshot;
                std::shared_ptr<const PlayerDataView> dataSnapshot;
                {
                    const bool selfChanged = objects.takeSelfChanged();
                    const bool itemsChanged = objects.takeItemsChanged();
                    if (selfChanged || itemsChanged) {
                        objects.playerFields(playerFieldsView, itemsChanged);
                        if (selfChanged)
                            ++playerFieldsView.fieldsSerial;
                        if (itemsChanged)
                            ++playerFieldsView.itemsSerial;
                        fieldsSnapshot = std::make_shared<PlayerFieldsView>(playerFieldsView);
                        std::vector<WorldPacket> questQueries;
                        player.requestQuests(playerFieldsView.fields, questQueries);
                        for (const auto &query : questQueries)
                            world.send(query.opcode, query.payload);
                    }
                    if (player.changed()) {
                        player.clearChanged();
                        dataSnapshot = std::make_shared<PlayerDataView>(player.view());
                    }
                }
                std::vector<WorldPacket> outgoing;
                std::vector<uint32_t> wantItems;
                {
                    std::lock_guard<std::mutex> link(worldLinkMutex());
                    auto &state = worldLink();
                    for (auto &event : player.takeEvents())
                        if (state.scriptEvents.size() < 4096)
                            state.scriptEvents.push_back(std::move(event));
                    if (serverEvents.gameTimeSerial() != state.gameTimeSerial) {
                        state.gameTimeSerial = serverEvents.gameTimeSerial();
                        state.gameHalfMinutes = serverEvents.gameHalfMinutes();
                    }
                    for (auto &action : serverEvents.takeActions()) {
                        if (action.guid == state.guid)
                            ++selfActions;
                        if (state.unitActions.size() < 512)
                            state.unitActions.push_back(action);
                    }
                    for (auto &event : serverEvents.take())
                        if (state.scriptEvents.size() < 4096)
                            state.scriptEvents.push_back(std::move(event));
                    if (state.interactSerial != interactSerial) {
                        interactSerial = state.interactSerial;
                        interact = state.interact;
                    }
                    if (fieldsSnapshot)
                        state.playerFields = std::move(fieldsSnapshot);
                    if (dataSnapshot)
                        state.playerData = std::move(dataSnapshot);
                    outgoing.swap(state.outgoing);
                    wantItems.swap(state.wantItems);
                    if (objects.changed()) {
                        state.units = objects.snapshot(steadyMs());
                        ++state.unitsSerial;
                    }
                    state.hud = objects.hud();
                    state.runSpeed = presence.runSpeed();
                    if (state.targetSerial != targetSerial) {
                        targetSerial = state.targetSerial;
                        target = state.target;
                        selection = true;
                    }
                }
                for (const auto &packet : outgoing) {
                    player.noteOutgoing(packet);
                    world.send(packet.opcode, packet.payload);
                }
                if (!wantItems.empty()) {
                    for (uint32_t entry : wantItems)
                        objects.wantItem(entry);
                    std::vector<WorldPacket> queries;
                    objects.flushQueries(queries);
                    for (const auto &query : queries)
                        world.send(query.opcode, query.payload);
                }
                if (selection) {
                    std::vector<uint8_t> payload;
                    for (unsigned i = 0; i < 8; ++i)
                        payload.push_back(uint8_t(target >> (8 * i)));
                    world.send(0x13d, payload); // CMSG_SET_SELECTION
                }
                if (interact)
                    for (const auto &packet : interactPackets(interact))
                        world.send(packet.opcode, packet.payload);
            }
            if (stage == Stage::Confirmed) {
                if (presence.loggedOut()) {
                    failed("O servidor encerrou a sessão do personagem.");
                    return;
                }
                publishPlacement();
                MoverSample sample;
                std::string terrain;
                {
                    std::lock_guard<std::mutex> link(worldLinkMutex());
                    const auto &state = worldLink();
                    if (state.sampleSerial >= state.requiredSerial)
                        sample = state.sample;
                    terrain = state.terrainStatus;
                }
                for (const auto &reply : presence.tick(sample, now))
                    world.send(reply.opcode, reply.payload);
                const auto &pos = presence.lastSent();
                std::ostringstream text;
                text.setf(std::ios::fixed);
                text.precision(1);
                text << world.selectedCharacter().name << " — mapa " << presence.placement().map
                     << " (" << pos.x << ", " << pos.y << ", " << pos.z << ")"
                     << "; latência " << presence.latency() << " ms; movimentos enviados "
                     << presence.movementPackets() << "; animações " << selfActions << "\n" << presence.notice()
                     << (terrain.empty() ? "" : " · " + terrain);
                message = text.str();
            }
        }
    }
}
} // namespace
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_networkConnect(
    JNIEnv *env, jclass, jstring endpoint, jstring account, jcharArray secret) {
    std::lock_guard<std::mutex> lock(mutex);
    std::string password;
    try {
        cancel();
        message = "Conectando…";
        if (!secret || env->GetArrayLength(secret) < 1 || env->GetArrayLength(secret) > 255)
            throw std::invalid_argument("Senha deve ter1..255 caracteres ASCII");
        std::array<jchar, 255> chars{};
        Wipe cleanChars{chars.data(), sizeof(chars)};
        const auto count = env->GetArrayLength(secret);
        env->GetCharArrayRegion(secret, 0, count, chars.data());
        for (jsize i = 0; i < count; ++i) {
            if (chars[i] < 32 || chars[i] > 126) {
                wipe(chars.data(), sizeof(chars));
                throw std::invalid_argument("Senha ASCII necessária nesta versão");
            }
            password += char(chars[i]);
        }
        wipe(chars.data(), sizeof(chars));
        const auto address = parseServerEndpoint(input(env, endpoint, 255));
        const auto name = input(env, account, 255);
        auto resolved = resolveServerEndpoint(address);
        login.begin(
            resolved, name, password, [](const auto &) { return build12340WinHash; }, 10000, 15000);
        wipe(password.data(), password.size());
        stage = Stage::Login;
    } catch (const std::exception &error) {
        wipe(password.data(), password.size());
        failed(error.what());
    }
    return utf8(env, snapshot());
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_wowdecomp_android_MainActivity_networkPoll(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        poll();
    } catch (const std::exception &error) {
        failed(error.what());
    }
    return utf8(env, snapshot());
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_wowdecomp_android_MainActivity_networkChoose(JNIEnv *env, jclass, jint kind, jint index) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        if (index < 0)
            throw std::invalid_argument("Seleção inválida");
        if (kind == 0 && stage == Stage::Realms) {
            const auto &realm = login.realms().realms.at(size_t(index));
            realmName = realm.name;
            std::ostringstream detail;
            detail << "Reino: " << realm.name << " (" << realm.address << "); flags=0x" << std::hex << unsigned(realm.flags)
                   << "; indicadores=0x" << unsigned(realm.locked);
            if (realm.version) {
                const auto &v = *realm.version;
                detail << std::dec << "; versão anunciada=" << unsigned(v.major) << '.'
                       << unsigned(v.minor) << '.' << unsigned(v.patch) << " build " << v.build;
            } else detail << std::dec << "; versão anunciada=ausente";
            detail << "; cliente=3.3.5 build " << worldClientBuildForRealm(realm) << "; auth=12340";
            if (worldClientBuildForRealm(realm) == 12341) detail << "; perfil experimental 12341";
            realmDiagnostic = detail.str();
            world.begin(login.selectRealm(size_t(index)));
            stage = Stage::World;
            message = "Conectando ao reino…\n" + realmDiagnostic;
        } else if (kind == 1 && stage == Stage::Characters) {
            world.requestPlayerLogin(size_t(index));
            stage = Stage::Player;
            message = "Solicitando entrada do personagem…";
        } else
            throw std::invalid_argument("Aguarde a etapa atual");
    } catch (const std::invalid_argument &error) {
        if ((stage == Stage::Realms && login.state() == AuthSession::State::Ready) ||
            (stage == Stage::Characters && world.state() == WorldSession::State::Ready))
            message = error.what();
        else
            failed(error.what());
    } catch (const std::out_of_range &error) {
        message = error.what();
    } catch (const std::exception &error) {
        failed(error.what());
    }
    return utf8(env, snapshot());
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_wowdecomp_android_MainActivity_networkDisconnect(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lock(mutex);
    cancel();
    message = "Desconectado.";
    return utf8(env, snapshot());
}

// CMSG_CHAR_CREATE(0x36): name, race, class, sex, skin, face, hairStyle, hairColor,
// facialHair, outfit (ClientConnection::CharacterCreate).
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_networkCreateCharacter(
    JNIEnv *env, jclass, jstring name, jintArray values) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        if (stage != Stage::Characters || world.state() != WorldSession::State::Ready ||
            characterRequest != CharacterRequest::None)
            throw std::invalid_argument("Aguarde a lista de personagens");
        const auto text = input(env, name, 48);
        if (!characterNameLooksValid(text))
            throw std::invalid_argument("Nome inválido: use de 2 a 12 letras, sem repetir a mesma letra três vezes.");
        jint v[8]{};
        if (!values || env->GetArrayLength(values) != 8)
            throw std::invalid_argument("Aparência inválida");
        env->GetIntArrayRegion(values, 0, 8, v);
        std::vector<uint8_t> payload(text.begin(), text.end());
        payload.push_back(0);
        for (jint value : v) {
            if (value < 0 || value > 255)
                throw std::invalid_argument("Aparência inválida");
            payload.push_back(uint8_t(value));
        }
        payload.push_back(0); // outfit
        world.send(0x36, payload);
        characterRequest = CharacterRequest::Create;
        message = "Criando personagem…";
    } catch (const std::exception &error) {
        message = error.what();
    }
    return utf8(env, snapshot());
}
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_networkDeleteCharacter(
    JNIEnv *env, jclass, jint index) {
    std::lock_guard<std::mutex> lock(mutex);
    try {
        if (stage != Stage::Characters || world.state() != WorldSession::State::Ready ||
            characterRequest != CharacterRequest::None)
            throw std::invalid_argument("Aguarde a lista de personagens");
        const auto &list = world.characters().characters;
        if (index < 0 || size_t(index) >= list.size())
            throw std::invalid_argument("Personagem inválido");
        std::vector<uint8_t> payload;
        for (int i = 0; i < 8; ++i)
            payload.push_back(uint8_t(list[size_t(index)].guid >> (8 * i)));
        world.send(0x38, payload);
        characterRequest = CharacterRequest::Delete;
        message = "Apagando personagem…";
    } catch (const std::exception &error) {
        message = error.what();
    }
    return utf8(env, snapshot());
}
