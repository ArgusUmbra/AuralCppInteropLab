#include <aural/core/range.hpp>

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void expect(const bool condition, const char* const message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main()
{
    using aural::core::ByteRange;
    using aural::core::checked_add;
    using aural::core::range_within_file;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t two_to_32 = UINT64_C(1) << 32U;
    constexpr std::uint64_t two_to_63 = UINT64_C(1) << 63U;

    expect(checked_add(0, 0) == 0, "zero addition");
    expect(checked_add(maximum, 0) == maximum, "UINT64_MAX plus zero");
    expect(!checked_add(maximum, 1), "UINT64_MAX addition overflow");
    expect(!checked_add(two_to_63, two_to_63), "2^63 plus 2^63 overflow");
    expect(checked_add(two_to_32, two_to_32) == (UINT64_C(1) << 33U),
           "addition above 2^32");

    const auto invalid_bounds = ByteRange::from_bounds(8, 7);
    expect(!invalid_bounds, "end before begin rejected");

    const auto empty = ByteRange::from_bounds(5, 5);
    const auto full = ByteRange::from_bounds(0, 10);
    const auto overlap = ByteRange::from_bounds(8, 12);
    const auto adjacent = ByteRange::from_bounds(10, 20);
    expect(empty && empty->empty() && empty->size() == 0,
           "empty range is valid");
    expect(full && full->begin() == 0 && full->end() == 10 && full->size() == 10,
           "valid range accessors");
    expect(full && full->contains(0) && full->contains(9) && !full->contains(10),
           "half-open offset containment");
    expect(full && empty && full->contains(*empty), "contained empty range");
    expect(full && overlap && full->overlaps(*overlap), "overlapping ranges");
    expect(full && adjacent && !full->overlaps(*adjacent),
           "adjacent ranges do not overlap");
    expect(empty && full && !empty->overlaps(*full), "empty range does not overlap");

    const auto exact = range_within_file(7, 3, 10);
    expect(exact && exact->end() == 10, "exact end-of-file boundary");
    expect(!range_within_file(7, 4, 10), "one byte past end rejected");
    expect(range_within_file(10, 0, 10).has_value(),
           "zero-length range at end of file");
    expect(!range_within_file(11, 0, 10), "empty range beyond file rejected");
    expect(!range_within_file(maximum, 1, maximum), "offset plus length overflow");
    expect(range_within_file(maximum, 0, maximum).has_value(),
           "empty range at UINT64_MAX file end");
    expect(range_within_file(two_to_32, 1, two_to_32 + 1).has_value(),
           "range around 2^32");
    expect(range_within_file(two_to_63, 1, two_to_63 + 1).has_value(),
           "range around 2^63");
    expect(!range_within_file(two_to_63, two_to_63, maximum),
           "large offset and length overflow");

    if (failures != 0) {
        return 1;
    }
    std::cout << "CORE_RANGE_TESTS_OK\n";
    return 0;
}
