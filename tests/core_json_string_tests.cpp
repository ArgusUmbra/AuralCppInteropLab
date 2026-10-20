#include "../src/core/json/json_string_decoder.hpp"

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

using aural::core::json::JsonStringDecodeResult;
using aural::core::json::JsonStringErrorCode;
using aural::core::json::JsonStringLimits;
using aural::core::json::decode_json_string;

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

std::u8string_view decoded_value(const JsonStringDecodeResult& result)
{
    require(result.valid(), "expected successful decode");
    require(!result.error.has_value(), "success must not contain an error");
    return result.decoded->value;
}

void expect_success(
    const std::string_view input,
    const std::u8string_view expected,
    const std::size_t consumed)
{
    const std::string unchanged(input);
    const auto result = decode_json_string(bytes(input));
    require(decoded_value(result) == expected, "decoded value mismatch");
    require(result.decoded->source_bytes_consumed == consumed,
            "source consumption mismatch");
    require(result.decoded->source.size() == consumed, "source view size mismatch");
    require(result.decoded->source.data() == bytes(input).data(),
            "source view must borrow the original token");
    require(input == unchanged, "decoder changed input bytes");
}

void expect_error(
    const std::span<const std::byte> input,
    const JsonStringErrorCode code,
    const std::size_t offset,
    const JsonStringLimits limits = {})
{
    const std::vector<std::byte> unchanged(input.begin(), input.end());
    const auto result = decode_json_string(input, limits);
    require(!result.valid(), "expected decode failure");
    require(!result.decoded.has_value(), "failure exposed a partial decoded value");
    require(result.error.has_value(), "failure omitted structured error");
    require(result.error->code == code, "wrong error category");
    require(result.error->offset == offset, "wrong error offset");
    require(std::equal(input.begin(), input.end(), unchanged.begin(), unchanged.end()),
            "failure path changed input bytes");
}

void expect_error(
    const std::string_view input,
    const JsonStringErrorCode code,
    const std::size_t offset,
    const JsonStringLimits limits = {})
{
    expect_error(bytes(input), code, offset, limits);
}

void test_valid_strings()
{
    expect_success("\"\"", u8"", 2);
    expect_success("\"plain ASCII\"", u8"plain ASCII", 13);
    expect_success("\"value\"tail", u8"value", 7);

    const auto raw = u8"\"Café 𝄞\"";
    const auto raw_result = decode_json_string(bytes(std::u8string_view{raw}));
    require(decoded_value(raw_result) == u8"Café 𝄞", "raw UTF-8 mismatch");
    require(raw_result.decoded->source_bytes_consumed == std::u8string_view{raw}.size(),
            "raw UTF-8 consumption mismatch");

    expect_success("\"\\\"\\\\\\/\"", u8"\"\\/", 8);
    expect_success("\"\\b\\f\\n\\r\\t\"", u8"\b\f\n\r\t", 12);
    expect_success("\"\\u0000\\u001F\"", std::u8string_view{u8"\0\x1f", 2}, 14);
    expect_success("\"\\u0041\\u00e9\\u20AC\"", u8"Aé€", 20);
    expect_success("\"\\uD834\\uDD1E\"", u8"𝄞", 14);
    expect_success("\"\\ud83d\\ude00\"", u8"😀", 14);

    const auto literal = decode_json_string(bytes(std::u8string_view{u8"\"é𝄞\""}));
    const auto escaped = decode_json_string(bytes("\"\\u00E9\\uD834\\uDD1E\""));
    require(decoded_value(literal) == decoded_value(escaped),
            "literal and escaped Unicode must decode identically");

    const auto precomposed = decode_json_string(bytes(std::u8string_view{u8"\"é\""}));
    const auto decomposed = decode_json_string(bytes(std::u8string_view{u8"\"é\""}));
    require(decoded_value(precomposed) != decoded_value(decomposed),
            "decoder must not normalize Unicode");

    const auto literal_key = decode_json_string(bytes(std::u8string_view{u8"\"café\""}));
    const auto escaped_key = decode_json_string(bytes("\"caf\\u00E9\""));
    require(decoded_value(literal_key) == decoded_value(escaped_key),
            "escaped object-key characters must compare equal after decode");
}

void test_syntax_and_escape_errors()
{
    expect_error("", JsonStringErrorCode::missing_opening_quote, 0);
    expect_error("plain", JsonStringErrorCode::missing_opening_quote, 0);
    expect_error("\"plain", JsonStringErrorCode::missing_closing_quote, 6);
    expect_error("\"\\", JsonStringErrorCode::dangling_escape, 2);
    expect_error("\"\\x\"", JsonStringErrorCode::unsupported_escape, 2);
    expect_error(std::string_view{"\"a\x00" "b\"", 5},
                 JsonStringErrorCode::unescaped_control_character, 2);

    for (const auto input : {"\"\\u", "\"\\u0", "\"\\u00", "\"\\u000"}) {
        expect_error(input, JsonStringErrorCode::incomplete_unicode_escape,
                     std::string_view{input}.size());
    }
    expect_error("\"\\u00x0\"", JsonStringErrorCode::invalid_unicode_hex_digit, 5);
}

void test_surrogate_errors()
{
    expect_error("\"\\uD800\"", JsonStringErrorCode::unpaired_high_surrogate, 7);
    expect_error("\"\\uDBFF", JsonStringErrorCode::unpaired_high_surrogate, 7);
    expect_error("\"\\uDC00\"", JsonStringErrorCode::unpaired_low_surrogate, 1);
    expect_error("\"\\uDFFF\\uD800\"", JsonStringErrorCode::unpaired_low_surrogate, 1);
    expect_error("\"\\uD800A\"", JsonStringErrorCode::unpaired_high_surrogate, 7);
    expect_error("\"\\uD800\\n\"", JsonStringErrorCode::unpaired_high_surrogate, 7);
    expect_error("\"\\uD800\\", JsonStringErrorCode::unpaired_high_surrogate, 7);
    expect_error("\"\\uD800\\u0041\"", JsonStringErrorCode::unpaired_high_surrogate, 7);
    expect_error("\"\\uD800\\u", JsonStringErrorCode::incomplete_unicode_escape, 9);
    expect_error("\"\\uD800\\u0", JsonStringErrorCode::incomplete_unicode_escape, 10);
    expect_error("\"\\uD800\\u00", JsonStringErrorCode::incomplete_unicode_escape, 11);
    expect_error("\"\\uD800\\u000", JsonStringErrorCode::incomplete_unicode_escape, 12);
    expect_error("\"\\uD800\\u00x0\"", JsonStringErrorCode::invalid_unicode_hex_digit, 11);
}

void test_utf8_and_unicode_policy_errors()
{
    const std::array invalid_continuation{
        std::byte{'"'}, std::byte{0xE2}, std::byte{'('}, std::byte{0xA1}, std::byte{'"'}};
    expect_error(invalid_continuation, JsonStringErrorCode::invalid_literal_utf8, 2);
    const auto detailed = decode_json_string(invalid_continuation);
    require(detailed.error->utf8_error.has_value(), "UTF-8 detail missing");
    require(detailed.error->utf8_error->offset == 2, "UTF-8 detail offset mismatch");

    const std::array truncated{std::byte{'"'}, std::byte{0xF0}, std::byte{0x9D}};
    expect_error(truncated, JsonStringErrorCode::invalid_literal_utf8, 3);

    expect_error("\"\\uFDD0\"", JsonStringErrorCode::unicode_noncharacter, 1);
    expect_error("\"\\uFFFF\"", JsonStringErrorCode::unicode_noncharacter, 1);
    expect_error("\"\\uD83F\\uDFFE\"", JsonStringErrorCode::unicode_noncharacter, 1);
    expect_error("\"\\uDBFF\\uDFFF\"", JsonStringErrorCode::unicode_noncharacter, 1);
    expect_error(bytes(std::u8string_view{u8"\"\uFDD0\""}),
                 JsonStringErrorCode::unicode_noncharacter, 1);
    expect_error(bytes(std::u8string_view{u8"\"\U0010FFFF\""}),
                 JsonStringErrorCode::unicode_noncharacter, 1);
}

void test_limits_and_determinism()
{
    expect_error("\"abc\"", JsonStringErrorCode::decoded_limit_exceeded, 3,
                 JsonStringLimits{5, 2});
    expect_error("\"a\"", JsonStringErrorCode::invalid_limits, 0,
                 JsonStringLimits{aural::core::json::max_manifest_bytes + 1U, 1});

    std::vector<std::byte> oversized(aural::core::json::max_manifest_bytes + 1U,
                                    std::byte{'a'});
    oversized.front() = std::byte{'"'};
    expect_error(oversized, JsonStringErrorCode::source_limit_exceeded,
                 aural::core::json::max_manifest_bytes);

    std::vector<std::byte> boundary(aural::core::json::max_manifest_bytes,
                                   std::byte{'a'});
    boundary.front() = std::byte{'"'};
    boundary.back() = std::byte{'"'};
    const auto boundary_result = decode_json_string(boundary);
    require(boundary_result.valid(), "exact source limit must be accepted");
    require(boundary_result.decoded->source_bytes_consumed == boundary.size(),
            "exact-limit consumption mismatch");
    require(boundary_result.decoded->value.size() == boundary.size() - 2U,
            "exact-limit decoded size mismatch");

    const auto input = bytes("\"\\uD834\\uDD1E and text\"");
    const auto first = decode_json_string(input);
    const auto second = decode_json_string(input);
    require(decoded_value(first) == decoded_value(second), "decode is not deterministic");
    require(first.decoded->source_bytes_consumed == second.decoded->source_bytes_consumed,
            "consumption is not deterministic");
}

} // namespace

int main()
{
    test_valid_strings();
    test_syntax_and_escape_errors();
    test_surrogate_errors();
    test_utf8_and_unicode_policy_errors();
    test_limits_and_determinism();
    std::cout << "CORE_JSON_STRING_TESTS_OK\n";
}
