#pragma once

// The intent vocabulary every caller shares: the error codes, the name grammar,
// the payload rules and the result envelope. Ported from logos-view-module-runtime's
// LogosIntent.h, with QVariant replaced by JSON, which is what crosses the wire.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace intents {

using json = nlohmann::json;

// ── Error codes ──────────────────────────────────────────────────────────────
// not_declared and unavailable are the broker's alone: both say whether a
// provider exists. A provider may report cancelled, timeout, failed and
// bad_request; anything else it sends becomes failed.
namespace err {
inline const std::string notDeclared = "not_declared";
inline const std::string unavailable = "unavailable";
inline const std::string badRequest  = "bad_request";
inline const std::string cancelled   = "cancelled";
inline const std::string timeout     = "timeout";
inline const std::string failed      = "failed";
}

bool isErrorCode(const std::string& code);

// A provider's error coerced into the codes it may use. Empty when ok.
std::string normalizeError(bool ok, const std::string& error);

// ── Names ────────────────────────────────────────────────────────────────────
//   intent  := segment ( "." segment ){1,3}
//   segment := [a-z] [a-z0-9]* ( "_" [a-z0-9]+ )*
//   3 <= length <= 64, byte-exact
bool isValidName(const std::string& name);

// "logos." belongs to the platform.
bool isPlatformName(const std::string& name);

// A package or module name: 1-64 of [A-Za-z0-9_.-], not starting with '.'.
// Every name the registry holds passes this, so names are safe to display.
bool isValidPackageName(const std::string& name);

// ── Reading JSON from outside ────────────────────────────────────────────────
struct JsonLimits {
    std::size_t maxBytes;
    int maxDepth;   // nested arrays and objects
    long maxNodes;  // an upper bound on values, counted without parsing
};

// Request params and provider results. The node bound is twice the payload
// rule's 1000 because the scan overcounts empty containers.
inline constexpr JsonLimits kPayloadLimits{1024 * 1024, 8, 2001};
// What the presenter sends: trusted, bounded only against accidents.
inline constexpr JsonLimits kPresenterLimits{16 * 1024 * 1024, 16, 1000000};
// A package's metadata.json.
inline constexpr JsonLimits kManifestLimits{1024 * 1024, 32, 20000};
// A provider's answer to `deliver`.
inline constexpr JsonLimits kReplyLimits{64 * 1024, 4, 64};

// Whether `text` stays within `limits`, by a scan that builds nothing.
bool withinLimits(const std::string& text, const JsonLimits& limits);

// Parsed JSON, or a discarded value when `text` is malformed or exceeds `limits`.
// Nothing is built for input the scan refuses, so depth can't exhaust the stack.
json parseBounded(const std::string& text, const JsonLimits& limits);

// Type-checked reads that never throw: a missing key or another type reads as absent.
const json* findKey(const json& object, const char* key);
std::string stringAt(const json& object, const char* key);
std::optional<bool> boolAt(const json& object, const char* key);

// ── Payloads ─────────────────────────────────────────────────────────────────
// Plain data only: null, bool, integers within ±2^53, finite numbers, strings,
// arrays and objects. At most 8 levels, 1000 nodes, 65536 characters per
// string, 64 per key, 1 Mi characters in total (and 1 MiB on the wire).
bool isCanonicalValue(const json& value);

// isCanonicalValue, and an object.
bool isCanonicalParams(const json& params);

// What a canonical value costs to hold: string bytes plus a fixed cost per node.
std::size_t approxBytes(const json& value);

// { ok, data, error }: data only when ok, error only when not.
json makeEnvelope(bool ok, const json& data, const std::string& error);

// The types a `params` or `result` entry may declare.
bool isParamType(const std::string& type);

// ── Text ─────────────────────────────────────────────────────────────────────
// Text a presenter will draw, with control and invisible formatting characters
// removed: C0, DEL, C1, bidirectional controls, zero-width characters, line and
// paragraph separators. Invalid UTF-8 is replaced with U+FFFD.
std::string displaySafe(const std::string& text);

// One field of a log line: control characters escaped, at most `max` bytes.
std::string forLog(const std::string& text, std::size_t max = 120);

} // namespace intents
