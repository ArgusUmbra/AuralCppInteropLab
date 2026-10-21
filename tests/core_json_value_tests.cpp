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

using aural::core::json::JsonValue;
using aural::core::json::JsonValueErrorCode;
using aural::core::json::JsonValueLimits;
using aural::core::json::JsonValueParseResult;
using aural::core::json::max_json_container_depth;
using aural::core::json::max_manifest_bytes;
using aural::core::json::parse_json_value;

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

const JsonValue& parsed_value(const JsonValueParseResult& result)
{
    require(result.valid(), "expected successful value parse");
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
    const auto result = parse_json_value(input, limits);
    require(!result.valid(), "expected value parse failure");
    require(!result.parsed.has_value(), "failure exposed a partial tree");
    require(result.error.has_value(), "failure omitted structured error");
    require(result.error->code == code, "wrong value error category");
    require(result.error->offset == offset, "wrong value error offset");
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

void test_supported_values()
{
    auto result = parse_json_value(bytes("  \"caf\\u00E9\" \r\n"));
    const auto& string = parsed_value(result);
    require(std::get<std::u8string>(string.storage) == u8"café",
            "decoded string mismatch");
    require(string.source.begin == 2U && string.source.end == 13U,
            "string source range mismatch");
    require(result.parsed->source_bytes_consumed == 16U,
            "complete consumption mismatch");

    result = parse_json_value(bytes("-9007199254740991"));
    require(std::get<std::int64_t>(parsed_value(result).storage) ==
                -9'007'199'254'740'991LL,
            "integer value mismatch");
    require(std::get<bool>(parsed_value(parse_json_value(bytes("true"))).storage),
            "true value mismatch");
    require(!std::get<bool>(parsed_value(parse_json_value(bytes("false"))).storage),
            "false value mismatch");
    require(std::holds_alternative<std::nullptr_t>(
                parsed_value(parse_json_value(bytes("null"))).storage),
            "null value mismatch");

    const auto prefix = parse_json_value(bytes("true false"), {}, false);
    require(prefix.valid() && prefix.parsed->source_bytes_consumed == 4U,
            "prefix parse consumption mismatch");
}

void test_arrays_and_source_ranges()
{
    const auto empty = parse_json_value(bytes(" [ ] "));
    const auto& empty_array = std::get<JsonValue::Array>(parsed_value(empty).storage);
    require(empty_array.empty(), "empty array is not empty");
    require(parsed_value(empty).source.begin == 1U &&
                parsed_value(empty).source.end == 4U,
            "empty array source range mismatch");

    const auto single = parse_json_value(bytes("[42]"));
    const auto& single_array =
        std::get<JsonValue::Array>(parsed_value(single).storage);
    require(single_array.size() == 1U &&
                std::get<std::int64_t>(single_array[0].storage) == 42,
            "single-element array mismatch");

    const auto mixed = parse_json_value(
        bytes("[\"x\", -1, true, false, null, [\"nested\"]]"));
    const auto& values = std::get<JsonValue::Array>(parsed_value(mixed).storage);
    require(values.size() == 6U, "mixed array size mismatch");
    require(std::get<std::u8string>(values[0].storage) == u8"x",
            "mixed string mismatch");
    require(std::get<std::int64_t>(values[1].storage) == -1,
            "mixed integer mismatch");
    require(std::get<bool>(values[2].storage) &&
                !std::get<bool>(values[3].storage),
            "mixed booleans mismatch");
    require(std::holds_alternative<std::nullptr_t>(values[4].storage),
            "mixed null mismatch");
    const auto& nested = std::get<JsonValue::Array>(values[5].storage);
    require(nested.size() == 1U &&
                std::get<std::u8string>(nested[0].storage) == u8"nested",
            "nested array mismatch");
    require(values[5].source.begin == 29U && values[5].source.end == 39U,
            "nested source range mismatch");
}

std::string nested_array(const std::size_t depth)
{
    return std::string(depth, '[') + "null" + std::string(depth, ']');
}

void test_depth_and_resource_limits()
{
    const auto depth32 = nested_array(max_json_container_depth);
    const auto valid = parse_json_value(bytes(depth32));
    require(valid.valid(), "container depth 32 must be accepted");

    const auto depth33 = nested_array(max_json_container_depth + 1U);
    expect_error(depth33, JsonValueErrorCode::nesting_limit_exceeded, 32U);

    expect_error("[]", JsonValueErrorCode::invalid_limits, 0,
                 JsonValueLimits{max_manifest_bytes, 33U, 1U, 1U});
    const JsonValueLimits no_containers{max_manifest_bytes, 0U,
                                        max_manifest_bytes, max_manifest_bytes};
    require(parse_json_value(bytes("null"), no_containers).valid(),
            "depth zero must still permit a primitive root");
    expect_error("[]", JsonValueErrorCode::nesting_limit_exceeded, 0U,
                 no_containers);
    expect_error("[0,0,0]", JsonValueErrorCode::value_limit_exceeded, 5U,
                 JsonValueLimits{max_manifest_bytes, 32U, 3U,
                                 max_manifest_bytes});
    expect_error("[\"ab\",\"cd\"]",
                 JsonValueErrorCode::decoded_string_limit_exceeded, 8U,
                 JsonValueLimits{max_manifest_bytes, 32U, max_manifest_bytes, 3U});

    std::vector<std::byte> oversized(max_manifest_bytes + 1U, std::byte{' '});
    expect_error(oversized, JsonValueErrorCode::source_limit_exceeded,
                 max_manifest_bytes);

    std::string many{"["};
    constexpr std::size_t element_count = 10'000U;
    many.reserve(element_count * 2U + 1U);
    for (std::size_t index = 0; index < element_count; ++index) {
        if (index != 0U) {
            many.push_back(',');
        }
        many.push_back('0');
    }
    many.push_back(']');
    const auto many_result = parse_json_value(bytes(many));
    require(std::get<JsonValue::Array>(parsed_value(many_result).storage).size() ==
                element_count,
            "many-small-elements parse mismatch");
}

void test_malformed_syntax_and_offsets()
{
    expect_error("", JsonValueErrorCode::empty_input, 0U);
    expect_error(" \t\r\n", JsonValueErrorCode::empty_input, 4U);
    expect_error("[", JsonValueErrorCode::missing_closing_bracket, 1U);
    expect_error("[1", JsonValueErrorCode::missing_closing_bracket, 2U);
    expect_error("[1 2]", JsonValueErrorCode::expected_comma_or_closing_bracket,
                 3U);
    expect_error("[1,]", JsonValueErrorCode::trailing_comma, 3U);
    expect_error("[1,,2]", JsonValueErrorCode::expected_value, 3U);
    expect_error("[,1]", JsonValueErrorCode::expected_value, 1U);
    expect_error("]", JsonValueErrorCode::unexpected_closing_bracket, 0U);
    expect_error("{}", JsonValueErrorCode::unsupported_object, 0U);
    expect_error("[{}]", JsonValueErrorCode::unsupported_object, 1U);
    expect_error("true false", JsonValueErrorCode::unexpected_extra_token, 5U);
    expect_error("[]x", JsonValueErrorCode::unexpected_extra_token, 2U);
    expect_error("[\"\\x\"]", JsonValueErrorCode::string_error, 3U);
    expect_error("[9007199254740992]", JsonValueErrorCode::primitive_error, 16U);
    expect_error("[1.0]", JsonValueErrorCode::primitive_error, 2U);
    expect_error("[trueish]", JsonValueErrorCode::primitive_error, 5U);

    const std::vector<std::byte> malformed{
        std::byte{'['}, std::byte{'"'}, std::byte{0xC2}, std::byte{' '},
        std::byte{'"'}, std::byte{']'}};
    expect_error(malformed, JsonValueErrorCode::string_error, 3U);
}

void test_determinism_and_error_details()
{
    const auto input = bytes(" [\"\\u0061\",[true,null],-7] ");
    const auto first = parse_json_value(input);
    const auto second = parse_json_value(input);
    require(first.valid() && second.valid(), "determinism input failed");
    require(first.parsed->source_bytes_consumed ==
                second.parsed->source_bytes_consumed,
            "success consumption is not deterministic");
    const auto& first_array = std::get<JsonValue::Array>(first.parsed->value.storage);
    const auto& second_array = std::get<JsonValue::Array>(second.parsed->value.storage);
    require(first_array.size() == second_array.size() &&
                first_array[0].source.begin == second_array[0].source.begin,
            "success tree is not deterministic");

    const auto bad_first = parse_json_value(bytes("[\"\\q\"]"));
    const auto bad_second = parse_json_value(bytes("[\"\\q\"]"));
    require(!bad_first && !bad_second &&
                bad_first.error->code == bad_second.error->code &&
                bad_first.error->offset == bad_second.error->offset,
            "failure is not deterministic");
    require(bad_first.error->string_error.has_value() &&
                bad_first.error->string_error->offset == 3U,
            "absolute nested string detail missing");

    const auto primitive = parse_json_value(bytes("[01]"));
    require(!primitive && primitive.error->primitive_error.has_value() &&
                primitive.error->primitive_error->offset == 2U,
            "absolute nested primitive detail missing");

    for (std::uint16_t raw = 0;
         raw <= static_cast<std::uint16_t>(JsonValueErrorCode::unexpected_extra_token);
         ++raw) {
        const auto code = static_cast<JsonValueErrorCode>(raw);
        require(!aural::core::json::json_value_error_message(code).empty(),
                "value error message missing");
    }
}

} // namespace

int main()
{
    test_supported_values();
    test_arrays_and_source_ranges();
    test_depth_and_resource_limits();
    test_malformed_syntax_and_offsets();
    test_determinism_and_error_details();
    std::cout << "CORE_JSON_VALUE_TESTS_OK\n";
}
