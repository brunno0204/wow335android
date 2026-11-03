#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace wow::android {
struct ClientDataReport {
    bool ready = false;
    bool legacyLayout = false;
    std::string locale;
    std::vector<std::string> availableLocales, mounted, missingRequired, errors;
    std::string summary() const;
};
// Owns the local installation's SFile mounts during original-client startup.
// Full local data only; manifest streaming/trial installations are not emulated.
class LocalClientData;
// Process-lifetime mount shared by every asset load (all loads run on the single asset
// worker). Mounting all MPQs is slow, so it happens once per data root.
std::shared_ptr<LocalClientData> sharedClientData(const std::filesystem::path& dataRoot);
class LocalClientData {
public:
    explicit LocalClientData(const std::filesystem::path& dataRoot, const std::string& locale = "");
    ~LocalClientData();
    LocalClientData(const LocalClientData&) = delete;
    LocalClientData& operator=(const LocalClientData&) = delete;
    const ClientDataReport& report() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
