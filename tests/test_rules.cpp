#include <logos_test.h>

#include "intent_rules.h"

using namespace intents;

LOGOS_TEST(names_follow_the_grammar)
{
    LOGOS_ASSERT_TRUE(isValidName("signing.approve"));
    LOGOS_ASSERT_TRUE(isValidName("evm.signing.approve"));
    LOGOS_ASSERT_TRUE(isValidName("a.b_c.d2.e"));
    LOGOS_ASSERT_FALSE(isValidName("signing"));            // one segment
    LOGOS_ASSERT_FALSE(isValidName("a.b.c.d.e"));          // five
    LOGOS_ASSERT_FALSE(isValidName("Signing.approve"));    // uppercase
    LOGOS_ASSERT_FALSE(isValidName("signing..approve"));
    LOGOS_ASSERT_FALSE(isValidName("signing.approve_"));
    LOGOS_ASSERT_FALSE(isValidName("signing.a__b"));
    LOGOS_ASSERT_FALSE(isValidName("signing.1approve"));
    LOGOS_ASSERT_FALSE(isValidName("signing.appr\xc3\xb6ve"));
    LOGOS_ASSERT_FALSE(isValidName(std::string("a.") + std::string(63, 'b')));
}

LOGOS_TEST(a_provider_cannot_claim_broker_only_codes)
{
    LOGOS_ASSERT_EQ(normalizeError(true, "failed"), std::string());
    LOGOS_ASSERT_EQ(normalizeError(false, "cancelled"), err::cancelled);
    LOGOS_ASSERT_EQ(normalizeError(false, "bad_request"), err::badRequest);
    LOGOS_ASSERT_EQ(normalizeError(false, "unavailable"), err::failed);
    LOGOS_ASSERT_EQ(normalizeError(false, "not_declared"), err::failed);
    LOGOS_ASSERT_EQ(normalizeError(false, "no such key in keystore"), err::failed);
}

LOGOS_TEST(payloads_are_plain_bounded_data)
{
    LOGOS_ASSERT_TRUE(isCanonicalParams(json{{"a", 1}, {"b", json::array({true, nullptr, 1.5, "x"})}}));
    LOGOS_ASSERT_FALSE(isCanonicalParams(json::array()));
    LOGOS_ASSERT_FALSE(isCanonicalValue(json(9007199254740992LL)));
    LOGOS_ASSERT_FALSE(isCanonicalValue(json::binary({1, 2, 3})));

    json deep = json::object();
    json* at = &deep;
    for (int i = 0; i < 9; ++i) {
        (*at)["x"] = json::object();
        at = &(*at)["x"];
    }
    LOGOS_ASSERT_FALSE(isCanonicalParams(deep));

    json many = json::array();
    for (int i = 0; i < 1000; ++i) many.push_back(i);
    LOGOS_ASSERT_FALSE(isCanonicalValue(many));

    LOGOS_ASSERT_FALSE(isCanonicalValue(json(std::string(65537, 'a'))));
    LOGOS_ASSERT_FALSE(isCanonicalParams(json{{std::string(65, 'k'), 1}}));

    // The aggregate bound: 1 Mi characters across the payload.
    json atLimit = json::array();
    for (int i = 0; i < 16; ++i) atLimit.push_back(std::string(65536, 'a'));
    LOGOS_ASSERT_TRUE(isCanonicalValue(atLimit));
    atLimit.push_back("a");
    LOGOS_ASSERT_FALSE(isCanonicalValue(atLimit));
}

LOGOS_TEST(untrusted_json_that_does_not_parse_is_discarded)
{
    LOGOS_ASSERT_TRUE(parseBounded("{", kPayloadLimits).is_discarded());
    LOGOS_ASSERT_TRUE(parseBounded("\"\xff\"", kPayloadLimits).is_discarded());
    LOGOS_ASSERT_TRUE(parseBounded("{\"a\":1}", kPayloadLimits).is_object());
}

// Nothing is built for input past the limits, so depth can't exhaust the stack
// and width can't exhaust memory.
LOGOS_TEST(json_past_its_limits_is_refused_before_it_is_built)
{
    const JsonLimits small{1024, 3, 10};
    LOGOS_ASSERT_TRUE(withinLimits("[[[1]]]", small));
    LOGOS_ASSERT_FALSE(withinLimits("[[[[1]]]]", small));
    LOGOS_ASSERT_TRUE(withinLimits("[1,2,3,4,5,6,7,8,9]", small));
    LOGOS_ASSERT_FALSE(withinLimits("[1,2,3,4,5,6,7,8,9,10]", small));
    LOGOS_ASSERT_FALSE(withinLimits(std::string(1025, ' '), small));
    // Brackets and commas inside strings, escaped quotes included, don't count.
    LOGOS_ASSERT_TRUE(withinLimits(R"(["[[[[,,,,,,,,,,\"[[[["])", small));

    // The payload limits never refuse what the payload rules accept.
    json deepest = json::object();
    json* at = &deepest;
    for (int i = 0; i < 7; ++i) {
        (*at)["x"] = json::object();
        at = &(*at)["x"];
    }
    LOGOS_ASSERT_TRUE(isCanonicalParams(deepest));
    LOGOS_ASSERT_TRUE(parseBounded(deepest.dump(), kPayloadLimits).is_object());
    json widest = json::object();
    for (int i = 0; i < 999; ++i) widest["k" + std::to_string(i)] = json::array();
    LOGOS_ASSERT_TRUE(isCanonicalParams(widest));
    LOGOS_ASSERT_TRUE(parseBounded(widest.dump(), kPayloadLimits).is_object());

    const std::string tooDeep = std::string(300000, '[') + std::string(300000, ']');
    LOGOS_ASSERT_TRUE(parseBounded(tooDeep, kManifestLimits).is_discarded());
}

LOGOS_TEST(typed_reads_never_throw)
{
    const json j = json::parse(R"({"s":"x","n":5,"b":true,"z":null})");
    LOGOS_ASSERT_EQ(stringAt(j, "s"), std::string("x"));
    LOGOS_ASSERT_EQ(stringAt(j, "n"), std::string());
    LOGOS_ASSERT_EQ(stringAt(j, "z"), std::string());
    LOGOS_ASSERT_EQ(stringAt(json::array(), "s"), std::string());
    LOGOS_ASSERT_TRUE(boolAt(j, "b").value_or(false));
    LOGOS_ASSERT_FALSE(boolAt(j, "s").has_value());
    LOGOS_ASSERT_TRUE(findKey(j, "missing") == nullptr);
}

LOGOS_TEST(package_names_are_plain)
{
    LOGOS_ASSERT_TRUE(isValidPackageName("wallet_ui"));
    LOGOS_ASSERT_TRUE(isValidPackageName("logos-blockchain.module2"));
    LOGOS_ASSERT_FALSE(isValidPackageName(""));
    LOGOS_ASSERT_FALSE(isValidPackageName(".hidden"));
    LOGOS_ASSERT_FALSE(isValidPackageName("wallet ui"));
    LOGOS_ASSERT_FALSE(isValidPackageName("wallet\x1b[2J"));
    LOGOS_ASSERT_FALSE(isValidPackageName("caf\xc3\xa9"));
    LOGOS_ASSERT_FALSE(isValidPackageName(std::string(65, 'a')));
}

LOGOS_TEST(log_fields_stay_on_one_line)
{
    const std::string forged = "x\n[intents_module] presenter claimed (headless)";
    const std::string field = forLog(forged);
    LOGOS_ASSERT_TRUE(field.find('\n') == std::string::npos);
    LOGOS_ASSERT_TRUE(field.find("\\x0A") != std::string::npos);
    LOGOS_ASSERT_TRUE(forLog("a\xe2\x80\xa8" "b").find("\\u2028") != std::string::npos);
    LOGOS_ASSERT_LE(forLog(std::string(1000, 'a'), 50).size(), std::size_t(53));
}

LOGOS_TEST(display_text_loses_control_characters)
{
    // An escape sequence that would repaint a terminal, a bidi override that
    // reorders what a person reads, and a bare C1 control.
    LOGOS_ASSERT_EQ(displaySafe("wallet\x1b[2J\x1b[Hok"), std::string("wallet[2J[Hok"));
    LOGOS_ASSERT_EQ(displaySafe("pay \xe2\x80\xae" "evil"), std::string("pay evil"));
    LOGOS_ASSERT_EQ(displaySafe("a\xc2\x9b" "b\r\nc"), std::string("abc"));
    LOGOS_ASSERT_EQ(displaySafe("caf\xc3\xa9"), std::string("caf\xc3\xa9"));
    LOGOS_ASSERT_EQ(displaySafe("bad\xff"), std::string("bad\xef\xbf\xbd"));
    LOGOS_ASSERT_EQ(displaySafe("\xc0\x9b"), std::string("\xef\xbf\xbd\xef\xbf\xbd"));   // overlong ESC-ish
    // Line and paragraph separators break lines in QML; zero-width characters hide.
    LOGOS_ASSERT_EQ(displaySafe("a\xe2\x80\xa8" "b\xe2\x80\xa9" "c"), std::string("abc"));
    LOGOS_ASSERT_EQ(displaySafe("Wal\xe2\x80\x8blet\xef\xbb\xbf"), std::string("Wallet"));
}

LOGOS_TEST(an_envelope_carries_data_or_an_error_never_both)
{
    const json ok = makeEnvelope(true, json{{"x", 1}}, "ignored");
    LOGOS_ASSERT_EQ(ok["error"].get<std::string>(), std::string());
    LOGOS_ASSERT_EQ(ok["data"]["x"].get<int>(), 1);
    const json bad = makeEnvelope(false, json{{"x", 1}}, err::failed);
    LOGOS_ASSERT_TRUE(bad["data"].is_null());
    LOGOS_ASSERT_EQ(bad["error"].get<std::string>(), err::failed);
}

// Parsed text gives large positive numbers as unsigned, not the signed type a literal has.
LOGOS_TEST(a_parsed_unsafe_integer_is_refused)
{
    LOGOS_ASSERT_TRUE(isCanonicalValue(json::parse("9007199254740991")));
    LOGOS_ASSERT_FALSE(isCanonicalValue(json::parse("9007199254740992")));
    LOGOS_ASSERT_FALSE(isCanonicalValue(json::parse("18446744073709551615")));
}

LOGOS_TEST(text_outside_the_basic_plane_survives_display_and_logs)
{
    const std::string emoji = "ok \xF0\x9F\x91\x8D";
    LOGOS_ASSERT_EQ(displaySafe(emoji), emoji);
    LOGOS_ASSERT_EQ(forLog(emoji), emoji);
}

LOGOS_TEST(invalid_utf8_is_escaped_in_logs)
{
    LOGOS_ASSERT_EQ(forLog(std::string("a\xFF" "b")), std::string("a\\xFFb"));
    LOGOS_ASSERT_EQ(forLog(std::string("\xC0\xAF")), std::string("\\xC0\\xAF"));   // overlong '/'
}
