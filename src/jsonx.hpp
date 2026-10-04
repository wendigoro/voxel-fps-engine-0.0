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
    // Walk from the opening quote, honoring backslash escapes, to the closing
    // unescaped quote. Mirrors the exporter's appendEscaped/unescape so a quote
    // in an event or NPC name survives a round trip (schema.md "Map mode"). A
    // newline or unknown escape makes the value malformed -> fallback.
    std::string out;
    ++k;
    while (k < obj.size()) {
        const char c = obj[k];
        if (c == '\\') {
            if (k + 1 >= obj.size()) return fallback;
            const char n = obj[k + 1];
            if (n == 'u') {
                // \uXXXX occupies six characters; consume all of them here.
                if (k + 6 > obj.size()) return fallback;
                unsigned v = 0;
                for (int h = 0; h < 4; ++h) {
                    const char hc = obj[k + 2 + h];
                    v <<= 4;
                    if (hc >= '0' && hc <= '9') v |= (hc - '0');
                    else if (hc >= 'a' && hc <= 'f') v |= (hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') v |= (hc - 'A' + 10);
                    else return fallback;
                }
                out += static_cast<char>(v);
                k += 6;
                continue;
            }
            switch (n) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                default: return fallback; // unknown escape: malformed
            }
            k += 2;
            continue;
        }
        if (c == '"') return out;
        out += c;
        ++k;
    }
    return fallback;
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

// Is the key present at all? Every extractor above takes a fallback, which
// cannot tell a field the author omitted from one whose value happens to equal
// the fallback. A field that is REQUIRED must be told apart from one that
// merely defaults, or a missing value silently becomes a default (the terrain
// seed is the case that matters: a defaulted seed would be a silent clock).
inline bool jsonxHasKey(const std::string& obj, const char* key) {
    return obj.find(std::string("\"") + key + "\"") != std::string::npos;
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

// Find the next top-level balanced {...} object at or after pos, skipping over
// quoted strings so a brace inside a string value cannot unbalance it. On
// success returns true and fills start/end as the object's [start, end)
// half-open range. Advancing pos = end iterates array elements. This is the
// walker the MAP reader uses for entity/voxel arrays (flat find('{')/find('}')
// breaks the moment a string contains a brace).
inline bool jsonxNextObject(const std::string& text, size_t pos, size_t& start, size_t& end) {
    size_t i = pos;
    while (i < text.size()) {
        if (text[i] == '"') {
            // Skip a quoted string, honoring backslash escapes.
            ++i;
            while (i < text.size()) {
                if (text[i] == '\\') i += 1;
                else if (text[i] == '"') { ++i; break; }
                i++;
            }
            continue;
        }
        if (text[i] == '{') {
            start = i;
            int depth = 1;
            ++i;
            while (i < text.size() && depth > 0) {
                if (text[i] == '"') {
                    ++i;
                    while (i < text.size()) {
                        if (text[i] == '\\') i += 1;
                        else if (text[i] == '"') { ++i; break; }
                        i++;
                    }
                    continue;
                }
                if (text[i] == '{') depth++;
                else if (text[i] == '}') {
                    depth--;
                    if (depth == 0) { end = i + 1; return true; }
                }
                i++;
            }
            return false;
        }
        i++;
    }
    return false;
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

    // jsonExtractString escape handling: escaped quote, backslash, control
    // escapes, and a surrogate-style \uXXXX (matches exporter's appendEscaped).
    ok = ok && jsonExtractString("{\"n\":\"Test \\\"Event\\\"\"}", "n", "") == "Test \"Event\"";
    ok = ok && jsonExtractString("{\"n\":\"a\\\\b\"}", "n", "") == "a\\b";
    ok = ok && jsonExtractString("{\"n\":\"line\\nnext\"}", "n", "") == "line\nnext";
    ok = ok && jsonExtractString("{\"n\":\"tag\\tend\"}", "n", "") == "tag\tend";
    ok = ok && jsonExtractString("{\"n\":\"\\u0041B\"}", "n", "") == "AB";
    ok = ok && jsonExtractString("{\"n\":\"bad\\q\"}", "n", "") == ""; // unknown escape -> fallback

    // jsonxNextObject: walks sibling array elements (the array body, exactly
    // what jsonExtractArrayBody + the MAP reader pass in), skips braces inside
    // strings, and exposes an end-exclusive range that can be advanced.
    {
        const std::string arr = "[{\"name\":\"a{b}c\",\"x\":1},{\"name\":\"d\",\"x\":2}]";
        size_t s = 0, e = 0;
        ok = ok && jsonxNextObject(arr, 0, s, e);
        ok = ok && jsonExtractString(arr.substr(s, e - s), "name", "") == "a{b}c";
        ok = ok && jsonxNextObject(arr, e, s, e);
        ok = ok && jsonExtractString(arr.substr(s, e - s), "name", "") == "d";
        ok = ok && !jsonxNextObject(arr, e, s, e); // no third object
    }
    {
        // Nested arrays inside a top-level object still balance: given a whole
        // document the walker returns the document's own balanced object, so
        // fields reachable from it extract correctly.
        const std::string doc = "{\"routes\":[{\"id\":\"r\",\"nodes\":[{\"x\":20,\"a\":\"{\"}]}]}";
        size_t s = 0, e = 0;
        ok = ok && jsonxNextObject(doc, 0, s, e);
        ok = ok && jsonExtractString(doc.substr(s, e - s), "id", "") == "r";
    }

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

    // jsonxHasKey: separates "authored as the fallback" from "not authored",
    // which no extractor taking a fallback can do on its own.
    ok = ok && jsonxHasKey("{\"seed\":0}", "seed");
    ok = ok && jsonxHasKey("{\"seed\":7,\"base\":3}", "seed");
    ok = ok && !jsonxHasKey("{\"base\":3}", "seed");
    ok = ok && !jsonxHasKey("{}", "seed");

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