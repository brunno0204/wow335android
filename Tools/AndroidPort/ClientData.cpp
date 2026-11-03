#include <mutex>
#include "ClientData.h"
#include "NativeAssetPaths.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <sstream>

namespace wow::android {
namespace fs = std::filesystem;
namespace {
struct ArchiveSpec { const char* name; int kind, layout; bool opened; };
constexpr ArchiveSpec archiveSpecs[] = {
#include "WoW/Source/ClientArchives.inc"
};
constexpr const char* localeNames[] = {
#include "WoW/Source/DB/DBClient/LocaleNames.inc"
};
static_assert(sizeof(archiveSpecs) / sizeof(*archiveSpecs) == 28);
std::atomic<bool> activeSession{false};
struct ExclusiveSession {
    ExclusiveSession() {
        bool expected = false;
        if (!activeSession.compare_exchange_strong(expected, true))
            throw std::logic_error("A client data session is already mounted");
    }
    ~ExclusiveSession() { activeSession.store(false); }
};
std::string fold(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string localized(std::string name, const std::string& locale) {
    size_t at;
    while ((at = name.find("****")) != std::string::npos) name.replace(at, 4, locale);
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}
bool patchMatch(const std::string& filename, const std::string& locale) {
    const auto name = fold(filename);
    const auto prefix = locale.empty() ? std::string("patch-") : "patch-" + fold(locale) + '-';
    return name.size() == prefix.size() + 5 && name.compare(0, prefix.size(), prefix) == 0 &&
        name.compare(name.size() - 4, 4, ".mpq") == 0;
}
}
struct LocalClientData::Impl {
    ExclusiveSession exclusive;
    ClientDataReport result;
    std::vector<SArchive*> handles;
    std::string oldBase = SFile::m_basePath, oldData = SFile::GetDataPath();
    uint32_t oldLocale = SFile::s_locale;
    bool pathsChanged = false;
    ~Impl() {
        clear();
        if (pathsChanged) { SFile::SetBasePath(oldBase.c_str()); SFile::SetDataPath(oldData.c_str()); }
        SFile::s_locale = oldLocale;
    }
    void clear() {
        for (auto it = handles.rbegin(); it != handles.rend(); ++it) SFile::CloseArchive(*it);
        handles.clear();
    }
    SArchive* mount(const fs::path& path, int priority, bool required) {
        SArchive* handle = nullptr;
        if (SFile::OpenArchive(path.c_str(), priority, 0xc00, &handle)) {
            handles.push_back(handle); result.mounted.push_back(path.filename().string()); return handle;
        }
        if (SErrGetLastError() != 2) result.errors.push_back(path.filename().string() + ": " + SFile::GetErrorDetails());
        else if (required) result.missingRequired.push_back(path.string());
        return nullptr;
    }
    void build(const fs::path& root, const std::string& requested) {
        auto data = resolvePhysicalPath(root);
        if (data.empty() || !fs::is_directory(data)) {
            result.errors.push_back("Client Data directory is missing"); return;
        }
        if (!requested.empty() && std::none_of(std::begin(localeNames), std::end(localeNames),
            [&](const char* name) { return requested == name; })) {
            result.errors.push_back("Unsupported client locale: " + requested); return;
        }
        pathsChanged = true;
        if (!SFile::SetBasePath(data.parent_path().c_str()) || !SFile::SetDataPath(data.c_str()) || !SFile::EnableDirectAccess(0)) {
            result.errors.push_back(SFile::GetErrorDetails()); return;
        }
        SArchive* common = mount(data / "common.MPQ", 47, false);
        result.legacyLayout = common != nullptr;
        if (!result.errors.empty()) return; // Corruption is not a split-pack installation.
        for (const char* name : localeNames) {
            if (!resolvePhysicalPath(data / name / (std::string("locale-") + name + ".MPQ")).empty())
                result.availableLocales.emplace_back(name);
        }
        if (!requested.empty()) result.locale = requested;
        else if (std::find(result.availableLocales.begin(), result.availableLocales.end(), "enUS") != result.availableLocales.end()) result.locale = "enUS";
        else if (!result.availableLocales.empty()) result.locale = result.availableLocales.front();
        else result.locale = "enUS";
        uint32_t code = 0;
        for (unsigned char c : result.locale) code = (code << 8) | c;
        SFile::s_locale = code;

        // Same two passes as PatchFiles(locale, false): numbered packs sorted
        // descending, followed by the root and locale unnumbered patch packs.
        std::vector<fs::path> patches;
        auto enumerate = [&](const fs::path& directory, const std::string& locale) {
            const auto resolved = resolvePhysicalPath(directory);
            if (resolved.empty()) return;
            for (const auto& entry : fs::directory_iterator(resolved))
                if (entry.is_regular_file() && patchMatch(entry.path().filename().string(), locale)) patches.push_back(entry.path());
        };
        enumerate(data, ""); enumerate(data / result.locale, result.locale);
        std::sort(patches.begin(), patches.end(), [](const auto& a, const auto& b) { return fold(a.string()) > fold(b.string()); });
        for (const auto& path : {data / "patch.MPQ", data / result.locale / ("patch-" + result.locale + ".MPQ")}) {
            const auto resolved = resolvePhysicalPath(path);
            if (!resolved.empty()) patches.push_back(resolved);
        }
        int priority = 64;
        std::vector<SArchive*> patchHandles;
        for (auto it = patches.rbegin(); it != patches.rend(); ++it)
            if (auto* handle = mount(*it, priority, false)) { patchHandles.push_back(handle); ++priority; }
        std::vector<bool> opened(std::size(archiveSpecs), false);
        for (size_t i = 0; i < std::size(archiveSpecs); ++i) {
            const auto& spec = archiveSpecs[i];
            if (spec.kind == 3) {
                opened[i] = mount(data / spec.name, priority, false) != nullptr;
                if (opened[i]) ++priority;
            }
        }
        const bool regional = result.locale == "zhCN" || result.locale == "enCN" || result.locale == "zhTW" || result.locale == "enTW";
        for (auto* patch : patchHandles) for (size_t i = 0; i < std::size(archiveSpecs); ++i) {
            const auto& spec = archiveSpecs[i];
            if (spec.kind != 3 || (!opened[i] && !regional)) continue;
            auto prefix = fs::path(spec.name).stem().string(); SArchive* view = nullptr;
            if (SFile::OpenPathAsArchive(patch, prefix.c_str(), priority, 0, &view)) { handles.push_back(view); ++priority; }
            else result.errors.push_back("Cannot create archive view: " + prefix);
        }
        int basePriority = 63;
        const int layout = result.legacyLayout ? 2 : 1;
        for (const auto& spec : archiveSpecs) {
            if (spec.kind == 3 || spec.kind == 4) continue;
            const int rank = basePriority--;
            if (!(spec.layout & layout) || (common && std::string(spec.name) == "common.MPQ")) continue;
            auto filename = localized(spec.name, result.locale);
            mount(data / filename, rank, spec.kind == 1);
        }
        result.ready = result.errors.empty() && result.missingRequired.empty();
        if (!result.ready) clear(); // Do not expose a partially mounted install.
    }
};
LocalClientData::LocalClientData(const fs::path& root, const std::string& locale) : impl_(std::make_unique<Impl>()) {
    try { impl_->build(root, locale); }
    catch (const std::exception& error) { impl_->result.errors.push_back(error.what()); }
    if (!impl_->result.ready) impl_->clear();
}
LocalClientData::~LocalClientData() = default;
const ClientDataReport& LocalClientData::report() const { return impl_->result; }
std::string ClientDataReport::summary() const {
    std::ostringstream out;
    out << (ready ? "Required local archives mounted" : "Client data is incomplete")
        << "; locale " << (locale.empty() ? "unknown" : locale) << "; " << mounted.size() << " archives opened.";
    for (const auto& item : missingRequired) out << "\nMissing: " << fs::path(item).filename().string();
    for (const auto& error : errors) out << "\n" << error;
    return out.str();
}
std::shared_ptr<LocalClientData> sharedClientData(const std::filesystem::path& dataRoot) {
    static std::mutex mutex;
    static std::shared_ptr<LocalClientData> shared;
    static std::filesystem::path root;
    std::lock_guard<std::mutex> lock(mutex);
    if (shared && root == dataRoot && shared->report().ready)
        return shared;
    shared.reset(); // unmount before mounting another root
    auto mounted = std::make_shared<LocalClientData>(dataRoot);
    if (mounted->report().ready) {
        shared = mounted;
        root = dataRoot;
    }
    return mounted;
}
}
