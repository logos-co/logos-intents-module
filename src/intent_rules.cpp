#include "intent_rules.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace intents {

namespace {

constexpr int kMaxDepth = 8;
constexpr int kMaxNodes = 1000;
constexpr std::size_t kMaxStringChars = 65536;
constexpr std::size_t kMaxKeyChars = 64;
constexpr std::size_t kMaxTotalChars = 1024 * 1024;
constexpr std::int64_t kMaxSafeInteger = 9007199254740991LL;
constexpr std::size_t kNodeCost = 16;

// Code points in valid UTF-8 (nlohmann has already validated it).
std::size_t charCount(const std::string& s)
{
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

struct Budget {
    int nodes = 0;
    std::size_t chars = 0;
};

bool check(const json& v, int depth, Budget& budget)
{
    if (depth > kMaxDepth) return false;
    if (++budget.nodes > kMaxNodes) return false;

    switch (v.type()) {
    case json::value_t::null:
    case json::value_t::boolean:
        return true;
    case json::value_t::number_integer: {
        const std::int64_t n = v.get<std::int64_t>();
        return n <= kMaxSafeInteger && n >= -kMaxSafeInteger;
    }
    case json::value_t::number_unsigned:
        return v.get<std::uint64_t>() <= static_cast<std::uint64_t>(kMaxSafeInteger);
    case json::value_t::number_float:
        return std::isfinite(v.get<double>());
    case json::value_t::string: {
        const std::size_t n = charCount(v.get_ref<const std::string&>());
        if (n > kMaxStringChars) return false;
        budget.chars += n;
        return budget.chars <= kMaxTotalChars;
    }
    case json::value_t::array:
        for (const json& element : v)
            if (!check(element, depth + 1, budget)) return false;
        return true;
    case json::value_t::object:
        for (auto it = v.begin(); it != v.end(); ++it) {
            const std::size_t k = charCount(it.key());
            if (k > kMaxKeyChars) return false;
            budget.chars += k;
            if (budget.chars > kMaxTotalChars) return false;
            if (!check(it.value(), depth + 1, budget)) return false;
        }
        return true;
    default:
        return false;   // binary, discarded
    }
}

bool isValidSegment(const std::string& seg)
{
    if (seg.empty()) return false;
    if (seg[0] < 'a' || seg[0] > 'z') return false;
    bool prevUnderscore = false;
    for (std::size_t i = 0; i < seg.size(); ++i) {
        const char c = seg[i];
        if (c == '_') {
            if (prevUnderscore || i == seg.size() - 1) return false;
            prevUnderscore = true;
            continue;
        }
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!lower && !digit) return false;
        prevUnderscore = false;
    }
    return true;
}

// The next code point at `i`, or false for an invalid sequence (overlong forms,
// surrogates and values past U+10FFFF included).
bool decodeNext(const std::string& text, std::size_t i, std::uint32_t* cp, int* len)
{
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80)                { *cp = c;        *len = 1; }
    else if ((c & 0xE0) == 0xC0) { *cp = c & 0x1F; *len = 2; }
    else if ((c & 0xF0) == 0xE0) { *cp = c & 0x0F; *len = 3; }
    else if ((c & 0xF8) == 0xF0) { *cp = c & 0x07; *len = 4; }
    else return false;
    if (i + static_cast<std::size_t>(*len) > text.size()) return false;
    for (int k = 1; k < *len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(text[i + k]);
        if ((cc & 0xC0) != 0x80) return false;
        *cp = (*cp << 6) | (cc & 0x3F);
    }
    if ((*len == 2 && *cp < 0x80) || (*len == 3 && *cp < 0x800) || (*len == 4 && *cp < 0x10000)
        || *cp > 0x10FFFF || (*cp >= 0xD800 && *cp <= 0xDFFF))
        return false;
    return true;
}

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isHiddenControl(std::uint32_t cp)
{
    return cp < 0x20 || cp == 0x7F
        || (cp >= 0x80 && cp <= 0x9F)
        || cp == 0x061C || cp == 0x180E
        || (cp >= 0x200B && cp <= 0x200F)
        || (cp >= 0x2028 && cp <= 0x202E)
        || (cp >= 0x2060 && cp <= 0x2064)
        || (cp >= 0x2066 && cp <= 0x2069)
        || cp == 0xFEFF
        || (cp >= 0xFFF9 && cp <= 0xFFFB);
}

std::size_t approx(const json& v)
{
    std::size_t n = kNodeCost;
    if (v.is_string()) {
        n += v.get_ref<const std::string&>().size();
    } else if (v.is_array()) {
        for (const json& e : v) n += approx(e);
    } else if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) n += it.key().size() + approx(it.value());
    }
    return n;
}

} // namespace

bool isErrorCode(const std::string& code)
{
    return code == err::notDeclared || code == err::unavailable || code == err::badRequest
        || code == err::cancelled || code == err::timeout || code == err::failed;
}

std::string normalizeError(bool ok, const std::string& error)
{
    if (ok) return {};
    if (error == err::cancelled || error == err::timeout || error == err::failed
        || error == err::badRequest)
        return error;
    return err::failed;
}

bool isValidName(const std::string& name)
{
    if (name.size() < 3 || name.size() > 64) return false;
    int segments = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = name.find('.', start);
        const std::string seg = name.substr(start, dot == std::string::npos ? std::string::npos
                                                                            : dot - start);
        if (!isValidSegment(seg)) return false;
        ++segments;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return segments >= 2 && segments <= 4;
}

bool isPlatformName(const std::string& name)
{
    return name.rfind("logos.", 0) == 0;
}

bool isValidPackageName(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                     || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool withinLimits(const std::string& text, const JsonLimits& limits)
{
    if (text.size() > limits.maxBytes) return false;
    // Values <= openers + commas + 1, so bounding those bounds the parse.
    int depth = 0;
    long nodes = 1;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        switch (c) {
        case '"':
            inString = true;
            break;
        case '[':
        case '{':
            if (++depth > limits.maxDepth || ++nodes > limits.maxNodes) return false;
            break;
        case ']':
        case '}':
            if (depth > 0) --depth;
            break;
        case ',':
            if (++nodes > limits.maxNodes) return false;
            break;
        default:
            break;
        }
    }
    return true;
}

json parseBounded(const std::string& text, const JsonLimits& limits)
{
    if (!withinLimits(text, limits)) return json::value_t::discarded;
    return json::parse(text, nullptr, /*allow_exceptions=*/false);
}

const json* findKey(const json& object, const char* key)
{
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

std::string stringAt(const json& object, const char* key)
{
    const json* v = findKey(object, key);
    return v && v->is_string() ? v->get<std::string>() : std::string();
}

std::optional<bool> boolAt(const json& object, const char* key)
{
    const json* v = findKey(object, key);
    if (!v || !v->is_boolean()) return std::nullopt;
    return v->get<bool>();
}

bool isCanonicalValue(const json& value)
{
    Budget budget;
    return check(value, 1, budget);
}

bool isCanonicalParams(const json& params)
{
    return params.is_object() && isCanonicalValue(params);
}

std::size_t approxBytes(const json& value)
{
    return approx(value);
}

json makeEnvelope(bool ok, const json& data, const std::string& error)
{
    return json{
        {"ok", ok},
        {"data", ok ? data : json()},
        {"error", ok ? std::string() : error},
    };
}

std::string displaySafe(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        std::uint32_t cp = 0;
        int len = 0;
        if (!decodeNext(text, i, &cp, &len)) {
            appendUtf8(out, 0xFFFD);
            ++i;
            continue;
        }
        if (!isHiddenControl(cp)) appendUtf8(out, cp);
        i += static_cast<std::size_t>(len);
    }
    return out;
}

std::string forLog(const std::string& text, std::size_t max)
{
    std::string out;
    std::size_t i = 0;
    char buf[16];
    while (i < text.size()) {
        if (out.size() >= max) {
            out += "\xE2\x80\xA6";   // …
            break;
        }
        std::uint32_t cp = 0;
        int len = 0;
        if (!decodeNext(text, i, &cp, &len)) {
            std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned char>(text[i]));
            out += buf;
            ++i;
            continue;
        }
        if (cp < 0x20 || cp == 0x7F) {
            std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned>(cp));
            out += buf;
        } else if (isHiddenControl(cp)) {
            std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(cp));
            out += buf;
        } else {
            out.append(text, i, static_cast<std::size_t>(len));
        }
        i += static_cast<std::size_t>(len);
    }
    return out;
}

} // namespace intents
