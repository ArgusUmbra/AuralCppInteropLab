#include "json_property_order.hpp"

#include "../utf8_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace aural::core::json {
namespace {

std::span<const std::byte> bytes(const std::u8string_view input) noexcept
{
    return std::as_bytes(std::span{input.data(), input.size()});
}

std::optional<PropertyNameError> validate_name(
    const std::u8string_view name,
    const PropertyNameSide side) noexcept
{
    if (name.size() > max_manifest_bytes) {
        return PropertyNameError{PropertyNameErrorCode::input_limit_exceeded,
                                 side, max_manifest_bytes, std::nullopt};
    }
    const auto input = bytes(name);
    std::size_t offset = 0U;
    while (offset < input.size()) {
        const auto decoded = detail::decode_utf8_scalar(input, offset);
        if (decoded.error.has_value()) {
            return PropertyNameError{PropertyNameErrorCode::invalid_utf8,
                                     side, decoded.error->offset,
                                     decoded.error};
        }
        if (detail::is_unicode_noncharacter(decoded.scalar->value)) {
            return PropertyNameError{PropertyNameErrorCode::unicode_noncharacter,
                                     side, offset, std::nullopt};
        }
        offset += static_cast<std::size_t>(decoded.scalar->width);
    }
    return std::nullopt;
}

class Utf16Units final {
public:
    explicit Utf16Units(const std::u8string_view input) noexcept
        : input_(bytes(input))
    {
    }

    [[nodiscard]] std::optional<std::uint16_t> next() noexcept
    {
        if (pending_low_.has_value()) {
            const auto result = pending_low_;
            pending_low_.reset();
            return result;
        }
        if (offset_ == input_.size()) {
            return std::nullopt;
        }

        const auto decoded = *detail::decode_utf8_scalar(input_, offset_).scalar;
        offset_ += static_cast<std::size_t>(decoded.width);
        const auto scalar = static_cast<std::uint32_t>(decoded.value);
        if (scalar <= UINT32_C(0xFFFF)) {
            return static_cast<std::uint16_t>(scalar);
        }

        const auto supplementary = scalar - UINT32_C(0x10000);
        pending_low_ = static_cast<std::uint16_t>(
            UINT32_C(0xDC00) + (supplementary & UINT32_C(0x3FF)));
        return static_cast<std::uint16_t>(
            UINT32_C(0xD800) + (supplementary >> 10U));
    }

private:
    std::span<const std::byte> input_;
    std::size_t offset_ = 0U;
    std::optional<std::uint16_t> pending_low_;
};

PropertyNameOrder compare_valid_names(
    const std::u8string_view left,
    const std::u8string_view right) noexcept
{
    Utf16Units left_units(left);
    Utf16Units right_units(right);
    while (true) {
        const auto left_unit = left_units.next();
        const auto right_unit = right_units.next();
        if (!left_unit.has_value() || !right_unit.has_value()) {
            if (left_unit.has_value()) {
                return PropertyNameOrder::greater;
            }
            if (right_unit.has_value()) {
                return PropertyNameOrder::less;
            }
            return PropertyNameOrder::equal;
        }
        if (*left_unit < *right_unit) {
            return PropertyNameOrder::less;
        }
        if (*left_unit > *right_unit) {
            return PropertyNameOrder::greater;
        }
    }
}

} // namespace

PropertyNameCompareResult compare_utf8_property_names(
    const std::u8string_view left,
    const std::u8string_view right) noexcept
{
    if (auto error = validate_name(left, PropertyNameSide::left)) {
        return {std::nullopt, std::move(error)};
    }
    if (auto error = validate_name(right, PropertyNameSide::right)) {
        return {std::nullopt, std::move(error)};
    }
    return {compare_valid_names(left, right), std::nullopt};
}

CanonicalPropertyOrderResult canonical_property_order(
    const JsonValue::Object& object)
{
    if (object.size() > max_manifest_bytes) {
        return {std::nullopt,
                CanonicalPropertyOrderError{
                    CanonicalPropertyOrderErrorCode::member_limit_exceeded,
                    max_manifest_bytes, std::nullopt}};
    }
    for (std::size_t index = 0U; index < object.size(); ++index) {
        if (auto error =
                validate_name(object[index].name, PropertyNameSide::left)) {
            return {std::nullopt,
                    CanonicalPropertyOrderError{
                        CanonicalPropertyOrderErrorCode::invalid_property_name,
                        index, std::move(error)}};
        }
    }

    std::vector<std::size_t> indices(object.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0U});
    std::stable_sort(indices.begin(), indices.end(),
                     [&object](const std::size_t left,
                               const std::size_t right) noexcept {
                         return compare_valid_names(object[left].name,
                                                    object[right].name) ==
                                PropertyNameOrder::less;
                     });
    return {std::move(indices), std::nullopt};
}

} // namespace aural::core::json
