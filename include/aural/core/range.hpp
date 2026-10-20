#ifndef AURAL_CORE_RANGE_HPP
#define AURAL_CORE_RANGE_HPP

#include <cstdint>
#include <optional>

namespace aural::core {

// Returns no value when the mathematical sum cannot be represented by uint64_t.
[[nodiscard]] std::optional<std::uint64_t> checked_add(
    std::uint64_t left,
    std::uint64_t right) noexcept;

// An immutable, validated half-open byte range [begin, end).
class ByteRange final {
public:
    [[nodiscard]] static std::optional<ByteRange> from_bounds(
        std::uint64_t begin,
        std::uint64_t end) noexcept;

    [[nodiscard]] std::uint64_t begin() const noexcept;
    [[nodiscard]] std::uint64_t end() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] bool contains(std::uint64_t offset) const noexcept;
    [[nodiscard]] bool contains(const ByteRange& other) const noexcept;
    [[nodiscard]] bool overlaps(const ByteRange& other) const noexcept;

private:
    ByteRange(std::uint64_t begin, std::uint64_t end) noexcept;

    const std::uint64_t begin_;
    const std::uint64_t end_;
};

// Validates [offset, offset + length) against file_size, rejecting overflow.
[[nodiscard]] std::optional<ByteRange> range_within_file(
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t file_size) noexcept;

} // namespace aural::core

#endif
