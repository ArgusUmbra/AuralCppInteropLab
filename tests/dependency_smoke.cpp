#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>

#include <openssl/evp.h>
#include <zlib.h>

namespace {

struct EvpContextDeleter {
    void operator()(EVP_MD_CTX* context) const noexcept {
        EVP_MD_CTX_free(context);
    }
};

std::string to_hex(std::span<const unsigned char> bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char value : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(value);
    }
    return output.str();
}

}  // namespace

int main() {
    static_assert(__cplusplus >= 202002L);

    constexpr char sha_input[] = "abc";
    constexpr char crc_input[] = "123456789";
    constexpr char expected_sha[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    constexpr std::uint32_t expected_crc = 0xcbf43926U;

    std::unique_ptr<EVP_MD_CTX, EvpContextDeleter> context{EVP_MD_CTX_new()};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), sha_input, sizeof(sha_input) - 1U) != 1) {
        std::cerr << "OpenSSL EVP initialization failed\n";
        return 2;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        std::cerr << "OpenSSL EVP finalization failed\n";
        return 3;
    }

    const std::string sha =
        to_hex(std::span<const unsigned char>{digest.data(), digest_size});
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc,
                reinterpret_cast<const Bytef*>(crc_input),
                static_cast<uInt>(sizeof(crc_input) - 1U));

    std::ostringstream crc_text;
    crc_text << std::hex << std::setfill('0') << std::setw(8)
             << static_cast<std::uint32_t>(crc);

    std::cout << "SHA256(abc)=" << sha << '\n'
              << "CRC32(123456789)=" << crc_text.str() << '\n';

    if (sha != expected_sha || static_cast<std::uint32_t>(crc) != expected_crc) {
        std::cerr << "Dependency known-answer mismatch\n";
        return 4;
    }

    std::cout << "DEPENDENCY_SMOKE_OK\n";
    return 0;
}
