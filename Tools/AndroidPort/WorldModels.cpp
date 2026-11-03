// Asset worker: dressed characters (CCharacterComponent item textures, geosets and
// attached item models), creature displays and game object displays for the world scene.
// Rules follow WoW/Common/ComponentCore/CharacterComponent.cpp (GeosRenderPrep,
// UpdateItem*, s_itemPriority, LoadItemComponentTexture, helm/shoulder/hand items).
#include "CharacterCustomization.h"
#include "ClientDb.h"
#include "GlueBridgeInternal.h"
#include "GlueScene.h"
#include "M2File.h"
#include "TextureDecoder.h"
#include "WorldAssets.h"
#include "WorldScene.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>
namespace wow::android {
namespace {
struct IndexedTable {
    DbcTable table;
    std::map<uint32_t, uint32_t> rows;
    void load(const char *path, uint32_t fields, uint32_t size, const char *name) {
        try {
            table = DbcTable(m2::readFile(path, 64 * 1024 * 1024), fields, size, name);
            for (uint32_t r = 0; r < table.rows(); ++r)
                rows[table.u32(r, 0)] = r;
        } catch (const std::exception &) {
            table = DbcTable();
            rows.clear();
        }
    }
    std::optional<uint32_t> row(uint32_t id) const {
        const auto found = rows.find(id);
        if (found == rows.end())
            return std::nullopt;
        return found->second;
    }
};
struct WorldDbc {
    std::string root;
    IndexedTable items, displays, models, extras, objects, helmets;
};
WorldDbc &dbc(const std::string &root) {
    static WorldDbc db;
    if (db.root != root) {
        db = WorldDbc{};
        db.root = root;
        db.items.load("DBFilesClient\\ItemDisplayInfo.dbc", 25, 100, "ItemDisplayInfo.dbc");
        db.displays.load("DBFilesClient\\CreatureDisplayInfo.dbc", 16, 64, "CreatureDisplayInfo.dbc");
        db.models.load("DBFilesClient\\CreatureModelData.dbc", 28, 112, "CreatureModelData.dbc");
        db.extras.load("DBFilesClient\\CreatureDisplayInfoExtra.dbc", 21, 84,
                       "CreatureDisplayInfoExtra.dbc");
        db.objects.load("DBFilesClient\\GameObjectDisplayInfo.dbc", 19, 76, "GameObjectDisplayInfo.dbc");
        db.helmets.load("DBFilesClient\\HelmetGeosetVisData.dbc", 8, 32, "HelmetGeosetVisData.dbc");
    }
    return db;
}
struct ItemDisplay {
    uint32_t id = 0;
    std::array<std::string, 2> model, modelTexture;
    std::array<uint32_t, 3> geoset{};
    std::array<uint32_t, 2> helmetVis{};
    std::array<std::string, 8> texture;
};
std::optional<ItemDisplay> itemDisplay(const WorldDbc &db, uint32_t id) {
    if (!id)
        return std::nullopt;
    const auto row = db.items.row(id);
    if (!row)
        return std::nullopt;
    const auto &t = db.items.table;
    ItemDisplay d;
    d.id = id;
    for (int i = 0; i < 2; ++i) {
        d.model[i] = t.str(*row, 1 + i);
        d.modelTexture[i] = t.str(*row, 3 + i);
        d.helmetVis[i] = t.u32(*row, 13 + i);
    }
    for (int i = 0; i < 3; ++i)
        d.geoset[i] = t.u32(*row, 7 + i);
    for (int i = 0; i < 8; ++i)
        d.texture[i] = t.str(*row, 15 + i);
    return d;
}
std::string asM2(std::string path) {
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos)
        path = path.substr(0, dot);
    return path + ".m2";
}
std::string skinOf(const std::string &m2) { return m2.substr(0, m2.size() - 3) + "00.skin"; }
bool exists(const std::string &path) {
    try {
        m2::readFile(path, 1);
        return true;
    } catch (const std::invalid_argument &) {
        return true; // present but larger than the probe limit
    } catch (const std::exception &) {
        return false;
    }
}
std::string directoryOf(const std::string &path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}
// Animation ID -> first preloaded slot; slot list for M2AnimatedModel::load.
std::vector<uint32_t> animationSlots(const std::string &path, const std::vector<uint16_t> &ids,
                                     std::map<uint16_t, uint32_t> &out) {
    const auto bytes = m2::readFile(path);
    std::vector<uint32_t> slots;
    for (uint16_t id : ids)
        if (const auto slot = m2SequenceIndex(bytes, id)) {
            out[id] = *slot;
            if (std::find(slots.begin(), slots.end(), *slot) == slots.end())
                slots.push_back(*slot);
        }
    if (slots.empty()) {
        slots.push_back(0);
        out[0] = 0;
    }
    return slots;
}
std::unique_ptr<M2ModelPreview> loadItemModel(const std::string &model, const std::string &texture) {
    M2TextureReplacements bindings;
    if (!texture.empty())
        bindings[2] = texture;
    return std::make_unique<M2ModelPreview>(M2AnimatedModel::load(model, skinOf(model), {0}, true, bindings));
}
// Equipment slot (inventory) -> component item slot (CCharacterComponent ITEM_SLOT).
constexpr int componentSlot[19] = {0, -1, 1, 2, 3, 4, 5, 6, 7, 8, -1, -1, -1, -1, 10, -1, -1, -1, 9};
// s_itemPriority[ITEM_SLOT][COMPONENT_SECTIONS] (first 8 sections).
constexpr int itemPriority[11][8] = {
    {-1, -1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1},
    {0, 0, -1, 0, 0, -1, -1, -1},     {1, 1, -1, 1, 1, 1, 1, -1},
    {-1, -1, -1, -1, 5, 2, -1, -1},   {-1, -1, -1, -1, -1, 0, 0, -1},
    {-1, -1, -1, -1, -1, -1, 2, 0},   {-1, 2, -1, -1, -1, -1, -1, -1},
    {-1, 3, 0, -1, -1, -1, -1, -1},   {-1, -1, -1, 4, 4, -1, -1, -1},
    {0, 0, 0, 0, 0, 0, 0, 0}};
constexpr const char *componentSections[8] = {"ArmUpperTexture", "ArmLowerTexture", "HandTexture",
                                              "TorsoUpperTexture", "TorsoLowerTexture", "LegUpperTexture",
                                              "LegLowerTexture", "FootTexture"};
class GeosetSet {
  public:
    explicit GeosetSet(std::vector<uint32_t> base) : ids_(base.begin(), base.end()) {}
    void hide(uint32_t low, uint32_t high) {
        for (auto it = ids_.lower_bound(low); it != ids_.end() && *it <= high;)
            it = ids_.erase(it);
    }
    void show(uint32_t id) { ids_.insert(id); }
    void replace(uint32_t group, uint32_t id) {
        hide(group ? group * 100 : 1, group * 100 + 99); // group 0 keeps the body (geoset 0)
        show(id);
    }
    std::vector<uint32_t> ids() const { return {ids_.begin(), ids_.end()}; }

  private:
    std::set<uint32_t> ids_;
};
} // namespace
void setPortraitCamera(DressedModel &dressed, const std::string &model) {
    try {
        if (const auto camera = decodeM2PortraitCamera(m2::readFile(model))) {
            dressed.hasPortrait = true;
            dressed.portraitEye = {float(camera->eye[0]), float(camera->eye[1]), float(camera->eye[2])};
            dressed.portraitTarget = {float(camera->target[0]), float(camera->target[1]), float(camera->target[2])};
            dressed.portraitFov = camera->fov;
        }
    } catch (const std::exception &) {
    }
}
std::unique_ptr<DressedModel> loadDressedCharacter(const std::string &root, const CharacterAppearance &a,
                                                   const std::vector<uint16_t> &animations,
                                                   std::string &note) {
    return loadDressedCharacterWith(root, a, animations, note, std::string());
}
std::unique_ptr<DressedModel> loadDressedCharacterWith(const std::string &root, const CharacterAppearance &a,
                                                       const std::vector<uint16_t> &animations,
                                                       std::string &note, const std::string &baked) {
    const auto &rules = characterRules(root);
    const auto &db = dbc(root);
    CharacterLook look{a.race, a.cls ? a.cls : uint8_t(1), a.sex, a.skin, a.face, a.hairStyle, a.hairColor,
                       a.facialHair};
    const auto modelPath = rules.modelPath(look.race, look.sex);
    const auto *race = rules.race(look.race);
    std::array<std::optional<ItemDisplay>, 19> items;
    for (size_t slot = 0; slot < 19; ++slot)
        items[slot] = itemDisplay(db, a.displays[slot]);
    auto item = [&](int slot) -> const ItemDisplay * { return items[slot] ? &*items[slot] : nullptr; };
    // Geosets: ComponentData defaults + hair/facial, helm visibility, then GeosRenderPrep.
    auto baseIds = rules.geosets(look);
    GeosetSet geosets(baseIds);
    if (const auto *helm = item(0); helm && race) {
        if (const auto vis = db.helmets.row(helm->helmetVis[std::min<uint8_t>(look.sex, 1)])) {
            const uint32_t mask = 1u << look.race;
            const auto &t = db.helmets.table;
            const uint32_t hidden[7][2] = {{0, 1}, {1, 101}, {2, 201}, {3, 301}, {7, 701}, {16, 1601}, {17, 1701}};
            for (int k = 0; k < 7; ++k)
                if (t.u32(*vis, 1 + k) & mask)
                    geosets.replace(hidden[k][0], hidden[k][1]);
        }
    }
    // Section textures by layer (s_itemPriority with the UpdateItem overrides).
    struct Layer {
        int layer;
        CharacterSection section;
        std::string path;
    };
    std::vector<Layer> layers;
    bool armLowerCovered = false;
    for (size_t slot = 0; slot < 19; ++slot) {
        const int component = componentSlot[slot];
        const auto *d = item(int(slot));
        if (component < 0 || !d)
            continue;
        for (int section = 0; section < 8; ++section) {
            if (d->texture[section].empty())
                continue;
            int layer = itemPriority[component][section];
            if (section == 1 && d->geoset[0]) {
                if (component == 3)
                    layer = 5;
                else if (component == 8)
                    layer = 6;
            } else if (section == 6) {
                if (component == 3 && d->geoset[2])
                    layer = 4;
                else if (component == 5 && d->geoset[2])
                    layer = item(4) && item(4)->geoset[2] ? 3 : 4;
                else if (component == 6 && d->geoset[0])
                    layer = 3;
            }
            if (layer < 0)
                continue;
            if (section == 1 && layer >= 1)
                armLowerCovered = true;
            const std::string base = std::string("Item\\TextureComponents\\") + componentSections[section] +
                                     "\\" + d->texture[section];
            std::string path = base + "_U.blp";
            if (!exists(path))
                path = base + (look.sex ? "_F.blp" : "_M.blp");
            layers.push_back({layer, CharacterSection(section), path});
        }
    }
    std::stable_sort(layers.begin(), layers.end(), [](const Layer &x, const Layer &y) { return x.layer < y.layer; });
    // GeosRenderPrep item rules.
    const auto *gloves = item(9), *chest = item(4), *shirt = item(3), *pants = item(6), *boots = item(7),
               *tabardItem = item(18), *cape = item(14), *belt = item(5);
    if (gloves && gloves->geoset[0])
        geosets.replace(4, gloves->geoset[0] + 401);
    else if (chest && chest->geoset[0])
        geosets.show(chest->geoset[0] + 801);
    if (!armLowerCovered && shirt && shirt->geoset[0])
        geosets.show(shirt->geoset[0] + 801);
    bool robe = false, dress = false, tabard = false;
    if (chest && chest->geoset[2]) {
        geosets.hide(501, 599);
        geosets.hide(902, 999);
        geosets.hide(1100, 1199);
        geosets.hide(1300, 1399);
        geosets.show(chest->geoset[2] + 1301);
        robe = true;
    } else if (pants && pants->geoset[2]) {
        geosets.hide(501, 599);
        geosets.hide(902, 999);
        geosets.hide(1100, 1199);
        geosets.hide(1300, 1399);
        geosets.show(pants->geoset[2] + 1301);
        dress = true;
    } else {
        if (boots && boots->geoset[0]) {
            geosets.hide(501, 599);
            geosets.show(901);
            geosets.show(boots->geoset[0] + 501);
        } else if (pants && pants->geoset[1])
            geosets.show(pants->geoset[1] + 901);
        else
            geosets.show(901);
        if (tabardItem && tabardItem->geoset[0]) {
            geosets.show(tabardItem->geoset[0] + 1201);
            tabard = true;
        }
    }
    (void)dress;
    if (!robe) {
        if (!tabard && shirt && shirt->geoset[1])
            geosets.show(shirt->geoset[1] + 1001);
        if (pants && pants->geoset[0]) {
            if (pants->geoset[0] > 2) {
                geosets.hide(1300, 1399);
                geosets.show(pants->geoset[0] + 1101);
            } else if (!tabard)
                geosets.show(pants->geoset[0] + 1101);
        }
    }
    if (cape && cape->geoset[0]) {
        geosets.hide(1500, 1599);
        geosets.show(cape->geoset[0] + 1501);
    }
    if (belt && belt->geoset[0]) {
        geosets.hide(1800, 1899);
        geosets.show(belt->geoset[0] + 1801);
    }
    // Body model with skin/hair/cape textures.
    const auto recipe = rules.textures(look);
    M2TextureReplacements bindings;
    const std::string skin = !baked.empty() ? baked : recipe.baseSkin;
    if (!skin.empty()) {
        bindings[1] = skin;
        bindings[8] = recipe.extraSkin.empty() ? skin : recipe.extraSkin;
    }
    if (!recipe.hair.empty())
        bindings[6] = recipe.hair;
    if (cape && !cape->modelTexture[0].empty())
        bindings[2] = "Item\\ObjectComponents\\Cape\\" + cape->modelTexture[0] + ".blp";
    auto dressed = std::make_unique<DressedModel>();
    const auto slots = animationSlots(modelPath, animations, dressed->animations);
    dressed->body = std::make_unique<M2ModelPreview>(
        M2AnimatedModel::load(modelPath, skinOf(modelPath), slots, true, bindings));
    setPortraitCamera(*dressed, modelPath);
    auto available = dressed->body->geosetCatalog()->sections;
    std::sort(available.begin(), available.end());
    std::vector<uint32_t> ids;
    for (uint32_t id : geosets.ids())
        if (std::binary_search(available.begin(), available.end(), id))
            ids.push_back(id);
    dressed->body->installGeosets(prepareM2Geosets(dressed->body->geosetCatalog(), ids));
    // Composed base skin: CharSections layers, then item components by priority.
    if (baked.empty() && !recipe.baseSkin.empty()) {
        try {
            const auto base = decodeBlp2(m2::readFile(recipe.baseSkin, 16 * 1024 * 1024));
            std::vector<std::pair<CharacterSection, TexturePixels>> pixels;
            auto add = [&](CharacterSection section, const std::string &path) {
                try {
                    pixels.push_back({section, decodeBlp2(m2::readFile(path, 16 * 1024 * 1024))});
                } catch (const std::exception &) {
                }
            };
            for (const auto &layer : recipe.layers)
                add(layer.section, layer.path);
            for (const auto &layer : layers)
                add(layer.section, layer.path);
            auto composed = std::make_shared<const TexturePixels>(composeCharacterTexture(base, pixels));
            auto materials = std::make_shared<M2PreviewMaterials>(*dressed->body->model().previewMaterials());
            for (auto &texture : materials->textures)
                if (texture.type == 1)
                    texture.pixels = composed;
            dressed->body->installAppearance(std::move(materials));
        } catch (const std::exception &error) {
            note += std::string(" Textura do corpo simples: ") + error.what() + ".";
        }
    }
    // Attached item models: helm, shoulders, weapons.
    auto attach = [&](const std::string &model, const std::string &texture, uint32_t point) {
        if (!dressed->body->model().hasAttachment(point))
            return;
        try {
            dressed->items.push_back({loadItemModel(model, texture), point});
        } catch (const std::exception &error) {
            note += " Item " + model + ": " + error.what() + ".";
        }
    };
    if (const auto *helm = item(0); helm && !helm->model[0].empty() && race) {
        auto base = helm->model[0];
        const auto dot = base.find_last_of('.');
        if (dot != std::string::npos)
            base = base.substr(0, dot);
        const std::string path = "Item\\ObjectComponents\\Head\\" + base + "_" + race->clientPrefix +
                                 (look.sex ? "F" : "M") + ".m2";
        attach(path,
               helm->modelTexture[0].empty() ? "" : "Item\\ObjectComponents\\Head\\" + helm->modelTexture[0] + ".blp",
               11);
    }
    if (const auto *shoulder = item(2)) {
        for (int side = 0; side < 2; ++side)
            if (!shoulder->model[side].empty())
                attach("Item\\ObjectComponents\\Shoulder\\" + asM2(shoulder->model[side]),
                       shoulder->modelTexture[side].empty()
                           ? ""
                           : "Item\\ObjectComponents\\Shoulder\\" + shoulder->modelTexture[side] + ".blp",
                       side == 0 ? 6 : 5);
    }
    for (int slot : {15, 16}) {
        const auto *weapon = item(slot);
        if (!weapon || weapon->model[0].empty())
            continue;
        const uint8_t type = a.inventoryTypes[size_t(slot)];
        const bool shield = type == 14;
        const std::string folder = shield ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";
        uint32_t point;
        if (!a.sheathed)
            point = shield ? 0 : slot == 15 ? 1 : 2;
        else if (shield)
            point = 28;
        else if (type == 17)
            point = slot == 15 ? 26 : 27; // two-handed: across the back
        else
            point = slot == 15 ? 32 : 33; // one-handed: hips
        if (slot == 15)
            dressed->mainHandType = type;
        const size_t before = dressed->items.size();
        attach(folder + asM2(weapon->model[0]),
               weapon->modelTexture[0].empty() ? "" : folder + weapon->modelTexture[0] + ".blp", point);
        // In hand when drawn (CGUnit_C sheath state): right hand 1, left hand 2, shield 0.
        const uint32_t hand = shield ? 0 : slot == 15 ? 1 : 2;
        if (dressed->items.size() > before && dressed->body->model().hasAttachment(hand))
            dressed->items.back().drawnAttachment = int32_t(hand);
    }
    return dressed;
}
std::unique_ptr<DressedModel> loadUnitModel(const std::string &root, const std::string &key,
                                            std::string &note) {
    std::vector<std::string> parts;
    {
        std::stringstream in(key);
        std::string part;
        while (std::getline(in, part, ':'))
            parts.push_back(part);
    }
    m2::require(!parts.empty(), "Empty unit model key");
    auto number = [&](size_t i) -> uint32_t {
        return i < parts.size() ? uint32_t(std::strtoul(parts[i].c_str(), nullptr, 10)) : 0;
    };
    const auto &db = dbc(root);
    // Base states plus the one-shots the server drives (WorldScene unit actions): walk back,
// melee, wound/dodge/parry, spell ready/cast/channel, swim and the common emotes.
    const std::vector<uint16_t> unitAnimations{0, 4, 5, 1, 6, 13, 16, 17, 18, 27, 9, 30, 21, 52, 54, 32, 125, 26,
                                               60, 66, 67, 68, 69, 70, 71, 72, 74, 77, 80, 41, 42, 91};
    if (parts[0] == "p") { // other players
        CharacterAppearance a;
        a.race = uint8_t(number(1));
        a.sex = uint8_t(number(2));
        a.skin = uint8_t(number(3));
        a.face = uint8_t(number(4));
        a.hairStyle = uint8_t(number(5));
        a.hairColor = uint8_t(number(6));
        a.facialHair = uint8_t(number(7));
        a.cls = uint8_t(number(8));
        for (size_t i = 0; i < 19; ++i)
            a.displays[i] = number(9 + i);
        a.inventoryTypes[15] = uint8_t(number(28));
        a.inventoryTypes[16] = uint8_t(number(29));
        a.inventoryTypes[17] = uint8_t(number(30));
        return loadDressedCharacter(root, a, unitAnimations, note);
    }
    if (parts[0] == "i") { // quest giver / interact icon (CGObject_C::s_interactIcons)
        static const char *const icons[12]{nullptr,
                                           "Interface\\Buttons\\TalkToMe.mdx",
                                           "Interface\\Buttons\\TalkToMeQuestion_LTBlue.mdx",
                                           "Interface\\Buttons\\TalkToMeBlue.mdx",
                                           "Interface\\Buttons\\TalkToMe.mdx",
                                           "Interface\\Buttons\\TalkToMeQuestion_Grey.mdx",
                                           "Interface\\Buttons\\TalkToMeGrey.mdx",
                                           "Interface\\Buttons\\TalkToMeGreen.mdx",
                                           nullptr,
                                           "Interface\\Buttons\\TalkToMeQuestionMark.mdx",
                                           "Interface\\Buttons\\TalkToMeQuestion_LTBlue.mdx",
                                           "Interface\\Buttons\\TalkToMeBlue.mdx"};
        const uint32_t index = number(1);
        m2::require(index < 12 && icons[index], "Unknown interact icon");
        auto dressed = std::make_unique<DressedModel>();
        const auto model = asM2(icons[index]);
        const auto slots = animationSlots(model, {0}, dressed->animations);
        dressed->body = std::make_unique<M2ModelPreview>(M2AnimatedModel::load(model, skinOf(model), slots, true));
        return dressed;
    }
    if (parts[0] == "g") { // game object
        const auto row = db.objects.row(number(1));
        m2::require(bool(row), "Unknown GameObjectDisplayInfo");
        const auto path = db.objects.table.str(*row, 1);
        auto lower = path;
        for (char &c : lower)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        m2::require(!path.empty() && lower.find(".wmo") == std::string::npos, "Unsupported game object model");
        auto dressed = std::make_unique<DressedModel>();
        const auto model = asM2(path);
        const auto slots = animationSlots(model, {0}, dressed->animations);
        dressed->body = std::make_unique<M2ModelPreview>(M2AnimatedModel::load(model, skinOf(model), slots, true));
        return dressed;
    }
    m2::require(parts[0] == "d", "Unknown unit model key");
    const auto row = db.displays.row(number(1));
    m2::require(bool(row), "Unknown CreatureDisplayInfo");
    const auto &displays = db.displays.table;
    const uint32_t modelId = displays.u32(*row, 1), extraId = displays.u32(*row, 3);
    float scale = displays.f32(*row, 4);
    if (!(scale > 0.01f && scale < 100))
        scale = 1;
    const auto modelRow = db.models.row(modelId);
    m2::require(bool(modelRow), "Unknown CreatureModelData");
    const float modelScale = db.models.table.f32(*modelRow, 4);
    if (modelScale > 0.01f && modelScale < 100)
        scale *= modelScale;
    // Weapons (virtual items): "display,inventoryType" pairs.
    std::array<std::pair<uint32_t, uint8_t>, 3> weapons{};
    for (size_t i = 0; i < 3; ++i) {
        const auto &text = 2 + i < parts.size() ? parts[2 + i] : std::string();
        const auto comma = text.find(',');
        weapons[i] = {uint32_t(std::strtoul(text.c_str(), nullptr, 10)),
                      uint8_t(comma == std::string::npos ? 0 : std::strtoul(text.c_str() + comma + 1, nullptr, 10))};
    }
    if (const auto extra = db.extras.row(extraId); extraId && extra) {
        const auto &t = db.extras.table;
        CharacterAppearance a;
        a.race = uint8_t(t.u32(*extra, 1));
        a.sex = uint8_t(t.u32(*extra, 2));
        a.skin = uint8_t(t.u32(*extra, 3));
        a.face = uint8_t(t.u32(*extra, 4));
        a.hairStyle = uint8_t(t.u32(*extra, 5));
        a.hairColor = uint8_t(t.u32(*extra, 6));
        a.facialHair = uint8_t(t.u32(*extra, 7));
        // NPCItemDisplay: helm, shoulder, shirt, chest, belt, legs, boots, wrist, gloves, tabard, cape.
        constexpr int slots[11] = {0, 2, 3, 4, 5, 6, 7, 8, 9, 18, 14};
        for (int i = 0; i < 11; ++i)
            a.displays[size_t(slots[i])] = t.u32(*extra, 8 + i);
        a.displays[15] = weapons[0].first;
        a.inventoryTypes[15] = weapons[0].second;
        a.displays[16] = weapons[1].first;
        a.inventoryTypes[16] = weapons[1].second;
        a.sheathed = false;
        const auto bake = t.str(*extra, 20);
        std::string baked;
        if (!bake.empty())
            baked = "Textures\\BakedNpcTextures\\" + bake;
        auto dressed = loadDressedCharacterWith(root, a, unitAnimations, note, baked);
        dressed->scale = scale;
        return dressed;
    }
    const auto model = asM2(db.models.table.str(*modelRow, 2));
    M2TextureReplacements bindings;
    const auto folder = directoryOf(model);
    for (int i = 0; i < 3; ++i)
        if (const auto variation = displays.str(*row, 6 + i); !variation.empty())
            bindings[11 + i] = folder + variation + ".blp";
    auto dressed = std::make_unique<DressedModel>();
    const auto slots = animationSlots(model, unitAnimations, dressed->animations);
    dressed->body =
        std::make_unique<M2ModelPreview>(M2AnimatedModel::load(model, skinOf(model), slots, true, bindings));
    setPortraitCamera(*dressed, model);
    // ApplyMonsterGeosets: nibbles of m_creatureGeosetData select groups 100..800.
    if (const uint32_t data = displays.u32(*row, 14)) {
        auto ids = dressed->body->geosetCatalog()->sections;
        GeosetSet set(ids);
        for (uint32_t group = 100, shift = 0; group < 900; group += 100, shift += 4)
            if (const uint32_t g = (data >> shift) & 0xf) {
                set.hide(group, group + 99);
                set.show(group + g);
            }
        auto available = ids;
        std::sort(available.begin(), available.end());
        std::vector<uint32_t> visible;
        for (uint32_t id : set.ids())
            if (std::binary_search(available.begin(), available.end(), id))
                visible.push_back(id);
        dressed->body->installGeosets(prepareM2Geosets(dressed->body->geosetCatalog(), visible));
    }
    dressed->scale = scale;
    return dressed;
}
} // namespace wow::android
