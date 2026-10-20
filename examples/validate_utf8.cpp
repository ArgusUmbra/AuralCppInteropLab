#include <aural/core/utf8.hpp>

#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

int main()
{
    constexpr std::string_view text = "AURAL metadata";
    const auto input = std::as_bytes(std::span{text.data(), text.size()});
    const auto result = aural::core::validate_utf8(input);

    if (!result) {
        std::cerr << "invalid UTF-8 at byte " << result.error->offset << '\n';
        return 1;
    }

    std::cout << "valid UTF-8\n";
    return 0;
}
