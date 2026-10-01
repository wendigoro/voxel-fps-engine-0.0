#pragma once
// Visual parameters: one registry for every adjustable look/effect setting.
//
// An effect declares its parameters here once (group, key, label, range,
// default). Everything else is derived from that list: the in-engine visuals
// menu draws a widget per parameter, presets are lists of key=value, and the
// settings file is the same keys. Adding an effect means declaring its
// parameters and reading them; no menu or save code changes.
//
// View-only (RULES.md "Render layers"): these values shape how sent data is
// drawn. The simulation never reads them, so changing one can never change a
// simulation outcome or the sim fingerprint.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vis {

enum class ParamType { Float, Int, Bool };

struct Param {
    const char* group;   // menu section
    const char* key;     // settings-file key; stable across versions
    const char* label;   // menu text
    const char* help;    // tooltip
    ParamType type;
    float* f = nullptr;
    int* i = nullptr;
    bool* b = nullptr;
    float minV = 0.0f, maxV = 1.0f; // Float / Int range
    float def = 0.0f;               // default (0/1 for Bool)

    float get() const {
        switch (type) {
        case ParamType::Float: return *f;
        case ParamType::Int: return static_cast<float>(*i);
        case ParamType::Bool: return *b ? 1.0f : 0.0f;
        }
        return 0.0f;
    }
    // Set from a number, clamped to the declared range. Non-finite is ignored.
    void set(float v) {
        if (!std::isfinite(v)) return;
        switch (type) {
        case ParamType::Float: *f = std::clamp(v, minV, maxV); break;
        case ParamType::Int: *i = static_cast<int>(std::lround(std::clamp(v, minV, maxV))); break;
        case ParamType::Bool: *b = v != 0.0f; break;
        }
    }
};

struct Preset {
    const char* name;
    std::vector<std::pair<const char*, float>> values; // keys not listed take their default
};

class Registry {
public:
    void addFloat(const char* group, const char* key, const char* label, float* v, float minV,
                  float maxV, float def, const char* help = "") {
        Param p{group, key, label, help, ParamType::Float};
        p.f = v; p.minV = minV; p.maxV = maxV; p.def = def;
        *v = def;
        params_.push_back(p);
    }
    void addInt(const char* group, const char* key, const char* label, int* v, int minV, int maxV,
                int def, const char* help = "") {
        Param p{group, key, label, help, ParamType::Int};
        p.i = v; p.minV = float(minV); p.maxV = float(maxV); p.def = float(def);
        *v = def;
        params_.push_back(p);
    }
    void addBool(const char* group, const char* key, const char* label, bool* v, bool def,
                 const char* help = "") {
        Param p{group, key, label, help, ParamType::Bool};
        p.b = v; p.minV = 0; p.maxV = 1; p.def = def ? 1.0f : 0.0f;
        *v = def;
        params_.push_back(p);
    }
    void addPreset(Preset p) { presets_.push_back(std::move(p)); }

    std::vector<Param>& params() { return params_; }
    const std::vector<Param>& params() const { return params_; }
    const std::vector<Preset>& presets() const { return presets_; }

    Param* find(const std::string& key) {
        for (auto& p : params_)
            if (key == p.key) return &p;
        return nullptr;
    }

    void resetDefaults() {
        for (auto& p : params_) p.set(p.def);
        ++revision_;
    }

    // Defaults, then the preset's values. Returns false for an unknown name.
    bool applyPreset(const std::string& name) {
        for (const auto& pr : presets_) {
            if (name != pr.name) continue;
            for (auto& p : params_) p.set(p.def);
            for (const auto& kv : pr.values)
                if (Param* p = find(kv.first)) p->set(kv.second);
            ++revision_;
            return true;
        }
        return false;
    }

    // {"key": value, ...} in declaration order.
    std::string toJson() const {
        std::string out = "{\n";
        char buf[160];
        for (size_t n = 0; n < params_.size(); ++n) {
            const Param& p = params_[n];
            if (p.type == ParamType::Float)
                std::snprintf(buf, sizeof(buf), "  \"%s\": %.4f", p.key, p.get());
            else
                std::snprintf(buf, sizeof(buf), "  \"%s\": %d", p.key, static_cast<int>(p.get()));
            out += buf;
            out += (n + 1 < params_.size()) ? ",\n" : "\n";
        }
        return out + "}\n";
    }

    // Read {"key": number, ...}. Unknown keys are ignored (an old file never
    // breaks a newer build), missing keys keep their current value, and every
    // value is clamped to its declared range. Returns the number applied.
    int fromJson(const std::string& text) {
        int applied = 0;
        size_t i = 0;
        while ((i = text.find('"', i)) != std::string::npos) {
            const size_t j = text.find('"', i + 1);
            if (j == std::string::npos) break;
            const std::string key = text.substr(i + 1, j - i - 1);
            size_t k = text.find(':', j);
            if (k == std::string::npos) break;
            ++k;
            while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) ++k;
            char* end = nullptr;
            const float v = std::strtof(text.c_str() + k, &end);
            if (end && end != text.c_str() + k) {
                if (Param* p = find(key)) { p->set(v); ++applied; }
            } else if (text.compare(k, 4, "true") == 0 || text.compare(k, 5, "false") == 0) {
                if (Param* p = find(key)) { p->set(text.compare(k, 4, "true") == 0 ? 1.0f : 0.0f); ++applied; }
            }
            i = (end && end > text.c_str() + k) ? static_cast<size_t>(end - text.c_str()) : j + 1;
        }
        ++revision_;
        return applied;
    }

    // Bumped on every bulk change (preset, reset, load); widgets bump it too.
    // Consumers compare it to rebuild what depends on parameters.
    unsigned revision() const { return revision_; }
    void touch() { ++revision_; }

private:
    std::vector<Param> params_;
    std::vector<Preset> presets_;
    unsigned revision_ = 0;
};

// ---- the engine's visual settings ----

struct Settings {
    // Resolution
    float renderScale = 1.0f; // world render target size, fraction of the window
    bool upscaleNearest = true;
    // Post (applied to the whole world image, in this order)
    int posterizeLevels = 0;  // 0 = off
    float dither = 0.0f;
    float crush = 1.0f;       // gamma on the final colour; 1 = none
    // World shading
    float banding = 1.0f;     // scales the shader's colour-step quantization; 0 = off
    float fisheye = 1.0f;     // scales the lens curve; 0 = rectilinear
    // Textures (Surface layer)
    bool textures = true;
    float textureStrength = 1.0f; // 0 = the flat colour, 1 = fully textured
    float textureScale = 1.0f;    // multiplies every material's tile size
};

// Declare every parameter. Defaults reproduce the look the engine shipped
// with before the menu existed (full-resolution render, the shader's own
// banding and fisheye, no post stage).
inline void registerEngineParams(Registry& r, Settings& s) {
    r.addFloat("Resolution", "render_scale", "Render scale", &s.renderScale, 0.25f, 1.0f, 1.0f,
               "World render resolution as a fraction of the window. Lower is chunkier and faster.");
    r.addBool("Resolution", "upscale_nearest", "Nearest upscale", &s.upscaleNearest, true,
              "Blocky (nearest) or smooth (linear) upscaling of a reduced render.");
    r.addInt("Post", "posterize_levels", "Posterize levels", &s.posterizeLevels, 0, 64, 0,
             "Colour levels per channel after the world is drawn. 0 turns it off.");
    r.addFloat("Post", "dither", "Dither", &s.dither, 0.0f, 1.0f, 0.0f,
               "Noise added before posterizing, to break up bands.");
    r.addFloat("Post", "crush", "Crush", &s.crush, 0.6f, 1.6f, 1.0f,
               "Gamma on the final image. Above 1 deepens shadows.");
    r.addFloat("World", "banding", "Shading banding", &s.banding, 0.0f, 3.0f, 1.0f,
               "Colour stepping inside world shading. 1 is the original look, 0 is smooth.");
    r.addFloat("World", "fisheye", "Fisheye", &s.fisheye, 0.0f, 1.5f, 1.0f,
               "Lens curve strength. 0 is a plain perspective.");

    r.addBool("Textures", "textures", "Textures", &s.textures, true,
              "Photo textures on world surfaces (CC0, data/textures/manifest.json).");
    r.addFloat("Textures", "texture_strength", "Texture strength", &s.textureStrength, 0.0f, 1.0f, 1.0f,
               "Blend between the flat material colour (0) and the texture (1).");
    r.addFloat("Textures", "texture_scale", "Texture scale", &s.textureScale, 0.25f, 4.0f, 1.0f,
               "Size of one texture repeat. Larger spreads each texture over more cells.");

    r.addPreset({"Original", {{"textures", 0.0f}}});
    r.addPreset({"Retro", {{"render_scale", 0.5f}, {"posterize_levels", 16.0f}, {"dither", 0.35f},
                           {"crush", 1.15f}}});
    r.addPreset({"Clean", {{"banding", 0.0f}, {"fisheye", 0.35f}, {"upscale_nearest", 0.0f}}});
}

// Headless contract checks (engine smoke). Uses its own Settings, so it never
// disturbs the live values.
inline bool selfTest() {
    Registry r;
    Settings s;
    registerEngineParams(r, s);
    bool ok = s.renderScale == 1.0f && s.posterizeLevels == 0 && r.find("fisheye") != nullptr;
    // Presets apply on top of defaults, and an unknown one is refused.
    ok = ok && r.applyPreset("Retro") && s.renderScale == 0.5f && s.posterizeLevels == 16 &&
         s.fisheye == 1.0f && !r.applyPreset("no such preset");
    // A save round-trips exactly.
    const std::string saved = r.toJson();
    r.resetDefaults();
    ok = ok && s.renderScale == 1.0f && r.fromJson(saved) == static_cast<int>(r.params().size()) &&
         s.renderScale == 0.5f && s.posterizeLevels == 16 && std::fabs(s.dither - 0.35f) < 1e-4f;
    // Unknown keys are ignored and out-of-range values clamp.
    r.fromJson("{\"render_scale\": 9.0, \"no_such_key\": 3, \"posterize_levels\": -4}");
    ok = ok && s.renderScale == 1.0f && s.posterizeLevels == 0;
    return ok;
}

} // namespace vis
