// Glue (login / character select / character create) 3D scenes and the character
// creator. Asset-worker JNI entry points build owned scene data; the GL thread adopts
// it in AndroidBridge.cpp (glueSceneFrame/glueSceneDraw). No GL calls here.
#include "GlueBridgeInternal.h"
#include "WorldScene.h"
#include "CharacterCustomization.h"
#include "ClientData.h"
#include "GlueScene.h"
#include "M2File.h"
#include "TextureDecoder.h"
#include <algorithm>
#include <android/log.h>
#include <cstdio>
#include "ClientDb.h"
#include "WorldAssets.h"
#include <cmath>
#include <jni.h>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
namespace wow::android {
std::mutex &glueSceneMutex() {
    static std::mutex m;
    return m;
}
std::unique_ptr<GlueSceneData> &pendingGlueScene() {
    static std::unique_ptr<GlueSceneData> p;
    return p;
}
std::atomic<float> &glueFacing() {
    static std::atomic<float> f{0};
    return f;
}
} // namespace wow::android
namespace {
using namespace wow::android;
// Asset worker only (MainActivity's single-thread executor).
std::unique_ptr<CharacterCustomization> creator;
std::string creatorRoot;
CharacterLook look;
uint8_t expansion = 2;
std::mt19937 rng{std::random_device{}()};
uint32_t sceneSerial = 0;
std::string javaText(JNIEnv *env, jstring value) {
    if (!value)
        throw std::invalid_argument("Missing text");
    const char *bytes = env->GetStringUTFChars(value, nullptr);
    if (!bytes)
        throw std::runtime_error("Cannot read text");
    std::string text(bytes);
    env->ReleaseStringUTFChars(value, bytes);
    return text;
}
jstring utf8(JNIEnv *env, const std::string &value) {
    auto bytes = env->NewByteArray(jsize(value.size()));
    if (!bytes)
        return nullptr;
    env->SetByteArrayRegion(bytes, 0, jsize(value.size()), reinterpret_cast<const jbyte *>(value.data()));
    auto type = env->FindClass("java/lang/String");
    auto constructor = env->GetMethodID(type, "<init>", "([BLjava/lang/String;)V");
    auto encoding = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(type, constructor, bytes, encoding));
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(encoding);
    env->DeleteLocalRef(type);
    return result;
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
std::string skinFor(const std::string &model) { return model.substr(0, model.size() - 3) + "00.skin"; }
// Caller holds a LocalClientData mount.
std::unique_ptr<M2ModelPreview> loadBackground(const std::string &path, M2SceneInfo &info) {
    const auto bytes = m2::readFile(path);
    info = decodeM2Scene(bytes);
    auto preview = std::make_unique<M2ModelPreview>(M2AnimatedModel::load(path, skinFor(path), {0}, true));
    try {
        preview->setEffects(loadM2Effects(bytes, 0)); // animated UVs/colors and particle emitters
    } catch (const std::exception &) {
    }
    return preview;
}
// GlueXML text of the login screen (caller holds the mount).
std::string glueXmlText() {
    std::string text;
    for (const char *file : {"Interface\\GlueXML\\AccountLogin.lua", "Interface\\GlueXML\\AccountLogin.xml",
                             "Interface\\GlueXML\\GlueParent.lua"}) {
        try {
            const auto bytes = m2::readFile(file, 4 * 1024 * 1024);
            text.append(bytes.begin(), bytes.end());
            text += '\n';
        } catch (const std::exception &) {
        }
    }
    return text;
}
std::string loginModelPath() {
    if (auto found = findGlueModelPath(glueXmlText()))
        return *found;
    return "Interface\\Glues\\Models\\UI_MainMenu_Northrend\\UI_MainMenu_Northrend.m2";
}
// Caller holds a LocalClientData mount. Composes the CCharacterComponent base texture.
std::unique_ptr<M2ModelPreview> loadCharacter(const CharacterLook &l, std::string &note,
                                              const std::vector<uint32_t> &slots = {0}) {
    const auto model = creator->modelPath(l.race, l.sex);
    const auto recipe = creator->textures(l);
    M2TextureReplacements bindings;
    if (!recipe.baseSkin.empty()) {
        bindings[1] = recipe.baseSkin;
        bindings[8] = recipe.extraSkin.empty() ? recipe.baseSkin : recipe.extraSkin;
    }
    if (!recipe.hair.empty())
        bindings[6] = recipe.hair;
    auto preview = std::make_unique<M2ModelPreview>(
        M2AnimatedModel::load(model, skinFor(model), slots, true, bindings));
    auto available = preview->geosetCatalog()->sections;
    std::sort(available.begin(), available.end());
    std::vector<uint32_t> ids;
    for (uint32_t id : creator->geosets(l))
        if (std::binary_search(available.begin(), available.end(), id))
            ids.push_back(id);
    preview->installGeosets(prepareM2Geosets(preview->geosetCatalog(), ids));
    if (recipe.baseSkin.empty())
        return preview;
    try {
        const auto base = decodeBlp2(m2::readFile(recipe.baseSkin, 16 * 1024 * 1024));
        std::vector<std::pair<CharacterSection, TexturePixels>> layers;
        for (const auto &layer : recipe.layers) {
            try {
                layers.push_back({layer.section, decodeBlp2(m2::readFile(layer.path, 16 * 1024 * 1024))});
            } catch (const std::exception &) {
                note += " Camada ausente: " + layer.path + ".";
            }
        }
        auto composed = std::make_shared<const TexturePixels>(composeCharacterTexture(base, layers));
        auto materials = std::make_shared<M2PreviewMaterials>(*preview->model().previewMaterials());
        for (auto &texture : materials->textures)
            if (texture.type == 1)
                texture.pixels = composed;
        preview->installAppearance(std::move(materials));
    } catch (const std::exception &error) {
        note += std::string(" Textura do corpo simples: ") + error.what() + ".";
    }
    return preview;
}
void publish(std::unique_ptr<GlueSceneData> scene) {
    scene->serial = ++sceneSerial;
    std::unique_ptr<GlueSceneData> retired;
    {
        std::lock_guard<std::mutex> lock(glueSceneMutex());
        retired = std::move(pendingGlueScene());
        pendingGlueScene() = std::move(scene);
    }
}
void ensureCreator(const std::string &root) {
    if (creator && creatorRoot == root)
        return;
    CharacterDbcFiles files;
    std::vector<uint8_t> *slots[] = {&files.chrRaces,         &files.chrClasses,
                                     &files.charBaseInfo,     &files.charSections,
                                     &files.charHairGeosets,  &files.facialHairStyles,
                                     &files.creatureDisplayInfo, &files.creatureModelData,
                                     &files.factionTemplate,  &files.factionGroup,
                                     &files.nameGen};
    {
        const auto mount = sharedClientData(root);
        const auto &install = *mount;
        m2::require(install.report().ready, "Local game data is incomplete");
        for (size_t i = 0; i < characterDbcPaths().size(); ++i)
            *slots[i] = m2::readFile(characterDbcPaths()[i], 32 * 1024 * 1024);
    }
    creator = std::make_unique<CharacterCustomization>(files);
    creatorRoot = root;
}
// Background + character for select/create. look must be validated.
std::string buildCharacterScene(const std::string &root, const CharacterLook &l, bool resetFacing,
                                const CharacterAppearance *gear = nullptr) {
    auto scene = std::make_unique<GlueSceneData>();
    std::string note;
    {
        const auto mount = sharedClientData(root);
        const auto &install = *mount;
        m2::require(install.report().ready, "Local game data is incomplete");
        auto path = creator->backgroundModel(l.race, l.cls);
        try {
            scene->background = loadBackground(path, scene->info);
        } catch (const std::exception &error) {
            note += " Cenário " + path + " indisponível (" + error.what() + ").";
            path = loginModelPath();
            scene->background = loadBackground(path, scene->info);
        }
        if (gear) {
            try {
                auto dressed = loadDressedCharacter(root, *gear, {0}, note);
                scene->character = std::move(dressed->body);
                for (auto &item : dressed->items)
                    scene->items.emplace_back(std::move(item.model), item.attachment);
            } catch (const std::exception &error) {
                note += std::string(" Equipamento indisponível: ") + error.what() + ".";
            }
        }
        if (!scene->character)
            scene->character = loadCharacter(l, note);
    }
    scene->resetFacing = resetFacing;
    publish(std::move(scene));
    return note;
}
std::string lookJson(const CharacterLook &l) {
    std::ostringstream o;
    o << '[' << int(l.race) << ',' << int(l.cls) << ',' << int(l.sex) << ',' << int(l.skin) << ','
      << int(l.face) << ',' << int(l.hairStyle) << ',' << int(l.hairColor) << ',' << int(l.facialHair)
      << ']';
    return o.str();
}
std::string creatorState(const std::string &status, const std::string &name) {
    std::ostringstream o;
    o << "{\"races\":[";
    bool first = true;
    for (const auto &r : creator->races()) {
        if (!first)
            o << ',';
        first = false;
        const auto &shown = look.sex ? (r.nameFemale.empty() ? r.name : r.nameFemale)
                                     : (r.nameMale.empty() ? r.name : r.nameMale);
        o << "{\"id\":" << r.id << ",\"name\":" << quote(shown) << ",\"file\":" << quote(r.fileString)
          << ",\"faction\":" << r.faction << ",\"enabled\":" << (r.requiredExpansion <= expansion ? "true" : "false")
          << '}';
    }
    o << "],\"classes\":[";
    first = true;
    for (const auto &c : creator->classesForRace(look.race)) {
        if (!first)
            o << ',';
        first = false;
        const auto &shown = look.sex ? (c.nameFemale.empty() ? c.name : c.nameFemale)
                                     : (c.nameMale.empty() ? c.name : c.nameMale);
        o << "{\"id\":" << c.id << ",\"name\":" << quote(shown) << ",\"file\":" << quote(c.filename)
          << ",\"enabled\":" << (c.requiredExpansion <= expansion ? "true" : "false") << '}';
    }
    const auto *r = creator->race(look.race);
    o << "],\"look\":" << lookJson(look) << ",\"facial\":" << quote(r ? r->facialToken[look.sex] : "")
      << ",\"hair\":" << quote(r ? r->hairToken : "") << ",\"counts\":[" << creator->numSkins(look) << ','
      << creator->numFaces(look) << ',' << creator->numHairStyles(look) << ','
      << creator->numHairColors(look) << ',' << creator->numFacialHair(look)
      << "],\"name\":" << quote(name) << ",\"status\":" << quote(status) << '}';
    return o.str();
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_wowdecomp_android_MainActivity_loadLoginScene(JNIEnv *env, jclass, jstring dataRoot) {
    try {
        const auto root = javaText(env, dataRoot);
        auto scene = std::make_unique<GlueSceneData>();
        std::string failure;
        {
            const auto mount = sharedClientData(root);
            const auto &install = *mount;
            m2::require(install.report().ready, "Local game data is incomplete");
            const std::string wanted = loginModelPath();
            for (const auto &path : {wanted,
                                     std::string("Interface\\Glues\\Models\\UI_MainMenu_Northrend\\UI_MainMenu_Northrend.m2"),
                                     std::string("Interface\\Glues\\Models\\UI_MainMenu\\UI_MainMenu.m2")}) {
                try {
                    scene->background = loadBackground(path, scene->info);
                    break;
                } catch (const std::exception &error) {
                    __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Login model %s: %s", path.c_str(), error.what());
                    if (failure.empty())
                        failure = path + ": " + error.what();
                }
            }
            if (!scene->background)
                throw std::runtime_error(failure);
        }
        scene->resetFacing = true;
        publish(std::move(scene));
        if (!failure.empty())
            return utf8(env, "Aviso: cenário principal não carregou (" + failure + "); usando outro.");
        return nullptr;
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Login scene: %s", error.what());
        return utf8(env, std::string("Cenário 3D do login indisponível: ") + error.what());
    }
}

// look: race,class,sex,skin,face,hairStyle,hairColor,facialHair. race 0 = empty scene.
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_loadSelectScene(
    JNIEnv *env, jclass, jstring dataRoot, jintArray values) {
    try {
        const auto root = javaText(env, dataRoot);
        ensureCreator(root);
        jint v[30]{};
        const jsize count = values ? env->GetArrayLength(values) : 0;
        if (count != 8 && count != 30)
            throw std::invalid_argument("Invalid character appearance");
        env->GetIntArrayRegion(values, 0, count, v);
        if (!v[0]) {
            auto scene = std::make_unique<GlueSceneData>();
            {
                const auto mount = sharedClientData(root);
        const auto &install = *mount;
                m2::require(install.report().ready, "Local game data is incomplete");
                const auto path = creator->backgroundModel(2, 1); // GetSelectBackgroundModel: Orc
                scene->background = loadBackground(path.empty() ? loginModelPath() : path, scene->info);
            }
            scene->resetFacing = true;
            publish(std::move(scene));
            return nullptr;
        }
        CharacterLook l{uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2]), uint8_t(v[3]),
                        uint8_t(v[4]), uint8_t(v[5]), uint8_t(v[6]), uint8_t(v[7])};
        std::optional<CharacterAppearance> gear;
        if (count == 30) {
            gear.emplace();
            gear->race = l.race;
            gear->cls = l.cls;
            gear->sex = l.sex;
            gear->skin = l.skin;
            gear->face = l.face;
            gear->hairStyle = l.hairStyle;
            gear->hairColor = l.hairColor;
            gear->facialHair = l.facialHair;
            for (size_t i = 0; i < 19; ++i)
                gear->displays[i] = uint32_t(v[8 + i]);
            for (size_t i = 0; i < 3; ++i)
                gear->inventoryTypes[15 + i] = uint8_t(v[27 + i]);
        }
        const auto note = buildCharacterScene(root, l, true, gear ? &*gear : nullptr);
        return note.empty() ? nullptr : utf8(env, note);
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Select scene: %s", error.what());
        return utf8(env, std::string("Prévia 3D do personagem indisponível: ") + error.what());
    }
}

// op: 0 reset(arg=expansion) 1 race(arg) 2 class(arg) 3 sex(arg) 4 cycle(arg=type,arg2=delta)
//     5 randomize appearance 6 random name 7 state only
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_creatorCommand(
    JNIEnv *env, jclass, jstring dataRoot, jint op, jint arg, jint arg2) {
    try {
        const auto root = javaText(env, dataRoot);
        ensureCreator(root);
        std::string name;
        bool rebuild = true, resetFacing = false;
        switch (op) {
        case 0:
            expansion = uint8_t(std::clamp<int>(arg, 0, 2));
            look = creator->randomCharacter(expansion, rng);
            resetFacing = true;
            name = creator->randomName(look.race, look.sex, rng);
            break;
        case 1: {
            const auto *r = creator->race(uint32_t(arg));
            if (!r || r->requiredExpansion > expansion)
                throw std::invalid_argument("Raça indisponível para esta conta");
            look.race = uint8_t(arg);
            if (!creator->raceClassValid(look.race, look.cls)) {
                std::vector<uint32_t> ids;
                for (const auto &c : creator->classesForRace(look.race))
                    if (c.requiredExpansion <= expansion)
                        ids.push_back(c.id);
                if (!ids.empty())
                    look.cls = uint8_t(ids[std::uniform_int_distribution<size_t>(0, ids.size() - 1)(rng)]);
            }
            creator->randomize(look, rng);
            creator->validate(look);
            resetFacing = true;
            break;
        }
        case 2: {
            const auto *c = creator->classInfo(uint32_t(arg));
            if (!c || c->requiredExpansion > expansion || !creator->raceClassValid(look.race, uint32_t(arg)))
                throw std::invalid_argument("Classe indisponível para esta raça");
            look.cls = uint8_t(arg);
            creator->validate(look);
            break;
        }
        case 3:
            if (arg < 0 || arg > 1)
                throw std::invalid_argument("Sexo inválido");
            if (look.sex != arg) {
                look.sex = uint8_t(arg);
                creator->randomize(look, rng);
                creator->validate(look);
            } else
                rebuild = false;
            break;
        case 4:
            if (arg < 0 || arg > 4)
                throw std::invalid_argument("Opção inválida");
            creator->cycle(look, static_cast<Customization>(arg), arg2);
            break;
        case 5:
            creator->randomize(look, rng);
            creator->validate(look);
            break;
        case 6:
            name = creator->randomName(look.race, look.sex, rng);
            rebuild = false;
            break;
        default:
            rebuild = false;
        }
        std::string status;
        if (rebuild)
            status = buildCharacterScene(root, look, resetFacing);
        return utf8(env, creatorState(status, name));
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Creator: %s", error.what());
        if (creator)
            return utf8(env, creatorState(error.what(), ""));
        return utf8(env, std::string("{\"error\":") + quote(error.what()) + "}");
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_rotateGlueScene(JNIEnv *,
                                                                                         jclass,
                                                                                         jfloat dx) {
    if (std::isfinite(dx))
        glueFacing().store(std::remainder(glueFacing().load() + dx * 0.01f, 6.2831853f));
}

extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_clearGlueScene(JNIEnv *,
                                                                                        jclass) {
    auto scene = std::make_unique<GlueSceneData>(); // empty scene clears the active one
    publish(std::move(scene));
}

// Glue music (SI2::StartGlueMusic -> SoundEntries by name): extracts the file to the app
// cache and returns its path for the platform player, or null.
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_extractGlueMusic(
    JNIEnv *env, jclass, jstring dataRoot, jstring cacheDir) {
    try {
        const auto root = javaText(env, dataRoot), cache = javaText(env, cacheDir);
        const auto mount = sharedClientData(root);
        m2::require(mount->report().ready, "Local game data is incomplete");
        const auto name = findGlueMusicName(glueXmlText()).value_or("GS_LichKing");
        const DbcTable sounds(m2::readFile("DBFilesClient\\SoundEntries.dbc", 32 * 1024 * 1024), 30, 120,
                              "SoundEntries.dbc");
        for (uint32_t row = 0; row < sounds.rows(); ++row) {
            std::string entry = sounds.str(row, 2);
            if (entry.size() != name.size() ||
                !std::equal(entry.begin(), entry.end(), name.begin(),
                            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                        std::tolower(static_cast<unsigned char>(b)); }))
                continue;
            std::vector<std::string> files;
            for (uint32_t f = 3; f < 13; ++f)
                if (auto file = sounds.str(row, f); !file.empty())
                    files.push_back(file);
            if (files.empty())
                break;
            const auto file = files[std::uniform_int_distribution<size_t>(0, files.size() - 1)(rng)];
            auto directory = sounds.str(row, 23);
            const auto path = directory.empty() ? file : directory + "\\" + file;
            const auto bytes = m2::readFile(path, 64 * 1024 * 1024);
            const auto dot = file.find_last_of('.');
            const auto out = cache + "/glue-music" + (dot == std::string::npos ? std::string(".mp3") : file.substr(dot));
            FILE *handle = std::fopen(out.c_str(), "wb");
            m2::require(handle != nullptr, "Cannot write music cache");
            const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), handle) == bytes.size();
            std::fclose(handle);
            m2::require(ok, "Cannot write music cache");
            return utf8(env, out);
        }
        return nullptr;
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Glue music: %s", error.what());
        return nullptr;
    }
}

namespace wow::android {
const CharacterCustomization &characterRules(const std::string &root) {
    ensureCreator(root);
    return *creator;
}
std::unique_ptr<M2ModelPreview> loadWorldCharacter(const std::string &root, const CharacterLook &l,
                                                   std::optional<uint32_t> &stand,
                                                   std::optional<uint32_t> &run, std::string &note) {
    ensureCreator(root);
    const auto mount = sharedClientData(root);
    m2::require(mount->report().ready, "Local game data is incomplete");
    const auto path = creator->modelPath(l.race, l.sex);
    const auto bytes = m2::readFile(path);
    stand = m2SequenceIndex(bytes, 0);
    run = m2SequenceIndex(bytes, 5);
    std::vector<uint32_t> slots{stand.value_or(0)};
    if (run && *run != slots[0])
        slots.push_back(*run);
    return loadCharacter(l, note, slots);
}
} // namespace wow::android
