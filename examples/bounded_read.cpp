#include <aural/core/file_reader.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <utility>

int read_header(const std::filesystem::path& path)
{
    auto opened = aural::core::FileReader::open(path);
    if (!opened) {
        std::cerr << opened.error().description() << '\n';
        return 1;
    }

    auto reader = std::move(opened).value();
    std::array<std::byte, 16> header{};
    const auto read = reader.read_at(0, header);
    if (!read) {
        std::cerr << read.error().description() << '\n';
        return 1;
    }
    return 0;
}

int main(const int argument_count, const char* const arguments[])
{
    if (argument_count != 2) {
        std::cerr << "usage: bounded_read_example <file>\n";
        return 2;
    }
    return read_header(std::filesystem::path{arguments[1]});
}
