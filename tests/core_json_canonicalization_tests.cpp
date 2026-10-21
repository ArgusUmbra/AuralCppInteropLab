#include "../src/core/json/canonical_json_string.hpp"
#include "../src/core/json/json_property_order.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using aural::core::Utf8ErrorCode;
using aural::core::json::CanonicalPropertyOrderErrorCode;
using aural::core::json::CanonicalStringErrorCode;
using aural::core::json::CanonicalStringLimits;
using aural::core::json::JsonObjectMember;
using aural::core::json::JsonSourceRange;
using aural::core::json::JsonValue;
using aural::core::json::PropertyNameErrorCode;
using aural::core::json::PropertyNameOrder;
using aural::core::json::PropertyNameSide;
using aural::core::json::canonical_property_order;
using aural::core::json::compare_utf8_property_names;
using aural::core::json::max_canonical_string_bytes;
using aural::core::json::max_manifest_bytes;
using aural::core::json::serialize_canonical_json_string;

[[noreturn]] void fail(const std::string_view message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(const bool condition, const std::string_view message)
{
    if (!condition) {
        fail(message);
    }
}

void expect_serialized(
    const std::u8string_view input,
    const std::u8string_view expected)
{
    const std::u8string unchanged(input);
    const auto first = serialize_canonical_json_string(input);
    const auto second = serialize_canonical_json_string(input);
    require(first.valid() && second.valid(), "expected serialization success");
    require(!first.error.has_value() && !second.error.has_value(),
            "successful serialization exposed an error");
    require(*first.serialized == expected, "canonical string bytes mismatch");
    require(*second.serialized == expected,
            "repeated canonical serialization was not deterministic");
    require(input == unchanged, "serializer changed its input");
}

void expect_string_error(
    const std::u8string_view input,
    const CanonicalStringErrorCode code,
    const std::size_t offset,
    const CanonicalStringLimits limits = {})
{
    const std::u8string unchanged(input);
    const auto result = serialize_canonical_json_string(input, limits);
    require(!result.valid(), "expected serialization failure");
    require(!result.serialized.has_value(), "failure exposed partial output");
    require(result.error.has_value(), "serialization failure omitted error");
    require(result.error->code == code, "wrong serialization error category");
    require(result.error->offset == offset, "wrong serialization error offset");
    require(input == unchanged, "failed serialization changed its input");
}

void test_string_vectors()
{
    expect_serialized(u8"", u8"\"\"");
    expect_serialized(u8"AURAL canonical JSON 123", u8"\"AURAL canonical JSON 123\"");
    expect_serialized(u8"\"\\/", u8"\"\\\"\\\\/\"");

    std::u8string controls;
    for (std::uint8_t value = 0U; value < 0x20U; ++value) {
        controls.push_back(static_cast<char8_t>(value));
    }
    constexpr auto expected_controls =
        u8"\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007"
        u8"\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
        u8"\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017"
        u8"\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f\"";
    expect_serialized(controls, expected_controls);

    expect_serialized(u8"\u00e9 \u03a9 \ue000", u8"\"\u00e9 \u03a9 \ue000\"");
    expect_serialized(u8"\U00010000 \U0001f600",
                      u8"\"\U00010000 \U0001f600\"");
    expect_serialized(u8"\u2028\u2029", u8"\"\u2028\u2029\"");

    const std::u8string embedded_nul{u8'a', u8'\0', u8'b'};
    expect_serialized(embedded_nul, u8"\"a\\u0000b\"");

    const auto precomposed = serialize_canonical_json_string(u8"Caf\u00e9");
    const auto decomposed = serialize_canonical_json_string(u8"Cafe\u0301");
    require(precomposed && decomposed,
            "normalization-distinct strings did not serialize");
    require(*precomposed.serialized == u8"\"Caf\u00e9\"" &&
                *decomposed.serialized == u8"\"Cafe\u0301\"" &&
                *precomposed.serialized != *decomposed.serialized,
            "serializer normalized Unicode");

    // Adapted directly from RFC 8785 section 3.2.2.2's sample string.
    expect_serialized(u8"\u20ac$\u000f\nA'B\"\\\\\"/",
                      u8"\"\u20ac$\\u000f\\nA'B\\\"\\\\\\\\\\\"/\"");
}

void test_string_failures_and_limits()
{
    std::u8string malformed{static_cast<char8_t>(0xC2), u8' '};
    expect_string_error(malformed, CanonicalStringErrorCode::invalid_utf8, 1U);
    const auto malformed_result = serialize_canonical_json_string(malformed);
    require(malformed_result.error->utf8_error.has_value() &&
                malformed_result.error->utf8_error->code ==
                    Utf8ErrorCode::invalid_continuation_byte,
            "UTF-8 detail was not preserved");

    expect_string_error(u8"\ufdd0",
                        CanonicalStringErrorCode::unicode_noncharacter, 0U);
    expect_string_error(u8"\U0010ffff",
                        CanonicalStringErrorCode::unicode_noncharacter, 0U);
    expect_string_error(u8"a", CanonicalStringErrorCode::invalid_limits, 0U,
                        CanonicalStringLimits{max_manifest_bytes + 1U,
                                              max_canonical_string_bytes});
    expect_string_error(u8"a", CanonicalStringErrorCode::invalid_limits, 0U,
                        CanonicalStringLimits{max_manifest_bytes,
                                              max_canonical_string_bytes + 1U});
    expect_string_error(u8"", CanonicalStringErrorCode::output_limit_exceeded,
                        0U, CanonicalStringLimits{0U, 1U});

    const std::u8string one_nul{u8'\0'};
    const auto exact = serialize_canonical_json_string(
        one_nul, CanonicalStringLimits{1U, 8U});
    require(exact && *exact.serialized == u8"\"\\u0000\"",
            "exact expanded-output boundary failed");
    expect_string_error(one_nul,
                        CanonicalStringErrorCode::output_limit_exceeded, 0U,
                        CanonicalStringLimits{1U, 7U});

    std::u8string maximum(max_manifest_bytes, u8'\0');
    const auto maximum_result = serialize_canonical_json_string(maximum);
    require(maximum_result &&
                maximum_result.serialized->size() ==
                    max_canonical_string_bytes,
            "maximum string expansion boundary failed");
    expect_string_error(
        maximum, CanonicalStringErrorCode::output_limit_exceeded,
        max_manifest_bytes - 1U,
        CanonicalStringLimits{max_manifest_bytes,
                              max_canonical_string_bytes - 1U});
    maximum.push_back(u8'x');
    expect_string_error(maximum,
                        CanonicalStringErrorCode::input_limit_exceeded,
                        max_manifest_bytes);
}

PropertyNameOrder compare(
    const std::u8string_view left,
    const std::u8string_view right)
{
    const auto result = compare_utf8_property_names(left, right);
    require(result.valid(), "expected property-name comparison success");
    require(!result.error.has_value(), "successful comparison exposed an error");
    return *result.order;
}

JsonObjectMember member(std::u8string name, const std::int64_t value)
{
    return JsonObjectMember{std::move(name), JsonSourceRange{0U, 0U},
                            JsonValue{value, JsonSourceRange{0U, 0U}},
                            JsonSourceRange{0U, 0U}};
}

void test_utf16_comparator()
{
    require(compare(u8"", u8"") == PropertyNameOrder::equal,
            "empty names should compare equal");
    require(compare(u8"a", u8"aa") == PropertyNameOrder::less,
            "prefix ordering failed");
    require(compare(u8"b", u8"a") == PropertyNameOrder::greater,
            "ASCII ordering failed");
    require(compare(u8"\u00e9", u8"e\u0301") == PropertyNameOrder::greater,
            "normalization-distinct ordering failed");

    constexpr auto bmp = u8"\ue000";
    constexpr auto supplementary = u8"\U00010000";
    require(std::u8string_view(bmp) < std::u8string_view(supplementary),
            "test precondition: UTF-8 byte ordering did not differ");
    require(compare(supplementary, bmp) == PropertyNameOrder::less &&
                compare(bmp, supplementary) == PropertyNameOrder::greater,
            "UTF-16 surrogate-pair ordering failed");
    require(compare(u8"\U0001f600", u8"\uf000") == PropertyNameOrder::less,
            "supplementary/BMP UTF-16 ordering failed");
}

void test_property_order_view_and_immutability()
{
    JsonValue::Object object;
    object.push_back(member(u8"\ue000", 0));
    object.push_back(member(u8"z", 1));
    object.push_back(member(u8"\U00010000", 2));
    object.push_back(member(u8"a", 3));

    std::vector<std::u8string> original_names;
    for (const auto& item : object) {
        original_names.push_back(item.name);
    }

    const auto first = canonical_property_order(object);
    const auto second = canonical_property_order(object);
    require(first && second, "canonical property ordering failed");
    const std::vector<std::size_t> expected{3U, 1U, 2U, 0U};
    require(*first.indices == expected && *second.indices == expected,
            "canonical property index order mismatch");
    require(std::get<std::int64_t>(object[0].value.storage) == 0 &&
                std::get<std::int64_t>(object[3].value.storage) == 3,
            "property ordering changed values");
    for (std::size_t index = 0U; index < object.size(); ++index) {
        require(object[index].name == original_names[index],
                "property ordering changed source order or names");
    }

    JsonValue::Object duplicates;
    duplicates.push_back(member(u8"same", 0));
    duplicates.push_back(member(u8"same", 1));
    const auto stable = canonical_property_order(duplicates);
    require(stable && *stable.indices == std::vector<std::size_t>{0U, 1U},
            "equal names did not retain deterministic source order");
}

void test_property_name_failures()
{
    std::u8string malformed{static_cast<char8_t>(0x80)};
    auto result = compare_utf8_property_names(malformed, malformed);
    require(!result && result.error->side == PropertyNameSide::left &&
                result.error->code == PropertyNameErrorCode::invalid_utf8 &&
                result.error->offset == 0U,
            "left invalid property name was not rejected first");
    result = compare_utf8_property_names(u8"valid", malformed);
    require(!result && result.error->side == PropertyNameSide::right,
            "right invalid property name side mismatch");
    result = compare_utf8_property_names(u8"\ufdd0", u8"valid");
    require(!result &&
                result.error->code ==
                    PropertyNameErrorCode::unicode_noncharacter,
            "noncharacter property name was not rejected");

    JsonValue::Object object;
    object.push_back(member(u8"valid", 0));
    object.push_back(member(malformed, 1));
    const auto ordered = canonical_property_order(object);
    require(!ordered && ordered.error->code ==
                            CanonicalPropertyOrderErrorCode::invalid_property_name &&
                ordered.error->member_index == 1U &&
                ordered.error->name_error.has_value(),
            "invalid object property name did not fail deterministically");

    std::u8string oversized(max_manifest_bytes + 1U, u8'a');
    result = compare_utf8_property_names(oversized, u8"x");
    require(!result && result.error->code ==
                           PropertyNameErrorCode::input_limit_exceeded &&
                result.error->offset == max_manifest_bytes,
            "property-name input ceiling was not enforced");
}

} // namespace

int main()
{
    test_string_vectors();
    test_string_failures_and_limits();
    test_utf16_comparator();
    test_property_order_view_and_immutability();
    test_property_name_failures();
    std::cout << "CORE_JSON_CANONICALIZATION_TESTS_OK\n";
}
