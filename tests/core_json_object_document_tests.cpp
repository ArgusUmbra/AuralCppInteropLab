#include "../src/core/json/json_value_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using aural::core::json::JsonObjectMember;
using aural::core::json::JsonValue;
using aural::core::json::JsonValueErrorCode;
using aural::core::json::JsonValueLimits;
using aural::core::json::JsonValueParseResult;
using aural::core::json::max_json_container_depth;
using aural::core::json::max_manifest_bytes;
using aural::core::json::parse_json_document;

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

std::span<const std::byte> bytes(const std::string_view value)
{
    return std::as_bytes(std::span{value.data(), value.size()});
}

std::span<const std::byte> bytes(const std::u8string_view value)
{
    return std::as_bytes(std::span{value.data(), value.size()});
}

const JsonValue& parsed_value(const JsonValueParseResult& result)
{
    require(result.valid(), "expected successful document parse");
    require(!result.error.has_value(), "success exposed an error");
    return result.parsed->value;
}

void expect_error(
    const std::span<const std::byte> input,
    const JsonValueErrorCode code,
    const std::size_t offset,
    const JsonValueLimits limits = {})
{
    const std::vector<std::byte> unchanged(input.begin(), input.end());
    const auto result = parse_json_document(input, limits);
    require(!result.valid(), "expected document parse failure");
    require(!result.parsed.has_value(), "failure exposed a partial document");
    require(result.error.has_value(), "failure omitted structured error");
    if (result.error->code != code) {
        std::cerr << "expected error code " << static_cast<unsigned>(code)
                  << ", got " << static_cast<unsigned>(result.error->code)
                  << " at offset " << result.error->offset << '\n';
        fail("wrong document error category");
    }
    if (result.error->offset != offset) {
        std::cerr << "expected error offset " << offset << ", got "
                  << result.error->offset << " for code "
                  << static_cast<unsigned>(code) << '\n';
        fail("wrong document error offset");
    }
    require(std::equal(input.begin(), input.end(), unchanged.begin(), unchanged.end()),
            "parser changed input bytes");
}

void expect_error(
    const std::string_view input,
    const JsonValueErrorCode code,
    const std::size_t offset,
    const JsonValueLimits limits = {})
{
    expect_error(bytes(input), code, offset, limits);
}

const JsonValue::Object& object_of(const JsonValue& value)
{
    return std::get<JsonValue::Object>(value.storage);
}

void test_object_model_and_mixed_nesting()
{
    const auto empty = parse_json_document(bytes(" \t{ }\r\n"));
    require(object_of(parsed_value(empty)).empty(), "empty object is not empty");
    require(parsed_value(empty).source.begin == 2U &&
                parsed_value(empty).source.end == 5U,
            "empty object source range mismatch");
    require(empty.parsed->source_bytes_consumed == 7U,
            "complete object consumption mismatch");

    const auto result = parse_json_document(
        bytes("{\"z\":0,\"a\":{\"items\":[true,{\"n\":null}]},\"s\":\"x\"}"));
    const auto& root = object_of(parsed_value(result));
    require(root.size() == 3U, "root object member count mismatch");
    require(root[0].name == u8"z" && root[1].name == u8"a" &&
                root[2].name == u8"s",
            "object source order was not preserved");
    require(root[0].name_source.begin == 1U && root[0].name_source.end == 4U,
            "property-name source range mismatch");
    require(root[0].source.begin == 1U && root[0].source.end == 6U,
            "object-member source range mismatch");
    require(std::get<std::int64_t>(root[0].value.storage) == 0,
            "property-value relationship mismatch");

    const auto& nested = object_of(root[1].value);
    const auto& items = std::get<JsonValue::Array>(nested[0].value.storage);
    require(items.size() == 2U && std::get<bool>(items[0].storage),
            "object/array mixed nesting mismatch");
    const auto& innermost = object_of(items[1]);
    require(innermost[0].name == u8"n" &&
                std::holds_alternative<std::nullptr_t>(
                    innermost[0].value.storage),
            "nested object value mismatch");

    const auto array_root = parse_json_document(bytes("[{\"a\":1},{}]"));
    const auto& array = std::get<JsonValue::Array>(parsed_value(array_root).storage);
    require(array.size() == 2U && object_of(array[1]).empty(),
            "array containing objects mismatch");
}

void test_decoded_duplicate_detection()
{
    expect_error("{\"a\":0,\"a\":1}",
                 JsonValueErrorCode::duplicate_property_name, 7U);
    expect_error("{\"a\":0,\"\\u0061\":1}",
                 JsonValueErrorCode::duplicate_property_name, 7U);
    expect_error("{\"\\u0061\":0,\"\\u0061\":1}",
                 JsonValueErrorCode::duplicate_property_name, 12U);
    expect_error("{\"\\/\":0,\"\\u002F\":1}",
                 JsonValueErrorCode::duplicate_property_name, 8U);
    expect_error("{\"\\n\":0,\"\\u000A\":1}",
                 JsonValueErrorCode::duplicate_property_name, 8U);

    std::u8string supplementary = u8"{\"😀\":0,\"\\uD83D\\uDE00\":1}";
    const auto later_supplementary = supplementary.find(u8"\"\\uD83D");
    expect_error(bytes(supplementary),
                 JsonValueErrorCode::duplicate_property_name,
                 later_supplementary);

    require(parse_json_document(bytes("[{\"same\":0},{\"same\":1}]")).valid(),
            "same name in separate objects must be valid");
    require(parse_json_document(bytes("{\"Name\":0,\"name\":1}")).valid(),
            "case-distinct names must remain distinct");
    require(parse_json_document(bytes(u8"{\"é\":0,\"é\":1}")).valid(),
            "normalization-distinct names must remain distinct");

    expect_error("{\"outer\":{\"x\":0,\"\\u0078\":1}}",
                 JsonValueErrorCode::duplicate_property_name, 16U);
}

void test_invalid_names_delimiters_and_nested_values()
{
    expect_error("{:1}", JsonValueErrorCode::expected_property_name, 1U);
    expect_error("{a:1}", JsonValueErrorCode::expected_property_name, 1U);
    expect_error("{\"a\" 1}", JsonValueErrorCode::expected_colon, 5U);
    expect_error("{\"a\":}", JsonValueErrorCode::unexpected_closing_brace, 5U);
    expect_error("{\"a\":1", JsonValueErrorCode::missing_closing_brace, 6U);
    expect_error("{\"a\":1,}", JsonValueErrorCode::trailing_comma, 7U);
    expect_error("{\"a\":1,,\"b\":2}",
                 JsonValueErrorCode::expected_property_name, 7U);
    expect_error("{\"a\":1 \"b\":2}",
                 JsonValueErrorCode::expected_comma_or_closing_brace, 7U);
    expect_error("{\"a\"::1}", JsonValueErrorCode::expected_value, 5U);
    expect_error("{\"a\":1]", JsonValueErrorCode::expected_comma_or_closing_brace,
                 6U);
    expect_error("}", JsonValueErrorCode::unexpected_closing_brace, 0U);
    expect_error("{\"\\q\":1}", JsonValueErrorCode::string_error, 3U);
    expect_error("{\"a\":01}", JsonValueErrorCode::primitive_error, 6U);
    expect_error("{\"a\":1.0}", JsonValueErrorCode::primitive_error, 6U);
    expect_error("{\"a\":[1,]}", JsonValueErrorCode::trailing_comma, 8U);

    const std::vector<std::byte> malformed_name{
        std::byte{'{'}, std::byte{'"'}, std::byte{0xC2}, std::byte{' '},
        std::byte{'"'}, std::byte{':'}, std::byte{'0'}, std::byte{'}'}};
    expect_error(malformed_name, JsonValueErrorCode::string_error, 3U);
}

std::string nested_object(const std::size_t depth)
{
    std::string result;
    result.reserve(depth * 6U + 4U);
    for (std::size_t index = 0; index < depth; ++index) {
        result += "{\"x\":";
    }
    result += "null";
    result.append(depth, '}');
    return result;
}

std::string mixed_containers(const std::size_t depth)
{
    std::string result;
    for (std::size_t index = 0; index < depth; ++index) {
        result += index % 2U == 0U ? "[" : "{\"x\":";
    }
    result += "null";
    for (std::size_t index = depth; index > 0U; --index) {
        result += (index - 1U) % 2U == 0U ? "]" : "}";
    }
    return result;
}

void test_depth_size_and_resource_limits()
{
    require(parse_json_document(bytes(nested_object(max_json_container_depth)))
                .valid(),
            "object depth 32 must be accepted");
    expect_error(nested_object(max_json_container_depth + 1U),
                 JsonValueErrorCode::nesting_limit_exceeded, 160U);
    require(parse_json_document(bytes(mixed_containers(max_json_container_depth)))
                .valid(),
            "mixed container depth 32 must be accepted");

    std::string exact{"null"};
    exact.append(max_manifest_bytes - exact.size(), ' ');
    const auto exact_result = parse_json_document(bytes(exact));
    require(exact_result.valid() &&
                exact_result.parsed->source_bytes_consumed == max_manifest_bytes,
            "exact document-size limit must be accepted");
    exact.push_back(' ');
    expect_error(exact, JsonValueErrorCode::source_limit_exceeded,
                 max_manifest_bytes);

    expect_error("{\"a\":0}", JsonValueErrorCode::value_limit_exceeded, 5U,
                 JsonValueLimits{max_manifest_bytes, max_json_container_depth,
                                 1U, max_manifest_bytes});
    expect_error("{\"a\":\"bc\"}",
                 JsonValueErrorCode::decoded_string_limit_exceeded, 7U,
                 JsonValueLimits{max_manifest_bytes, max_json_container_depth,
                                 max_manifest_bytes, 2U});
    expect_error("{\"a\":0,\"b\":1}",
                 JsonValueErrorCode::value_limit_exceeded, 11U,
                 JsonValueLimits{max_manifest_bytes, max_json_container_depth,
                                 2U, max_manifest_bytes});

    std::string large{"{"};
    constexpr std::size_t member_count = 5'000U;
    for (std::size_t index = 0; index < member_count; ++index) {
        if (index != 0U) {
            large.push_back(',');
        }
        large += "\"k" + std::to_string(index) + "\":0";
    }
    large.push_back('}');
    const auto large_result = parse_json_document(bytes(large));
    const auto& members = object_of(parsed_value(large_result));
    require(members.size() == member_count && members.front().name == u8"k0" &&
                members.back().name == u8"k4999",
            "large object parse/order mismatch");
}

void test_complete_document_contract_and_ownership()
{
    require(parse_json_document(bytes("null")).valid(),
            "primitive document root must be accepted");
    require(parse_json_document(bytes(" [ ] ")).valid(),
            "array document root must be accepted");
    expect_error(" \t\r\n", JsonValueErrorCode::empty_input, 4U);
    expect_error("null true", JsonValueErrorCode::unexpected_extra_token, 5U);
    expect_error("{}[]", JsonValueErrorCode::unexpected_extra_token, 2U);
    expect_error("null\v", JsonValueErrorCode::primitive_error, 4U);

    const std::vector<std::byte> bom{
        std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}, std::byte{'{'},
        std::byte{'}'}};
    expect_error(bom, JsonValueErrorCode::leading_bom, 0U);

    std::string mutable_input{"{\"owned\":\"value\"}"};
    const auto owned = parse_json_document(bytes(mutable_input));
    require(owned.valid(), "ownership input did not parse");
    std::fill(mutable_input.begin(), mutable_input.end(), 'x');
    const auto& member = object_of(parsed_value(owned))[0];
    require(member.name == u8"owned" &&
                std::get<std::u8string>(member.value.storage) == u8"value",
            "parsed document borrowed input storage");
}

void test_determinism_and_error_details()
{
    const auto input = bytes("{\"a\":[{\"b\":2}],\"c\":true}");
    const auto first = parse_json_document(input);
    const auto second = parse_json_document(input);
    require(first.valid() && second.valid(), "determinism document failed");
    const auto& first_object = object_of(first.parsed->value);
    const auto& second_object = object_of(second.parsed->value);
    require(first_object.size() == second_object.size() &&
                first_object[1].name == second_object[1].name &&
                first_object[0].source.begin == second_object[0].source.begin,
            "success tree is not deterministic");

    const auto bad_first = parse_json_document(bytes("{\"\\q\":0}"));
    const auto bad_second = parse_json_document(bytes("{\"\\q\":0}"));
    require(!bad_first && !bad_second &&
                bad_first.error->code == bad_second.error->code &&
                bad_first.error->offset == bad_second.error->offset,
            "failure is not deterministic");
    require(bad_first.error->string_error.has_value() &&
                bad_first.error->string_error->offset == 3U,
            "absolute property-name string detail missing");

    const auto primitive = parse_json_document(bytes("{\"a\":9007199254740992}"));
    require(!primitive && primitive.error->primitive_error.has_value() &&
                primitive.error->primitive_error->offset == 20U,
            "absolute object primitive detail missing");
}

} // namespace

int main()
{
    test_object_model_and_mixed_nesting();
    test_decoded_duplicate_detection();
    test_invalid_names_delimiters_and_nested_values();
    test_depth_size_and_resource_limits();
    test_complete_document_contract_and_ownership();
    test_determinism_and_error_details();
    std::cout << "CORE_JSON_OBJECT_DOCUMENT_TESTS_OK\n";
}
