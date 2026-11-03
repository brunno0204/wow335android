#include <map>
#include <mutex>
#include "TerrainMaterials.h"
#include "M2File.h"
#include "TerrainAlpha.h"
#include <algorithm>
#include <map>
namespace wow::android {
bool terrainNeedsAlphaMode(const TerrainGeometry &geometry) {
    if (geometry.alphaMip)
        return false;
    for (const auto &chunk : geometry.chunks) {
        if (chunk.layers.size() < 2 || chunk.layers.size() > 4)
            continue;
        bool allowed = true;
        for (size_t i = 0; i < chunk.layers.size(); ++i) {
            const auto &l = chunk.layers[i];
            if (l.texture >= geometry.textureFlags.size() || geometry.textureFlags[l.texture] ||
                (i == 0 ? l.flags != 0 : (l.flags != 0x100 && l.flags != 0x300)))
                allowed = false;
        }
        if (allowed)
            return true;
    }
    return false;
}
size_t TerrainPreviewMaterials::texturedChunks() const noexcept {
    return std::count_if(chunks.begin(), chunks.end(),
                         [](const auto &c) { return c.texture >= 0; });
}
TerrainPreviewMaterials prepareTerrainMaterials(const TerrainGeometry &geometry) {
    m2::require(geometry.alphaMode == TerrainAlphaMode::Unknown ||
                    geometry.alphaMode == TerrainAlphaMode::Legacy4 ||
                    geometry.alphaMode == TerrainAlphaMode::Big8,
                "Invalid terrain alpha mode");
    m2::require(geometry.chunks.size() == 256 && geometry.texturePaths.size() <= 256 &&
                    geometry.textureFlags.size() == geometry.texturePaths.size(),
                "Invalid terrain material source");
    TerrainPreviewMaterials result;
    std::map<std::string, int32_t> cache;
    constexpr size_t budget = 128 * 1024 * 1024;
    size_t encoded = 0, decoded = 0;
    auto resolve = [&](uint32_t textureIndex) -> int32_t {
        const auto &path = geometry.texturePaths[textureIndex];
        m2::require(!path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos,
                    "Invalid terrain texture path");
        auto normalized = path;
        for (char &c : normalized) {
            if (c == '/')
                c = '\\';
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        }
        auto found = cache.find(normalized);
        // Streamed neighbour tiles share ground textures: reuse decoded pixels still alive.
        static std::mutex sharedMutex;
        static std::map<std::string, std::weak_ptr<const TexturePixels>> sharedPixels;
        if (found == cache.end()) {
            std::shared_ptr<const TexturePixels> alive;
            {
                std::lock_guard<std::mutex> lock(sharedMutex);
                if (auto it = sharedPixels.find(normalized); it != sharedPixels.end())
                    alive = it->second.lock();
            }
            if (alive) {
                int32_t slot = int32_t(result.textures.size());
                result.textures.push_back(alive);
                result.clamp.push_back(0);
                found = cache.emplace(normalized, slot).first;
                return found->second;
            }
        }
        if (found == cache.end()) {
            auto bytes =
                m2::readFile(path, uint32_t(std::min<size_t>(64 * 1024 * 1024, budget - encoded)));
            encoded += bytes.size();
            m2::require(bytes.size() >= 148, "Truncated terrain BLP2 texture");
            const uint64_t pixels = uint64_t(m2::u32(bytes, 12)) * m2::u32(bytes, 16);
            m2::require(pixels <= (budget - decoded) / 4,
                        "Terrain decoded texture budget exceeded");
            auto image = std::make_shared<TexturePixels>(decodeBlp2(bytes));
            decoded += image->rgba.size();
            auto power = [](uint32_t x) { return x && !(x & (x - 1)); };
            int32_t slot = -1;
            // Detail UVs repeat 8x/chunk; GLES2 NPOT repeat is unsupported.
            if (power(image->width) && power(image->height)) {
                {
                    std::lock_guard<std::mutex> lock(sharedMutex);
                    sharedPixels[normalized] = image;
                }
                slot = int32_t(result.textures.size());
                result.textures.push_back(std::move(image));
                result.clamp.push_back(0);
            }
            found = cache.emplace(std::move(normalized), slot).first;
        }
        return found->second;
    };
    auto mask = [&](const std::vector<uint8_t> &alpha, bool weighted) -> int32_t {
        m2::require(alpha.size() == 4096 && 16384 <= budget - decoded,
                    "Terrain alpha image budget exceeded");
        decoded += 16384;
        auto image = std::make_shared<TexturePixels>();
        image->width = image->height = 64;
        image->rgba.resize(16384);
        for (size_t i = 0; i < 4096; ++i) {
            image->rgba[4 * i] = image->rgba[4 * i + 1] = image->rgba[4 * i + 2] =
                weighted ? alpha[i] : 255;
            image->rgba[4 * i + 3] = alpha[i];
        }
        const auto slot = int32_t(result.textures.size());
        result.textures.push_back(std::move(image));
        result.clamp.push_back(1);
        return slot;
    };
    for (const auto &chunk : geometry.chunks) {
        TerrainPreviewMaterial material;
        if (chunk.layers.empty())
            material.fallback = "no layer";
        else if (chunk.layers.size() > 4)
            material.fallback = "too many layers";
        else {
            const bool multi = chunk.layers.size() > 1;
            for (size_t i = 0; i < chunk.layers.size(); ++i) {
                const auto &layer = chunk.layers[i];
                m2::require(layer.texture < geometry.texturePaths.size(),
                            "Invalid terrain texture index");
                const bool allowed = (!multi || i == 0)
                                         ? layer.flags == 0
                                         : (layer.flags == 0x100 || layer.flags == 0x300);
                if (!allowed)
                    material.fallback = "animated/unsupported layer flags";
                if (geometry.textureFlags[layer.texture])
                    material.fallback = "unsupported terrain texture flags";
            }
            if (multi && geometry.alphaMip)
                material.fallback = "reduced alpha maps unsupported";
            if (multi && geometry.alphaMode == TerrainAlphaMode::Unknown)
                material.fallback = "map alpha mode unknown";
            if (multi && geometry.alphaMode == TerrainAlphaMode::Legacy4)
                for (const auto &layer : chunk.layers)
                    if (layer.flags & 0x200)
                        material.fallback = "compressed legacy alpha unsupported";
            if (material.fallback.empty()) {
                std::vector<std::vector<uint8_t>> alpha(chunk.layers.size());
                const bool weighted = multi && geometry.alphaMode == TerrainAlphaMode::Big8;
                if (multi) {
                    for (size_t i = 1; i < chunk.layers.size(); ++i) {
                        const auto &layer = chunk.layers[i];
                        size_t end = chunk.alpha.size();
                        for (size_t j = 1; j < chunk.layers.size(); ++j)
                            if (chunk.layers[j].alphaOffset > layer.alphaOffset)
                                end = std::min<size_t>(end, chunk.layers[j].alphaOffset);
                        alpha[i] = decodeTerrainAlpha(
                            chunk.alpha, layer.alphaOffset, end, geometry.alphaMode,
                            (layer.flags & 0x200) != 0, (chunk.flags & 0x8000) != 0);
                    }
                    if (weighted) {
                        alpha[0].resize(4096);
                        for (size_t p = 0; p < 4096; ++p) {
                            unsigned sum = 0;
                            for (size_t i = 1; i < alpha.size(); ++i)
                                sum += alpha[i][p];
                            m2::require(sum <= 255, "Terrain alpha weights exceed one");
                            alpha[0][p] = uint8_t(255 - sum);
                        }
                    }
                }
                std::vector<int32_t> images;
                for (const auto &layer : chunk.layers)
                    images.push_back(resolve(layer.texture));
                if (std::find(images.begin(), images.end(), -1) != images.end())
                    material.fallback = "NPOT repeat unsupported on GLES2";
                else {
                    material.texture = images[0];
                    material.additive = weighted;
                    for (size_t i = 0; i < images.size(); ++i)
                        material.passes.push_back(
                            {images[i], multi && (i || weighted) ? mask(alpha[i], weighted) : -1});
                }
            }
        }
        result.chunks.push_back(std::move(material));
    }
    return result;
}
} // namespace wow::android
