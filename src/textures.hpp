#pragma once
// Surface textures (view only; Surface layer in RULES.md "Render layers").
//
// build/textures.bin is written by scripts/build_textures.py, which refuses any
// texture without a complete CC0, photographic manifest entry
// (data/textures/manifest.json). This header reads that file and says which
// texture each block uses and how it blends with the cell's paint.
//
// Blending (shaders/voxel.frag, texturedBase):
//   unpainted cell -> the texture is the surface colour
//   painted cell   -> filter: the texture multiplied toward the paint (tint)
//                     base:   the paint showing through where the texture's
//                             coverage mask is low (coverage, maskFromLuma)
//   Both apply together, so a material can be rust tinted by its paint and
//   still let the paint show through its pale patches.
// Textures map triplanar in world space, so one tile spans many cells instead
// of repeating per cube and exposing the grid.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "voxel_wire.hpp"

namespace tex {

static constexpr int kMaxLayers = 16;

struct TextureSet {
    int size = 0;
    std::vector<std::string> keys;  // layer -> key
    std::vector<uint8_t> rgba;      // layers * size * size * 4
    bool ok() const { return size > 0 && !keys.empty(); }
    int layerOf(const std::string& key) const {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return static_cast<int>(i);
        return -1;
    }
};

inline TextureSet load(const std::string& path) {
    TextureSet t;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return t;
    char magic[4];
    uint32_t hdr[3] = {0, 0, 0};
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "VTEX", 4) != 0 ||
        std::fread(hdr, 4, 3, f) != 3 || hdr[0] != 1 || hdr[1] == 0 || hdr[1] > kMaxLayers ||
        hdr[2] == 0 || hdr[2] > 4096) {
        std::fclose(f);
        return t;
    }
    const size_t layerBytes = size_t(hdr[2]) * hdr[2] * 4;
    t.rgba.resize(layerBytes * hdr[1]);
    for (uint32_t i = 0; i < hdr[1]; ++i) {
        char key[33] = {};
        if (std::fread(key, 1, 32, f) != 32 ||
            std::fread(t.rgba.data() + layerBytes * i, 1, layerBytes, f) != layerBytes) {
            std::fclose(f);
            return TextureSet{};
        }
        t.keys.push_back(key);
    }
    std::fclose(f);
    t.size = static_cast<int>(hdr[2]);
    return t;
}

// How a material's texture sits on a cell.
struct Blend {
    const char* key;        // texture key in the manifest
    float tileCells;        // cells per texture repeat (world-space, triplanar)
    float tint;             // 0..1: how far a painted cell's paint tints the texture
    float coverage;         // 0..1: how much the texture covers the paint
    float maskFromLuma;     // 0..1: let bright texels reveal the paint underneath
};

// Block -> blend. Water and air have none (water keeps its shader tide).
inline const Blend* blendFor(wire::BlockId b) {
    static const Blend kConcrete{"concrete", 96.0f, 0.6f, 1.0f, 0.0f};
    static const Blend kSheet{"sheet_metal", 48.0f, 0.8f, 0.85f, 0.5f};
    static const Blend kGirder{"girder", 40.0f, 0.7f, 0.9f, 0.4f};
    static const Blend kWood{"wood", 64.0f, 0.6f, 1.0f, 0.0f};
    static const Blend kWoodDark{"wood_dark", 64.0f, 0.6f, 1.0f, 0.0f};
    static const Blend kDirt{"dirt", 80.0f, 0.4f, 1.0f, 0.0f};
    // Ground blends. tileCells is world-space triplanar repeats per cell, so a
    // ground tile is deliberately large: the point is to break up one 1 mm cube,
    // not to show a 1 mm photograph.
    static const Blend kSand{"sand", 120.0f, 0.5f, 1.0f, 0.0f};
    static const Blend kGrass{"grass", 96.0f, 0.5f, 1.0f, 0.0f};
    static const Blend kSnow{"snow", 110.0f, 0.4f, 1.0f, 0.0f};
    static const Blend kAsphalt{"asphalt", 72.0f, 0.5f, 1.0f, 0.0f};
    switch (b) {
    case wire::BlockId::Concrete: return &kConcrete;
    case wire::BlockId::SheetMetal: return &kSheet;
    case wire::BlockId::Girder: return &kGirder;
    case wire::BlockId::Wood: return &kWood;
    case wire::BlockId::WoodDark: return &kWoodDark;
    case wire::BlockId::Dirt: return &kDirt;
    case wire::BlockId::Sand: return &kSand;
    case wire::BlockId::Grass: return &kGrass;
    case wire::BlockId::Snow: return &kSnow;
    case wire::BlockId::Asphalt: return &kAsphalt;
    default: return nullptr;
    }
}

// What the mesher and the shader need, resolved once against the loaded set:
// layerPlus1[block] (0 = untextured) and per-layer blend parameters.
struct Table {
    uint8_t layerPlus1[256] = {};
    float params[kMaxLayers][4] = {}; // tileCells, tint, coverage, maskFromLuma
};

inline Table resolve(const TextureSet& set) {
    Table t;
    for (int b = 0; b < 256; ++b) {
        const Blend* bl = blendFor(static_cast<wire::BlockId>(b));
        if (!bl) continue;
        const int layer = set.layerOf(bl->key);
        if (layer < 0 || layer >= kMaxLayers) continue;
        t.layerPlus1[b] = static_cast<uint8_t>(layer + 1);
        t.params[layer][0] = bl->tileCells;
        t.params[layer][1] = bl->tint;
        t.params[layer][2] = bl->coverage;
        t.params[layer][3] = bl->maskFromLuma;
    }
    return t;
}

} // namespace tex
