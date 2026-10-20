#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace {

struct EvpContextDeleter {
    void operator()(EVP_MD_CTX* context) const noexcept {
        EVP_MD_CTX_free(context);
    }
};

bool is_lower_hex(std::string_view value) {
    for (const char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower_hex = character >= 'a' && character <= 'f';
        if (!digit && !lower_hex) {
            return false;
        }
    }
    return true;
}

std::string to_hex(std::span<const unsigned char> bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char value : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(value);
    }
    return output.str();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("cannot open frozen file");
    }

    std::unique_ptr<EVP_MD_CTX, EvpContextDeleter> context{EVP_MD_CTX_new()};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("cannot initialize SHA-256");
    }

    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 &&
            EVP_DigestUpdate(context.get(),
                             buffer.data(),
                             static_cast<std::size_t>(count)) != 1) {
            throw std::runtime_error("cannot update SHA-256");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("error while reading frozen file");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        throw std::runtime_error("cannot finalize SHA-256");
    }
    return to_hex(std::span<const unsigned char>{digest.data(), digest_size});
}

bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    static_assert(__cplusplus >= 202002L);
    if (argc != 2) {
        std::cerr << "usage: frozen_input_verify <frozen-input-directory>\n";
        return 2;
    }

    try {
        const std::filesystem::path root{argv[1]};
        std::ifstream sums{root / "SHA256SUMS.txt", std::ios::binary};
        if (!sums) {
            throw std::runtime_error("cannot open SHA256SUMS.txt");
        }

        std::size_t verified = 0;
        std::string line;
        while (std::getline(sums, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.size() < 67U || line[64] != ' ' || line[65] != ' ' ||
                !is_lower_hex(std::string_view{line}.substr(0, 64))) {
                throw std::runtime_error("invalid checksum line format");
            }

            const std::string expected = line.substr(0, 64);
            const std::filesystem::path relative{line.substr(66)};
            if (!is_safe_relative_path(relative)) {
                throw std::runtime_error("unsafe checksum path");
            }

            const std::filesystem::path file = root / relative;
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(file)) ||
                !std::filesystem::is_regular_file(file)) {
                throw std::runtime_error("frozen path is not a regular file");
            }
            if (sha256_file(file) != expected) {
                throw std::runtime_error("frozen file checksum mismatch");
            }
            ++verified;
        }
        if (!sums.eof() || verified == 0U) {
            throw std::runtime_error("checksum inventory read failed or was empty");
        }

        std::cout << "FROZEN_INPUTS_OK files=" << verified << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frozen input verification failed: " << error.what() << '\n';
        return 3;
    }
}
