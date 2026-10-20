#include <aural/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using aural::core::Utf8ErrorCode;

int failures = 0;

void expect(const bool condition, const char* const message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] std::vector<std::byte> bytes(
    const std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] std::span<const std::byte> ascii_bytes(
    const std::string_view value) noexcept
{
    return std::as_bytes(std::span{value.data(), value.size()});
}

void expect_valid(
    const std::span<const std::byte> input,
    const char* const message)
{
    const auto result = aural::core::validate_utf8(input);
    expect(result.valid() && static_cast<bool>(result) && !result.error, message);
}

void expect_error(
    const std::span<const std::byte> input,
    const Utf8ErrorCode code,
    const std::size_t offset,
    const char* const message)
{
    const auto result = aural::core::validate_utf8(input);
    expect(!result.valid() && !static_cast<bool>(result) && result.error.has_value(),
           message);
    if (result.error) {
        expect(result.error->code == code, message);
        expect(result.error->offset == offset, message);
    }
}

[[nodiscard]] std::array<std::byte, 4> encode_scalar(
    const std::uint32_t scalar,
    std::size_t& width) noexcept
{
    std::array<std::byte, 4> output{};
    if (scalar <= UINT32_C(0x7F)) {
        width = 1;
        output[0] = static_cast<std::byte>(scalar);
    } else if (scalar <= UINT32_C(0x7FF)) {
        width = 2;
        output[0] = static_cast<std::byte>(UINT32_C(0xC0) | (scalar >> 6U));
        output[1] = static_cast<std::byte>(
            UINT32_C(0x80) | (scalar & UINT32_C(0x3F)));
    } else if (scalar <= UINT32_C(0xFFFF)) {
        width = 3;
        output[0] = static_cast<std::byte>(UINT32_C(0xE0) | (scalar >> 12U));
        output[1] = static_cast<std::byte>(
            UINT32_C(0x80) | ((scalar >> 6U) & UINT32_C(0x3F)));
        output[2] = static_cast<std::byte>(
            UINT32_C(0x80) | (scalar & UINT32_C(0x3F)));
    } else {
        width = 4;
        output[0] = static_cast<std::byte>(UINT32_C(0xF0) | (scalar >> 18U));
        output[1] = static_cast<std::byte>(
            UINT32_C(0x80) | ((scalar >> 12U) & UINT32_C(0x3F)));
        output[2] = static_cast<std::byte>(
            UINT32_C(0x80) | ((scalar >> 6U) & UINT32_C(0x3F)));
        output[3] = static_cast<std::byte>(
            UINT32_C(0x80) | (scalar & UINT32_C(0x3F)));
    }
    return output;
}

void test_valid_inputs()
{
    expect_valid({}, "empty input");
    expect_valid(ascii_bytes("AURAL metadata 123"), "ASCII");
    expect_valid(bytes({0xC2, 0xA2}), "valid two-byte scalar");
    expect_valid(bytes({0xE2, 0x82, 0xAC}), "valid three-byte scalar");
    expect_valid(bytes({0xF0, 0x9F, 0x98, 0x80}), "valid four-byte scalar");
    expect_valid(bytes({
                     0x54, 0xC3, 0xAD, 0x74, 0x75, 0x6C, 0x6F, 0x3A,
                     0x20, 0xE6, 0x9D, 0xB1, 0xE4, 0xBA, 0xAC, 0x20,
                     0xF0, 0x9F, 0x8E, 0x99, 0xEF, 0xB8, 0x8F,
                 }),
                 "mixed real metadata");
    expect_valid(bytes({0xF0, 0x9D, 0x84, 0x9E}),
                 "supplementary-plane musical symbol");
    expect_valid(bytes({0xF4, 0x8F, 0xBF, 0xBD}),
                 "U+10FFFD last allowed scalar under AURAL policy");
    expect_valid(bytes({0xEF, 0xBF, 0xBD}), "encoded U+FFFD");
    expect_valid(bytes({0x41, 0x00, 0x42}), "embedded zero at decoder layer");
    expect_valid(bytes({0x1F}), "raw control is valid at decoder layer");
    expect_valid(bytes({0xEF, 0xBB, 0xBF}),
                 "U+FEFF is valid here; leading JSON BOM policy is deferred");
    expect_valid(ascii_bytes("\\uD800"),
                 "escaped-surrogate spelling is UTF-8; JSON policy is deferred");
    expect_valid(bytes({0xC3, 0xA9}), "precomposed text preserved");
    expect_valid(bytes({0x65, 0xCC, 0x81}), "decomposed text preserved");
}

void test_invalid_structure()
{
    expect_error(bytes({0x80}), Utf8ErrorCode::unexpected_continuation, 0,
                 "isolated continuation");
    expect_error(bytes({0x41, 0xBF}), Utf8ErrorCode::unexpected_continuation, 1,
                 "continuation after valid text");

    expect_error(bytes({0xC0, 0x80}), Utf8ErrorCode::overlong_encoding, 0,
                 "overlong two-byte sequence");
    expect_error(bytes({0xE0, 0x80, 0x80}), Utf8ErrorCode::overlong_encoding, 0,
                 "overlong three-byte sequence");
    expect_error(bytes({0xF0, 0x80, 0x80, 0x80}),
                 Utf8ErrorCode::overlong_encoding, 0,
                 "overlong four-byte sequence");

    expect_error(bytes({0xC2}), Utf8ErrorCode::truncated_sequence, 1,
                 "two-byte truncation");
    expect_error(bytes({0xE1}), Utf8ErrorCode::truncated_sequence, 1,
                 "three-byte truncation after lead");
    expect_error(bytes({0xE1, 0x80}), Utf8ErrorCode::truncated_sequence, 2,
                 "three-byte truncation after first continuation");
    expect_error(bytes({0xF1}), Utf8ErrorCode::truncated_sequence, 1,
                 "four-byte truncation after lead");
    expect_error(bytes({0xF1, 0x80}), Utf8ErrorCode::truncated_sequence, 2,
                 "four-byte truncation after first continuation");
    expect_error(bytes({0xF1, 0x80, 0x80}), Utf8ErrorCode::truncated_sequence, 3,
                 "four-byte truncation after second continuation");

    expect_error(bytes({0xC2, 0x41}),
                 Utf8ErrorCode::invalid_continuation_byte, 1,
                 "bad second byte");
    expect_error(bytes({0xE1, 0x41}),
                 Utf8ErrorCode::invalid_continuation_byte, 1,
                 "bad available continuation takes precedence over truncation");
    expect_error(bytes({0xE1, 0x80, 0x41}),
                 Utf8ErrorCode::invalid_continuation_byte, 2,
                 "bad third byte");
    expect_error(bytes({0xF1, 0x80, 0x80, 0x41}),
                 Utf8ErrorCode::invalid_continuation_byte, 3,
                 "bad fourth byte");

    expect_error(bytes({0xF5, 0x80, 0x80, 0x80}),
                 Utf8ErrorCode::invalid_leading_byte, 0,
                 "F5 lead rejected");
    expect_error(bytes({0xF8}), Utf8ErrorCode::invalid_leading_byte, 0,
                 "five-byte lead rejected");
    expect_error(bytes({0xFE}), Utf8ErrorCode::invalid_leading_byte, 0,
                 "FE lead rejected");
    expect_error(bytes({0xFF}), Utf8ErrorCode::invalid_leading_byte, 0,
                 "FF lead rejected");
}

void test_invalid_scalars_and_policy()
{
    expect_error(bytes({0xED, 0xA0, 0x80}),
                 Utf8ErrorCode::surrogate_code_point, 0,
                 "high surrogate encoding");
    expect_error(bytes({0xED, 0xBF, 0xBF}),
                 Utf8ErrorCode::surrogate_code_point, 0,
                 "low surrogate encoding");
    expect_error(bytes({0xF4, 0x90, 0x80, 0x80}),
                 Utf8ErrorCode::code_point_out_of_range, 0,
                 "value above U+10FFFF");

    expect_error(bytes({0xEF, 0xB7, 0x90}), Utf8ErrorCode::noncharacter, 0,
                 "U+FDD0 noncharacter");
    expect_error(bytes({0xEF, 0xB7, 0xAF}), Utf8ErrorCode::noncharacter, 0,
                 "U+FDEF noncharacter");
    expect_error(bytes({0xEF, 0xBF, 0xBE}), Utf8ErrorCode::noncharacter, 0,
                 "U+FFFE noncharacter");
    expect_error(bytes({0xF0, 0x9F, 0xBF, 0xBE}),
                 Utf8ErrorCode::noncharacter, 0,
                 "U+1FFFE noncharacter");
    expect_error(bytes({0xF4, 0x8F, 0xBF, 0xBF}),
                 Utf8ErrorCode::noncharacter, 0,
                 "U+10FFFF is valid UTF-8 but forbidden by AURAL");

    for (std::uint32_t scalar = UINT32_C(0xFDD0);
         scalar <= UINT32_C(0xFDEF);
         ++scalar) {
        std::size_t width = 0;
        const auto encoded = encode_scalar(scalar, width);
        expect_error(std::span{encoded}.first(width),
                     Utf8ErrorCode::noncharacter, 0,
                     "all BMP contiguous noncharacters rejected");
    }
    for (std::uint32_t plane = 0; plane <= 16; ++plane) {
        for (std::uint32_t ending = UINT32_C(0xFFFE);
             ending <= UINT32_C(0xFFFF);
             ++ending) {
            const auto scalar = (plane << 16U) | ending;
            std::size_t width = 0;
            const auto encoded = encode_scalar(scalar, width);
            expect_error(std::span{encoded}.first(width),
                         Utf8ErrorCode::noncharacter, 0,
                         "all plane-ending noncharacters rejected");
        }
    }
}

void test_safety_and_invariants()
{
    const auto malformed = bytes({0x41, 0xE2, 0x82, 0xAC, 0x42, 0xC2, 0x20});
    const auto original = malformed;
    expect_error(malformed, Utf8ErrorCode::invalid_continuation_byte, 6,
                 "valid text followed by malformed final byte");
    expect(malformed == original, "validation preserves original bytes");

    const auto first = aural::core::validate_utf8(malformed);
    const auto second = aural::core::validate_utf8(malformed);
    expect(first.error && second.error &&
               first.error->code == second.error->code &&
               first.error->offset == second.error->offset,
           "repeated validation is deterministic");

    const auto multibyte = bytes({0xE2, 0x82, 0xAC});
    expect_error(std::span{multibyte}.first(1),
                 Utf8ErrorCode::truncated_sequence, 1,
                 "one-byte prefix");
    expect_error(std::span{multibyte}.first(2),
                 Utf8ErrorCode::truncated_sequence, 2,
                 "two-byte prefix");
    expect_error(std::span{multibyte}.subspan(1),
                 Utf8ErrorCode::unexpected_continuation, 0,
                 "suffix begins with continuation");
    expect_valid(std::span{multibyte}.subspan(3), "empty suffix");

    const auto truncated = aural::core::validate_utf8(bytes({0xF0, 0x9F}));
    expect(truncated.error && truncated.error->expected_length == 4 &&
               truncated.error->observed_length == 2 &&
               !truncated.error->observed_byte,
           "truncation reports expected and observed lengths");

    const auto bad_continuation =
        aural::core::validate_utf8(bytes({0xE2, 0x28, 0xA1}));
    expect(bad_continuation.error &&
               bad_continuation.error->observed_byte == UINT8_C(0x28),
           "bad continuation reports observed byte");

    constexpr std::size_t manifest_limit = 1'048'576;
    std::vector<std::byte> boundary(manifest_limit, std::byte{'a'});
    expect_valid(boundary, "AURAL manifest-size boundary without decoder allocation");
    boundary.back() = std::byte{0x80};
    expect_error(boundary,
                 Utf8ErrorCode::unexpected_continuation,
                 manifest_limit - 1,
                 "malformed byte at metadata boundary");
}

} // namespace

int main()
{
    test_valid_inputs();
    test_invalid_structure();
    test_invalid_scalars_and_policy();
    test_safety_and_invariants();

    if (failures != 0) {
        return 1;
    }
    std::cout << "CORE_UTF8_TESTS_OK\n";
    return 0;
}
