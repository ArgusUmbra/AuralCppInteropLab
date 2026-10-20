#include <aural/core/range.hpp>

#include <limits>

namespace aural::core {

std::optional<std::uint64_t> checked_add(
    const std::uint64_t left,
    const std::uint64_t right) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::nullopt;
    }
    return left + right;
}

ByteRange::ByteRange(const std::uint64_t begin, const std::uint64_t end) noexcept
    : begin_{begin}, end_{end}
{
}

std::optional<ByteRange> ByteRange::from_bounds(
    const std::uint64_t begin,
    const std::uint64_t end) noexcept
{
    if (end < begin) {
        return std::nullopt;
    }
    return ByteRange{begin, end};
}

std::uint64_t ByteRange::begin() const noexcept
{
    return begin_;
}

std::uint64_t ByteRange::end() const noexcept
{
    return end_;
}

std::uint64_t ByteRange::size() const noexcept
{
    return end_ - begin_;
}

bool ByteRange::empty() const noexcept
{
    return begin_ == end_;
}

bool ByteRange::contains(const std::uint64_t offset) const noexcept
{
    return begin_ <= offset && offset < end_;
}

bool ByteRange::contains(const ByteRange& other) const noexcept
{
    return begin_ <= other.begin_ && other.end_ <= end_;
}

bool ByteRange::overlaps(const ByteRange& other) const noexcept
{
    return !empty() && !other.empty() && begin_ < other.end_ && other.begin_ < end_;
}

std::optional<ByteRange> range_within_file(
    const std::uint64_t offset,
    const std::uint64_t length,
    const std::uint64_t file_size) noexcept
{
    const auto end = checked_add(offset, length);
    if (!end || *end > file_size) {
        return std::nullopt;
    }
    return ByteRange::from_bounds(offset, *end);
}

} // namespace aural::core
