#include "../src/core/json/json_primitive_parser.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aural::core::json::JsonIntegerParseResult;
using aural::core::json::JsonLiteral;
using aural::core::json::JsonLiteralParseResult;
using aural::core::json::JsonPrimitiveErrorCode;
using aural::core::json::JsonPrimitiveLimits;
using aural::core::json::max_aural_json_integer;
using aural::core::json::min_aural_json_integer;
using aural::core::json::parse_json_false;
using aural::core::json::parse_json_integer;
using aural::core::json::parse_json_null;
using aural::core::json::parse_json_true;

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

void expect_integer(
    const std::string_view input,
    const std::int64_t expected,
    const std::size_t consumed)
{
    const std::string unchanged(input);
    const auto input_bytes = bytes(input);
    const auto result = parse_json_integer(input_bytes);
    require(result.valid(), "expected integer success");
    require(!result.error.has_value(), "integer success contains an error");
    require(result.parsed->value == expected, "integer value mismatch");
    require(result.parsed->source_bytes_consumed == consumed,
            "integer consumed-byte mismatch");
    require(result.parsed->source.size() == consumed, "integer source size mismatch");
    require(result.parsed->source.data() == input_bytes.data(),
            "integer token must borrow caller storage");
    require(input == unchanged, "integer parser changed input");
}

void expect_integer_error(
    const std::span<const std::byte> input,
    const JsonPrimitiveErrorCode code,
    const std::size_t offset,
    const JsonPrimitiveLimits limits = {})
{
    const std::vector<std::byte> unchanged(input.begin(), input.end());
    const auto result = parse_json_integer(input, limits);
    require(!result.valid(), "expected integer failure");
    require(!result.parsed.has_value(), "integer failure exposed a parsed value");
    require(result.error.has_value(), "integer failure omitted error detail");
    require(result.error->code == code, "wrong integer error category");
    require(result.error->offset == offset, "wrong integer failure offset");
    require(std::equal(input.begin(), input.end(), unchanged.begin(), unchanged.end()),
            "integer failure changed input");
}

void expect_integer_error(
    const std::string_view input,
    const JsonPrimitiveErrorCode code,
    const std::size_t offset,
    const JsonPrimitiveLimits limits = {})
{
    expect_integer_error(bytes(input), code, offset, limits);
}

using LiteralParser = JsonLiteralParseResult (*)(
    std::span<const std::byte>, JsonPrimitiveLimits) noexcept;

void expect_literal(
    const LiteralParser parser,
    const std::string_view input,
    const JsonLiteral expected,
    const std::size_t consumed)
{
    const std::string unchanged(input);
    const auto input_bytes = bytes(input);
    const auto result = parser(input_bytes, {});
    require(result.valid(), "expected literal success");
    require(!result.error.has_value(), "literal success contains an error");
    require(result.parsed->value == expected, "literal value mismatch");
    require(result.parsed->source_bytes_consumed == consumed,
            "literal consumed-byte mismatch");
    require(result.parsed->source.size() == consumed, "literal source size mismatch");
    require(result.parsed->source.data() == input_bytes.data(),
            "literal token must borrow caller storage");
    require(input == unchanged, "literal parser changed input");
}

void expect_literal_error(
    const LiteralParser parser,
    const std::string_view input,
    const JsonPrimitiveErrorCode code,
    const std::size_t offset,
    const JsonPrimitiveLimits limits = {})
{
    const std::string unchanged(input);
    const auto result = parser(bytes(input), limits);
    require(!result.valid(), "expected literal failure");
    require(!result.parsed.has_value(), "literal failure exposed a parsed value");
    require(result.error.has_value(), "literal failure omitted error detail");
    require(result.error->code == code, "wrong literal error category");
    require(result.error->offset == offset, "wrong literal failure offset");
    require(input == unchanged, "literal failure changed input");
}

void test_valid_integer_boundaries()
{
    expect_integer("0", 0, 1);
    expect_integer("1", 1, 1);
    expect_integer("42", 42, 2);
    expect_integer("-1", -1, 2);
    expect_integer("9007199254740991", max_aural_json_integer, 16);
    expect_integer("-9007199254740991", min_aural_json_integer, 17);

    expect_integer("7,tail", 7, 1);
    expect_integer("8]tail", 8, 1);
    expect_integer("9}tail", 9, 1);
    expect_integer("10 tail", 10, 2);
    expect_integer("11\ttail", 11, 2);
    expect_integer("12\ntail", 12, 2);
    expect_integer("13\rtail", 13, 2);
}

void test_json_syntax_and_aural_profile_split()
{
    // These are JSON numbers under RFC 8259 but are intentionally outside
    // AURAL Draft 0.2's integer-only, no-negative-zero profile.
    expect_integer_error("-0", JsonPrimitiveErrorCode::negative_zero, 0);
    expect_integer_error("-0.5", JsonPrimitiveErrorCode::fraction_not_permitted, 2);
    expect_integer_error("-0e1", JsonPrimitiveErrorCode::exponent_not_permitted, 2);
    expect_integer_error("1.0", JsonPrimitiveErrorCode::fraction_not_permitted, 1);
    expect_integer_error("-2.5", JsonPrimitiveErrorCode::fraction_not_permitted, 2);
    expect_integer_error("1e3", JsonPrimitiveErrorCode::exponent_not_permitted, 1);
    expect_integer_error("1E-3", JsonPrimitiveErrorCode::exponent_not_permitted, 1);

    // These spellings are invalid in ordinary JSON as well.
    expect_integer_error("01", JsonPrimitiveErrorCode::leading_zero, 1);
    expect_integer_error("00", JsonPrimitiveErrorCode::leading_zero, 1);
    expect_integer_error("-01", JsonPrimitiveErrorCode::leading_zero, 2);
    expect_integer_error("+1", JsonPrimitiveErrorCode::leading_plus_sign, 0);
    expect_integer_error("NaN", JsonPrimitiveErrorCode::expected_integer, 0);
    expect_integer_error("Infinity", JsonPrimitiveErrorCode::expected_integer, 0);
}

void test_integer_failures_and_offsets()
{
    expect_integer_error("", JsonPrimitiveErrorCode::empty_input, 0);
    expect_integer_error("-", JsonPrimitiveErrorCode::missing_integer_digit, 1);
    expect_integer_error("-x", JsonPrimitiveErrorCode::missing_integer_digit, 1);
    expect_integer_error("9007199254740992",
                         JsonPrimitiveErrorCode::integer_out_of_range, 15);
    expect_integer_error("-9007199254740992",
                         JsonPrimitiveErrorCode::integer_out_of_range, 16);
    expect_integer_error("1x", JsonPrimitiveErrorCode::invalid_token_boundary, 1);
    expect_integer_error("2:", JsonPrimitiveErrorCode::invalid_token_boundary, 1);
    expect_integer_error("3[", JsonPrimitiveErrorCode::invalid_token_boundary, 1);

    const std::array non_ascii{std::byte{0xC2}, std::byte{0xA2}};
    expect_integer_error(non_ascii, JsonPrimitiveErrorCode::expected_integer, 0);
    const std::array after_digit{std::byte{'1'}, std::byte{0xC2}, std::byte{0xA2}};
    expect_integer_error(after_digit,
                         JsonPrimitiveErrorCode::invalid_token_boundary, 1);

    std::string hostile(200'000, '9');
    expect_integer_error(hostile, JsonPrimitiveErrorCode::integer_out_of_range, 15);

    std::vector<std::byte> oversized(aural::core::json::max_manifest_bytes + 1U,
                                     std::byte{'1'});
    expect_integer_error(oversized,
                         JsonPrimitiveErrorCode::source_limit_exceeded,
                         aural::core::json::max_manifest_bytes);

    expect_integer_error("1", JsonPrimitiveErrorCode::invalid_limits, 0,
                         JsonPrimitiveLimits{
                             aural::core::json::max_manifest_bytes + 1U});
    expect_integer_error("12", JsonPrimitiveErrorCode::source_limit_exceeded, 1,
                         JsonPrimitiveLimits{1});
}

void test_literals_and_boundaries()
{
    expect_literal(parse_json_true, "true", JsonLiteral::true_value, 4);
    expect_literal(parse_json_false, "false", JsonLiteral::false_value, 5);
    expect_literal(parse_json_null, "null", JsonLiteral::null_value, 4);
    expect_literal(parse_json_true, "true,tail", JsonLiteral::true_value, 4);
    expect_literal(parse_json_false, "false]tail", JsonLiteral::false_value, 5);
    expect_literal(parse_json_null, "null}tail", JsonLiteral::null_value, 4);
    expect_literal(parse_json_true, "true tail", JsonLiteral::true_value, 4);

    expect_literal_error(parse_json_true, "", JsonPrimitiveErrorCode::truncated_literal, 0);
    expect_literal_error(parse_json_true, "tru",
                         JsonPrimitiveErrorCode::truncated_literal, 3);
    expect_literal_error(parse_json_false, "fals",
                         JsonPrimitiveErrorCode::truncated_literal, 4);
    expect_literal_error(parse_json_null, "nul",
                         JsonPrimitiveErrorCode::truncated_literal, 3);
    expect_literal_error(parse_json_true, "True",
                         JsonPrimitiveErrorCode::literal_mismatch, 0);
    expect_literal_error(parse_json_false, "FALSE",
                         JsonPrimitiveErrorCode::literal_mismatch, 0);
    expect_literal_error(parse_json_null, "Null",
                         JsonPrimitiveErrorCode::literal_mismatch, 0);
    expect_literal_error(parse_json_true, "trux",
                         JsonPrimitiveErrorCode::literal_mismatch, 3);
    expect_literal_error(parse_json_true, "trueish",
                         JsonPrimitiveErrorCode::invalid_token_boundary, 4);
    expect_literal_error(parse_json_false, "false0",
                         JsonPrimitiveErrorCode::invalid_token_boundary, 5);
    expect_literal_error(parse_json_null, "nullx",
                         JsonPrimitiveErrorCode::invalid_token_boundary, 4);
    expect_literal_error(parse_json_null, "null:",
                         JsonPrimitiveErrorCode::invalid_token_boundary, 4);
}

void test_determinism_and_messages()
{
    const auto input = bytes("-12345}");
    const auto first = parse_json_integer(input);
    const auto second = parse_json_integer(input);
    require(first.valid() && second.valid(), "determinism input rejected");
    require(first.parsed->value == second.parsed->value,
            "integer value is not deterministic");
    require(first.parsed->source_bytes_consumed ==
                second.parsed->source_bytes_consumed,
            "integer consumption is not deterministic");

    for (std::uint8_t raw = 0;
         raw <= static_cast<std::uint8_t>(JsonPrimitiveErrorCode::invalid_token_boundary);
         ++raw) {
        const auto code = static_cast<JsonPrimitiveErrorCode>(raw);
        require(!aural::core::json::json_primitive_error_message(code).empty(),
                "error code lacks an actionable message");
    }
}

} // namespace

int main()
{
    test_valid_integer_boundaries();
    test_json_syntax_and_aural_profile_split();
    test_integer_failures_and_offsets();
    test_literals_and_boundaries();
    test_determinism_and_messages();
    std::cout << "CORE_JSON_PRIMITIVE_TESTS_OK\n";
}
