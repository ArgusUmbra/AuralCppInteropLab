#ifndef AURAL_CORE_UTF8_INTERNAL_HPP
#define AURAL_CORE_UTF8_INTERNAL_HPP

#include <aural/core/utf8.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aural::core::detail {

struct DecodedScalar final {
    char32_t value;
    std::uint8_t width;
};

struct ScalarDecodeResult final {
    std::optional<DecodedScalar> scalar;
    std::optional<Utf8Error> error;
};

// Decodes exactly one scalar beginning at offset. An offset at or beyond the
// end is reported as truncation without accessing storage. This primitive
// applies RFC 3629 structure and scalar rules, but not AURAL's separate
// noncharacter policy.
[[nodiscard]] ScalarDecodeResult decode_utf8_scalar(
    std::span<const std::byte> input,
    std::size_t offset) noexcept;

[[nodiscard]] bool is_unicode_noncharacter(char32_t value) noexcept;

} // namespace aural::core::detail

#endif
