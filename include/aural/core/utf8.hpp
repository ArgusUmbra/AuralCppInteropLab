#ifndef AURAL_CORE_UTF8_HPP
#define AURAL_CORE_UTF8_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aural::core {

// Experimental: source compatibility and ABI stability are not yet promised.
enum class Utf8ErrorCode : std::uint8_t {
    unexpected_continuation,
    invalid_leading_byte,
    truncated_sequence,
    invalid_continuation_byte,
    overlong_encoding,
    surrogate_code_point,
    code_point_out_of_range,
    noncharacter,
};

struct Utf8Error final {
    Utf8ErrorCode code;

    // For byte-structure failures, the offending byte offset. For truncation,
    // the end offset where another byte was required. For scalar-policy
    // failures, the offset of the scalar's leading byte.
    std::size_t offset;

    // Populated for sequences whose encoded and available lengths are useful
    // to the caller. Both values are byte counts, including the leading byte.
    std::optional<std::uint8_t> expected_length;
    std::optional<std::uint8_t> observed_length;

    // Populated when a present byte directly caused a structural failure.
    std::optional<std::uint8_t> observed_byte;
};

struct Utf8ValidationResult final {
    std::optional<Utf8Error> error;

    [[nodiscard]] bool valid() const noexcept
    {
        return !error.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return valid();
    }
};

// Validates RFC 3629 UTF-8 plus AURAL's prohibition on all Unicode
// noncharacters. The input is borrowed, is never modified, and need not be
// NUL-terminated. This function performs no allocation and throws no
// exceptions. JSON token and escape rules are deliberately outside its scope.
[[nodiscard]] Utf8ValidationResult validate_utf8(
    std::span<const std::byte> input) noexcept;

} // namespace aural::core

#endif
