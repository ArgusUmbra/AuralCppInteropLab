#include "../src/core/json/canonical_json_document.hpp"
#include "../src/core/json/json_value_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using aural::core::json::CanonicalDocumentErrorCode;
using aural::core::json::CanonicalDocumentLimits;
using aural::core::json::JsonObjectMember;
using aural::core::json::JsonSourceRange;
using aural::core::json::JsonValue;
using aural::core::json::max_canonical_document_bytes;
using aural::core::json::max_json_container_depth;
using aural::core::json::max_manifest_bytes;
using aural::core::json::parse_json_document;
using aural::core::json::serialize_canonical_json_document;

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

std::span<const std::byte> bytes(const std::u8string_view input)
{
    return std::as_bytes(std::span{input.data(), input.size()});
}

JsonValue parse(const std::u8string_view input)
{
    auto result = parse_json_document(bytes(input));
    require(result.valid(), "test JSON did not parse");
    return std::move(result.parsed->value);
}

void expect_canonical(
    const std::u8string_view input,
    const std::u8string_view expected)
{
    const auto value = parse(input);
    const auto first = serialize_canonical_json_document(value);
    const auto second = serialize_canonical_json_document(value);
    require(first.valid() && second.valid(),
            "expected document serialization success");
    require(!first.error.has_value() && !second.error.has_value(),
            "successful serialization exposed an error");
    require(*first.serialized == expected, "canonical document bytes mismatch");
    require(*second.serialized == expected,
            "repeated document serialization was not deterministic");
}

void expect_error(
    const JsonValue& value,
    const CanonicalDocumentErrorCode code,
    const CanonicalDocumentLimits limits = {})
{
    const auto result = serialize_canonical_json_document(value, limits);
    require(!result.valid(), "expected canonical document failure");
    require(!result.serialized.has_value(), "failure exposed partial output");
    require(result.error.has_value(), "failure omitted structured error");
    require(result.error->code == code, "wrong canonical document error code");
    require(!aural::core::json::canonical_document_error_message(code).empty(),
            "canonical document error message was empty");
}

JsonValue manual_value(JsonValue::Storage storage)
{
    return JsonValue{std::move(storage), JsonSourceRange{0U, 0U}};
}

JsonObjectMember manual_member(std::u8string name, JsonValue value)
{
    return JsonObjectMember{std::move(name), JsonSourceRange{0U, 0U},
                            std::move(value), JsonSourceRange{0U, 0U}};
}

void test_primitive_roots_and_empty_containers()
{
    expect_canonical(u8" null ", u8"null");
    expect_canonical(u8"true", u8"true");
    expect_canonical(u8"false", u8"false");
    expect_canonical(u8"0", u8"0");
    expect_canonical(u8"-9007199254740991", u8"-9007199254740991");
    expect_canonical(u8"9007199254740991", u8"9007199254740991");
    expect_canonical(u8"\"root\\nstring\"", u8"\"root\\nstring\"");
    expect_canonical(u8"[ ]", u8"[]");
    expect_canonical(u8"{ }", u8"{}");
}

void test_mixed_nesting_and_order()
{
    expect_canonical(
        u8"{\"z\":null,\"a\":[true,{\"b\":2,\"a\":1}],\"m\":false}",
        u8"{\"a\":[true,{\"a\":1,\"b\":2}],\"m\":false,\"z\":null}");

    // UTF-8 byte order puts U+E000 first, while RFC 8785 UTF-16 order puts
    // supplementary U+10000 first.
    expect_canonical(u8"{\"\ue000\":0,\"\U00010000\":1}",
                     u8"{\"\U00010000\":1,\"\ue000\":0}");

    expect_canonical(u8"{\"a\":\"literal \u00e9\"}",
                     u8"{\"a\":\"literal \u00e9\"}");
    expect_canonical(u8"{\"\\u0061\":\"escaped \\u00e9\"}",
                     u8"{\"a\":\"escaped \u00e9\"}");
    expect_canonical(u8"{\"\\u00e9\":\"\\ud834\\udd1e\"}",
                     u8"{\"\u00e9\":\"\U0001d11e\"}");
    expect_canonical(u8"[\"\\u0000\\b\\t\\n\\f\\r\",\"\\\"\\\\/\"]",
                     u8"[\"\\u0000\\b\\t\\n\\f\\r\",\"\\\"\\\\/\"]");
    expect_canonical(u8"[\"\U0001f600\",\"\U00010000\"]",
                     u8"[\"\U0001f600\",\"\U00010000\"]");
}

void test_published_rfc_vectors()
{
    // RFC 8785 section 3.2.2.2 sample string, within AURAL's profile.
    expect_canonical(
        u8"{\"string\":\"\u20ac$\\u000f\\nA'B\\\"\\\\\\\\\\\"/\","
        u8"\"literals\":[null,true,false]}",
        u8"{\"literals\":[null,true,false],"
        u8"\"string\":\"\u20ac$\\u000f\\nA'B\\\"\\\\\\\\\\\"/\"}");

    // RFC 8785 section 3.2.3 property-order sample. Values are shortened only
    // by selecting the published integer-free member names and string values.
    expect_canonical(
        u8"{\"\u20ac\":\"Euro Sign\",\"\\r\":\"Carriage Return\","
        u8"\"\ufb33\":\"Hebrew Letter Dalet With Dagesh\",\"1\":\"One\","
        u8"\"\U0001f600\":\"Emoji: Grinning Face\","
        u8"\"\u0080\":\"Control\",\"\u00f6\":\"Latin Small Letter O With Diaeresis\"}",
        u8"{\"\\r\":\"Carriage Return\",\"1\":\"One\","
        u8"\"\u0080\":\"Control\",\"\u00f6\":\"Latin Small Letter O With Diaeresis\","
        u8"\"\u20ac\":\"Euro Sign\",\"\U0001f600\":\"Emoji: Grinning Face\","
        u8"\"\ufb33\":\"Hebrew Letter Dalet With Dagesh\"}");
}

void test_determinism_and_immutability()
{
    auto first = parse(u8"{\"z\":0,\"a\":1}");
    auto second = parse(u8"{\"a\":1,\"z\":0}");
    const auto first_result = serialize_canonical_json_document(first);
    const auto second_result = serialize_canonical_json_document(second);
    require(first_result && second_result &&
                *first_result.serialized == *second_result.serialized,
            "equivalent member orders produced different bytes");

    const auto& object = std::get<JsonValue::Object>(first.storage);
    require(object[0].name == u8"z" && object[1].name == u8"a" &&
                std::get<std::int64_t>(object[0].value.storage) == 0 &&
                std::get<std::int64_t>(object[1].value.storage) == 1,
            "serialization mutated original object order or values");

    const auto ordered_array = serialize_canonical_json_document(
        parse(u8"[1,2]"));
    const auto reversed_array = serialize_canonical_json_document(
        parse(u8"[2,1]"));
    require(ordered_array && reversed_array &&
                *ordered_array.serialized != *reversed_array.serialized,
            "array order was not significant");

    const auto precomposed = serialize_canonical_json_document(
        parse(u8"{\"v\":\"Caf\u00e9\"}"));
    const auto decomposed = serialize_canonical_json_document(
        parse(u8"{\"v\":\"Cafe\u0301\"}"));
    require(precomposed && decomposed &&
                *precomposed.serialized != *decomposed.serialized,
            "document serialization normalized Unicode");
}

void test_large_and_deep_documents()
{
    std::u8string large = u8"[";
    constexpr std::size_t element_count = 4096U;
    for (std::size_t index = 0U; index < element_count; ++index) {
        if (index != 0U) {
            large.push_back(u8',');
        }
        large.push_back(static_cast<char8_t>(u8'0' + (index % 10U)));
    }
    large.push_back(u8']');
    expect_canonical(large, large);

    JsonValue::Object large_object;
    constexpr std::size_t member_count = 1024U;
    large_object.reserve(member_count);
    for (std::size_t index = member_count; index > 0U; --index) {
        const auto value = index - 1U;
        std::u8string name = u8"k0000";
        auto digits = value;
        for (std::size_t position = name.size(); position > 1U; --position) {
            name[position - 1U] =
                static_cast<char8_t>(u8'0' + (digits % 10U));
            digits /= 10U;
        }
        large_object.push_back(manual_member(
            std::move(name), manual_value(static_cast<std::int64_t>(value))));
    }
    const auto large_object_value = manual_value(std::move(large_object));
    const auto large_object_first =
        serialize_canonical_json_document(large_object_value);
    const auto large_object_second =
        serialize_canonical_json_document(large_object_value);
    require(large_object_first && large_object_second &&
                *large_object_first.serialized ==
                    *large_object_second.serialized &&
                large_object_first.serialized->starts_with(u8"{\"k0000\":0,") &&
                large_object_first.serialized->ends_with(u8"\"k1023\":1023}"),
            "large object ordering was incorrect or nondeterministic");

    std::u8string deep;
    for (std::size_t depth = 0U; depth < max_json_container_depth; ++depth) {
        deep.append(depth % 2U == 0U ? u8"{\"v\":" : u8"[");
    }
    deep.append(u8"null");
    for (std::size_t depth = max_json_container_depth; depth > 0U; --depth) {
        deep.append((depth - 1U) % 2U == 0U ? u8"}" : u8"]");
    }
    const auto deep_value = parse(deep);
    const auto deep_result = serialize_canonical_json_document(deep_value);
    require(deep_result && *deep_result.serialized == deep,
            "maximum mixed container depth did not serialize");

    std::u8string deep_arrays(max_json_container_depth, u8'[');
    deep_arrays.append(u8"0");
    deep_arrays.append(max_json_container_depth, u8']');
    expect_canonical(deep_arrays, deep_arrays);

    std::u8string deep_objects;
    for (std::size_t depth = 0U; depth < max_json_container_depth; ++depth) {
        deep_objects.append(u8"{\"v\":");
    }
    deep_objects.append(u8"0");
    deep_objects.append(max_json_container_depth, u8'}');
    expect_canonical(deep_objects, deep_objects);

    auto too_deep = manual_value(nullptr);
    for (std::size_t depth = 0U; depth <= max_json_container_depth; ++depth) {
        JsonValue::Array wrapper;
        wrapper.push_back(std::move(too_deep));
        too_deep = manual_value(std::move(wrapper));
    }
    expect_error(too_deep,
                 CanonicalDocumentErrorCode::nesting_limit_exceeded);
}

void test_limits_and_invalid_internal_values()
{
    const auto value = parse(u8"{\"b\":2,\"a\":1}");
    const auto exact = serialize_canonical_json_document(
        value, CanonicalDocumentLimits{32U, max_manifest_bytes,
                                       max_manifest_bytes, 13U});
    require(exact && *exact.serialized == u8"{\"a\":1,\"b\":2}",
            "exact document output budget failed");
    expect_error(value, CanonicalDocumentErrorCode::output_limit_exceeded,
                 CanonicalDocumentLimits{32U, max_manifest_bytes,
                                         max_manifest_bytes, 12U});

    expect_error(value, CanonicalDocumentErrorCode::invalid_limits,
                 CanonicalDocumentLimits{max_json_container_depth + 1U,
                                         max_manifest_bytes,
                                         max_manifest_bytes,
                                         max_canonical_document_bytes});
    expect_error(value, CanonicalDocumentErrorCode::invalid_limits,
                 CanonicalDocumentLimits{32U, 0U, max_manifest_bytes,
                                         max_canonical_document_bytes});
    expect_error(
        value, CanonicalDocumentErrorCode::invalid_limits,
        CanonicalDocumentLimits{32U, max_manifest_bytes, max_manifest_bytes,
                                max_canonical_document_bytes + 1U});

    const std::u8string embedded_nul{u8'x', u8'\0'};
    const auto expanded = manual_value(embedded_nul);
    const auto expanded_exact = serialize_canonical_json_document(
        expanded, CanonicalDocumentLimits{32U, max_manifest_bytes,
                                          max_manifest_bytes, 9U});
    require(expanded_exact && *expanded_exact.serialized == u8"\"x\\u0000\"",
            "expanded document exact output budget failed");
    expect_error(expanded, CanonicalDocumentErrorCode::output_limit_exceeded,
                 CanonicalDocumentLimits{32U, max_manifest_bytes,
                                         max_manifest_bytes, 8U});

    expect_error(parse(u8"[null,null]"),
                 CanonicalDocumentErrorCode::value_limit_exceeded,
                 CanonicalDocumentLimits{32U, 2U, max_manifest_bytes,
                                         max_canonical_document_bytes});
    expect_error(parse(u8"{\"ab\":\"cd\"}"),
                 CanonicalDocumentErrorCode::decoded_string_limit_exceeded,
                 CanonicalDocumentLimits{32U, max_manifest_bytes, 3U,
                                         max_canonical_document_bytes});

    expect_error(manual_value(INT64_C(9007199254740992)),
                 CanonicalDocumentErrorCode::integer_out_of_range);
    std::u8string malformed{static_cast<char8_t>(0x80)};
    expect_error(manual_value(std::move(malformed)),
                 CanonicalDocumentErrorCode::invalid_string);

    JsonValue::Object duplicate;
    duplicate.push_back(manual_member(u8"same", manual_value(nullptr)));
    duplicate.push_back(manual_member(u8"same", manual_value(true)));
    expect_error(manual_value(std::move(duplicate)),
                 CanonicalDocumentErrorCode::duplicate_property_name);

    JsonValue::Object bad_name;
    std::u8string invalid_name{static_cast<char8_t>(0x80)};
    bad_name.push_back(
        manual_member(std::move(invalid_name), manual_value(nullptr)));
    expect_error(manual_value(std::move(bad_name)),
                 CanonicalDocumentErrorCode::invalid_property_name);

    require(!parse_json_document(bytes(u8"{\"a\":1,}")),
            "malformed document was repaired by the parser boundary");
    require(!parse_json_document(bytes(u8"{\"a\":1,\"a\":2}")),
            "duplicate document was repaired by the parser boundary");
}

void test_frozen_golden_manifest(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not open frozen golden manifest");
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const auto source_u8 = std::u8string_view{
        reinterpret_cast<const char8_t*>(source.data()), source.size()};
    const auto value = parse(source_u8);
    const auto result = serialize_canonical_json_document(value);
    require(result.valid(), "frozen golden manifest did not canonicalize");

    constexpr auto expected =
        u8"{\"assets\":[{\"media_type\":\"audio/wav\","
        u8"\"path\":\"audio/original.wav\",\"role\":\"original\","
        u8"\"sha256\":\"125f39eda3fea4265c129447c18ea5a89ed7302255c7a33ef104393c1f716873\","
        u8"\"size\":52}],\"extensions\":{\"https://example.org/optional\":{"
        u8"\"\U0001d11e\":\"supplementary\",\"\ue000\":\"BMP\"}},"
        u8"\"format\":\"org.aural.audio-evidence\","
        u8"\"original\":\"audio/original.wav\","
        u8"\"recorded_at\":\"2026-10-13T18:00:00Z\","
        u8"\"title\":\"Caf\u00e9 \U0001d11e\",\"version\":\"0.2-draft\"}";
    require(*result.serialized == expected,
            "frozen golden manifest canonical bytes mismatch");
}

} // namespace

int main(const int argc, const char* const argv[])
{
    require(argc == 2, "expected frozen golden manifest path argument");
    test_primitive_roots_and_empty_containers();
    test_mixed_nesting_and_order();
    test_published_rfc_vectors();
    test_determinism_and_immutability();
    test_large_and_deep_documents();
    test_limits_and_invalid_internal_values();
    test_frozen_golden_manifest(argv[1]);
    std::cout << "CORE_JSON_DOCUMENT_CANONICALIZATION_TESTS_OK\n";
}
