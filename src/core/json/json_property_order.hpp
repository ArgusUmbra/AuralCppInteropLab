#ifndef AURAL_CORE_JSON_JSON_PROPERTY_ORDER_HPP
#define AURAL_CORE_JSON_JSON_PROPERTY_ORDER_HPP

#include "json_value_parser.hpp"

#include <aural/core/utf8.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace aural::core::json {

enum class PropertyNameOrder : std::int8_t {
    less = -1,
    equal = 0,
    greater = 1,
};

enum class PropertyNameSide : std::uint8_t {
    left,
    right,
};

enum class PropertyNameErrorCode : std::uint8_t {
    input_limit_exceeded,
    invalid_utf8,
    unicode_noncharacter,
};

struct PropertyNameError final {
    PropertyNameErrorCode code;
    PropertyNameSide side;
    std::size_t offset;
    std::optional<Utf8Error> utf8_error;
};

struct PropertyNameCompareResult final {
    std::optional<PropertyNameOrder> order;
    std::optional<PropertyNameError> error;

    [[nodiscard]] bool valid() const noexcept { return order.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

// Compares decoded UTF-8 property names lexicographically by their UTF-16
// code-unit sequences, without normalization or locale-dependent collation.
[[nodiscard]] PropertyNameCompareResult compare_utf8_property_names(
    std::u8string_view left,
    std::u8string_view right) noexcept;

enum class CanonicalPropertyOrderErrorCode : std::uint8_t {
    member_limit_exceeded,
    invalid_property_name,
};

struct CanonicalPropertyOrderError final {
    CanonicalPropertyOrderErrorCode code;
    std::size_t member_index;
    std::optional<PropertyNameError> name_error;
};

struct CanonicalPropertyOrderResult final {
    // Indices into the original source-ordered object. The object is not
    // modified and duplicate equal names retain their source order.
    std::optional<std::vector<std::size_t>> indices;
    std::optional<CanonicalPropertyOrderError> error;

    [[nodiscard]] bool valid() const noexcept { return indices.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

[[nodiscard]] CanonicalPropertyOrderResult canonical_property_order(
    const JsonValue::Object& object);

} // namespace aural::core::json

#endif
