#pragma once
// Minimal hand-rolled JSON extraction for our controlled data files.
//
// The def files (projectiles, ammo, weapons, items) and the map documents are
// written by our Python exporter, so the schema is ours and deliberately flat.
// These helpers are the single home for that reader contract: every def loader
// and every MAP reader feeds through jsonReadText + jsonExtract*. Keeping them
// together is why the smoke self-test (jsonxSelfTest) can prove the whole
// reader contract in one place, and why a future real parser can replace this
// header without touching any consumer.
//
// Determinism note: pure string parsing, no clocks, no randomness, no ordering
// that depends on iteration. Safe for the fixed-timestep sim.

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Read an entire text file. Returns "" (and sets ok=false) on failure so callers
// can distinguish "missing/unreadable" from "genuinely empty (rare)".
inline std::string jsonReadText(const std::string& path, bool* ok = nullptr) {
    std::ifstream in(path);
    if (!in) {
        if (ok) *ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

// Very small JSON helpers (schema is controlled by our Python exporter).
inline std::string jsonExtractString(const std::string& obj, const char* key, const std::string& fallback = {}) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat);
    if (k == std::string::npos) return fallback;
    k = obj.find(':', k);
    if (k == std::string::npos) return fallback;
    k = obj.find('"', k);
    if (k == std::string::npos) return fallback;
    size_t e = obj.find('"', k + 1);
    if (e == std::string::npos) return fallback;
    return obj.substr(k + 1, e - k - 1);
}

inline float jsonExtractFloat(const std::string& obj, const char* key, float fallback) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat);
    if (k == std::string::npos) return fallback;
    k = obj.find(':', k);
    if (k == std::string::npos) return fallback;
    k++;
    while (k < obj.size() && (obj[k] == ' ' || obj[k] == '\t')) k++;
    try {
        return std::stof(obj.substr(k));
    } catch (...) {
        return fallback;
    }
}

inline bool jsonExtractBool(const std::string& obj, const char* key, bool fallback) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat);
    if (k == std::string::npos) return fallback;
    k = obj.find(':', k);
    if (k == std::string::npos) return fallback;
    k++;
    while (k < obj.size() && (obj[k] == ' ' || obj[k] == '\t' || obj[k] == '\n' || obj[k] == '\r')) k++;
    if (k >= obj.size()) return fallback;
    // Accept true/false and numeric 0/1 (weapon export uses 0|1).
    if (obj[k] == '1') return true;
    if (obj[k] == '0') return false;
    if (k + 4 <= obj.size() && obj.compare(k, 4, "true") == 0) return true;
    if (k + 5 <= obj.size() && obj.compare(k, 5, "false") == 0) return false;
    return fallback;
}

// Extract balanced {...} body immediately after "key": (empty if missing).
inline std::string jsonExtractObjectBody(const std::string& text, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = text.find(pat);
    if (k == std::string::npos) return {};
    k = text.find('{', k);
    if (k == std::string::npos) return {};
    int depth = 0;
    for (size_t i = k; i < text.size(); ++i) {
        char c = text[i];
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return text.substr(k, i - k + 1);
        }
    }
    return {};
}

// Extract first JSON array body after "key": [ ... ] (empty if missing).
inline std::string jsonExtractArrayBody(const std::string& text, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = text.find(pat);
    if (k == std::string::npos) return {};
    k = text.find('[', k);
    if (k == std::string::npos) return {};
    int depth = 0;
    for (size_t i = k; i < text.size(); ++i) {
        char c = text[i];
        if (c == '[') depth++;
        else if (c == ']') {
            depth--;
            if (depth == 0) return text.substr(k, i - k + 1);
        }
    }
    return {};
}

// Collect every integer in the array body after "key". Handles both flat
// arrays ([3,3,4]) and nested ones ([[0,0,0],[1,0,0]]) by flattening, which is
// all our controlled exporter emits.
inline std::vector<int> jsonExtractIntArray(const std::string& text, const char* key) {
    const std::string body = jsonExtractArrayBody(text, key);
    std::vector<int> out;
    size_t i = 0;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t j = i;
            if (body[j] == '-') ++j;
            while (j < body.size() && body[j] >= '0' && body[j] <= '9') ++j;
            try {
                out.push_back(std::stoi(body.substr(i, j - i)));
            } catch (...) {
            }
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

inline std::vector<float> jsonExtractFloatArray(const std::string& text, const char* key) {
    const std::string body = jsonExtractArrayBody(text, key);
    std::vector<float> out;
    size_t i = 0;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            size_t j = i;
            if (body[j] == '-' || body[j] == '+') ++j;
            while (j < body.size() && (body[j] == '.' || body[j] == 'e' || body[j] == 'E' ||
                                       (body[j] >= '0' && body[j] <= '9') || body[j] == '-' ||
                                       body[j] == '+'))
                ++j;
            try {
                out.push_back(std::stof(body.substr(i, j - i)));
            } catch (...) {
            }
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

// Self-test for the reader contract. Exercising every extractor against a known
// document (missing keys, numeric bools, nested arrays, deep object bodies) so
// the smoke gate can prove the parser rather than whichever defs happen to load.
inline bool jsonxSelfTest() {
    bool ok = true;

    // jsonExtractString: present, missing, wrong-type value.
    ok = ok && jsonExtractString("{\"id\":\"slug\"}", "id", "") == "slug";
    ok = ok && jsonExtractString("{\"id\":\"slug\"}", "name", "zzz") == "zzz";
    ok = ok && jsonExtractString("{\"id\":123}", "id", "") == ""; // non-string falls back

    // jsonExtractFloat: present (ws padded), missing, non-numeric.
    ok = ok && jsonExtractFloat("{\"mass\": 2.5 }", "mass", 0.0f) == 2.5f;
    ok = ok && jsonExtractFloat("{\"mass\":2.5}", "mass", 0.0f) == 2.5f;
    ok = ok && jsonExtractFloat("{\"mass\":\"x\"}", "mass", 7.0f) == 7.0f;
    ok = ok && jsonExtractFloat("{\"a\":1}", "mass", 9.0f) == 9.0f;

    // jsonExtractBool: true/false words and numeric 0/1.
    ok = ok && jsonExtractBool("{\"hitscan\":true}", "hitscan", false);
    ok = ok && !jsonExtractBool("{\"hitscan\":false}", "hitscan", true);
    ok = ok && jsonExtractBool("{\"hitscan\":1}", "hitscan", false);
    ok = ok && !jsonExtractBool("{\"hitscan\":0}", "hitscan", true);
    ok = ok && jsonExtractBool("{\"a\":1}", "hitscan", true); // missing -> fallback

    // jsonExtractObjectBody: nested braces must stay balanced.
    ok = ok && jsonExtractObjectBody("{\"stats\":{\"a\":{\"b\":1},\"c\":2}}", "stats") ==
                      "{\"a\":{\"b\":1},\"c\":2}";
    ok = ok && jsonExtractObjectBody("{\"stats\":1}", "stats").empty();
    ok = ok && jsonExtractObjectBody("{\"a\":{}, \"stats\":{\"x\":[1,{},{}]}}", "stats") ==
                      "{\"x\":[1,{},{}]}";

    // jsonExtractArrayBody: flat + nested.
    ok = ok && jsonExtractArrayBody("{\"v\":[1,2,3]}", "v") == "[1,2,3]";
    ok = ok && jsonExtractArrayBody("{\"v\":[[0,0],[1,0]]}", "v") == "[[0,0],[1,0]]";
    ok = ok && jsonExtractArrayBody("{\"a\":{}}", "v").empty();

    // jsonExtractIntArray: flat, nested flatten, negatives, missing.
    ok = ok && jsonExtractIntArray("{\"size\":[3,3,4]}", "size").size() == 3;
    ok = ok && jsonExtractIntArray("{\"v\":[[0,0,0],[1,0,0]]}", "v").size() == 6;
    ok = ok && jsonExtractIntArray("{\"v\":[-2,5]}", "v") == std::vector<int>({-2, 5});
    ok = ok && jsonExtractIntArray("{\"a\":{}}", "v").empty();

    // jsonExtractFloatArray: decimals preserved.
    ok = ok && jsonExtractFloatArray("{\"c\":[0.5,0.25,1.0]}", "c").size() == 3;
    ok = ok && jsonExtractFloatArray("{\"c\":[0.5,0.25,1.0]}", "c")[0] == 0.5f;
    ok = ok && jsonExtractFloatArray("{\"a\":{}}", "c").empty();

    // jsonReadText: missing file must report failure and "".
    bool fileOk = true;
    const std::string m = jsonReadText("C:\\__no__such__file__never__.json", &fileOk);
    ok = ok && !fileOk && m.empty();

    return ok;
}