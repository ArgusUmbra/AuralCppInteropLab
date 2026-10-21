#include "../src/core/json/canonical_json_verifier.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aural::core::json::JsonCanonicalityResource;
using aural::core::json::JsonCanonicalityResult;
using aural::core::json::JsonCanonicalityStatus;
using aural::core::json::JsonValueErrorCode;
using aural::core::json::max_json_container_depth;
using aural::core::json::max_manifest_bytes;
using aural::core::json::verify_canonical_json;

[[noreturn]] void fail(const std::string_view message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(const bool condition, const std::string_view message)
{
    if (!condition) {
        fail(message);
    }
}

std::span<const std::byte> bytes(const std::u8string_view input)
{
    return std::as_bytes(std::span{input.data(), input.size()});
}

std::span<const std::byte> bytes(const std::string_view input)
{
    return std::as_bytes(std::span{input.data(), input.size()});
}

void require_result_shape(const JsonCanonicalityResult& result)
{
    require(!aural::core::json::json_canonicality_status_message(result.status)
                 .empty(),
            "canonicality status has no message");
    switch (result.status) {
    case JsonCanonicalityStatus::canonical:
        require(result.canonical() && !result.mismatch_offset.has_value() &&
                    !result.parse_error.has_value() &&
                    !result.serialization_error.has_value() &&
                    !result.resource.has_value(),
                "canonical result exposed failure detail");
        break;
    case JsonCanonicalityStatus::noncanonical:
        require(!result.canonical() && result.mismatch_offset.has_value() &&
                    !result.parse_error.has_value() &&
                    !result.serialization_error.has_value() &&
                    !result.resource.has_value(),
                "noncanonical result shape mismatch");
        break;
    case JsonCanonicalityStatus::invalid_document:
        require(!result.canonical() && !result.mismatch_offset.has_value() &&
                    (result.parse_error.has_value() !=
                     result.serialization_error.has_value()) &&
                    !result.resource.has_value(),
                "invalid result exposed success or resource detail");
        break;
    case JsonCanonicalityStatus::resource_failure:
        require(!result.canonical() && !result.mismatch_offset.has_value() &&
                    result.resource.has_value(),
                "resource result shape mismatch");
        require(!aural::core::json::json_canonicality_resource_message(
                     *result.resource)
                     .empty(),
                "resource failure has no message");
        break;
    }
}

void require_same_result(
    const JsonCanonicalityResult& first,
    const JsonCanonicalityResult& second)
{
    require(first.status == second.status &&
                first.mismatch_offset == second.mismatch_offset &&
                first.resource == second.resource &&
                first.parse_error.has_value() == second.parse_error.has_value() &&
                first.serialization_error.has_value() ==
                    second.serialization_error.has_value(),
            "canonicality result was not deterministic");
    if (first.parse_error.has_value()) {
        require(first.parse_error->code == second.parse_error->code &&
                    first.parse_error->offset == second.parse_error->offset,
                "parse failure detail was not deterministic");
    }
    if (first.serialization_error.has_value()) {
        require(first.serialization_error->code ==
                    second.serialization_error->code &&
                    first.serialization_error->value_index ==
                        second.serialization_error->value_index,
                "serialization failure detail was not deterministic");
    }
}

JsonCanonicalityResult checked_verify(const std::span<const std::byte> input)
{
    const std::vector<std::byte> original(input.begin(), input.end());
    const auto first = verify_canonical_json(input);
    const auto second = verify_canonical_json(input);
    require_result_shape(first);
    require_result_shape(second);
    require_same_result(first, second);
    require(std::equal(input.begin(), input.end(), original.begin(), original.end()),
            "canonicality verification modified source bytes");
    return first;
}

JsonCanonicalityResult checked_verify(const std::u8string_view input)
{
    return checked_verify(bytes(input));
}

void expect_canonical(const std::u8string_view input)
{
    const auto result = checked_verify(input);
    require(result.status == JsonCanonicalityStatus::canonical,
            "expected canonical document");
}

void expect_noncanonical(
    const std::u8string_view input,
    const std::size_t mismatch_offset)
{
    const auto result = checked_verify(input);
    require(result.status == JsonCanonicalityStatus::noncanonical,
            "expected noncanonical document");
    require(*result.mismatch_offset == mismatch_offset,
            "wrong first mismatch offset");
}

void expect_invalid(
    const std::span<const std::byte> input,
    const JsonValueErrorCode code)
{
    const auto result = checked_verify(input);
    require(result.status == JsonCanonicalityStatus::invalid_document &&
                result.parse_error.has_value() &&
                result.parse_error->code == code,
            "wrong invalid-document categorization");
}

void expect_invalid(
    const std::u8string_view input,
    const JsonValueErrorCode code)
{
    expect_invalid(bytes(input), code);
}

void expect_resource(
    const std::span<const std::byte> input,
    const JsonCanonicalityResource resource)
{
    const auto result = checked_verify(input);
    require(result.status == JsonCanonicalityStatus::resource_failure &&
                result.resource == resource,
            "wrong resource-failure categorization");
}

void test_canonical_documents()
{
    expect_canonical(u8"null");
    expect_canonical(u8"true");
    expect_canonical(u8"false");
    expect_canonical(u8"0");
    expect_canonical(u8"-9007199254740991");
    expect_canonical(u8"9007199254740991");
    expect_canonical(u8"\"text\"");
    expect_canonical(u8"[]");
    expect_canonical(u8"{}");
    expect_canonical(u8"[null,true,false,0,\"x\",[],{}]");
    expect_canonical(u8"{\"a\":[{\"a\":1,\"b\":2}],\"z\":null}");

    // RFC 8785 compares decoded names as unsigned UTF-16 code units, putting
    // U+10000 before U+E000 even though their UTF-8 byte order is opposite.
    expect_canonical(u8"{\"\U00010000\":1,\"\ue000\":0}");
    expect_canonical(u8"[\"\\u0000\\b\\t\\n\\f\\r\",\"\\\"\\\\/\"]");

    std::u8string deepest(max_json_container_depth, u8'[');
    deepest.push_back(u8'0');
    deepest.append(max_json_container_depth, u8']');
    expect_canonical(deepest);

    // Exactly the on-disk byte ceiling and the aggregate decoded-string
    // budget minus the two mandatory quotes.
    std::u8string largest(max_manifest_bytes, u8'a');
    largest.front() = u8'"';
    largest.back() = u8'"';
    expect_canonical(largest);
}

void test_valid_but_noncanonical_documents()
{
    expect_noncanonical(u8" null", 0U);
    expect_noncanonical(u8"null\n", 4U);
    expect_noncanonical(u8"[0, 1]", 3U);
    expect_noncanonical(u8"{\"b\":2,\"a\":1}", 2U);
    expect_noncanonical(u8"\"\\u0061\"", 1U);
    expect_noncanonical(u8"\"\\/\"", 1U);
    expect_noncanonical(u8"\"\\u0008\"", 2U);
    expect_noncanonical(u8"\"\\u000A\"", 2U);
    expect_noncanonical(u8"\"\\u00e9\"", 1U);
    expect_noncanonical(u8"{\"\ue000\":0,\"\U00010000\":1}", 2U);
    expect_noncanonical(u8"\"\\u000a\"", 2U);
}

void test_invalid_documents()
{
    expect_invalid(u8"{\"a\":0,\"\\u0061\":1}",
                   JsonValueErrorCode::duplicate_property_name);

    const std::vector<std::byte> invalid_utf8{
        std::byte{'"'}, std::byte{0xC2}, std::byte{' '}, std::byte{'"'}};
    expect_invalid(invalid_utf8, JsonValueErrorCode::string_error);
    expect_invalid(u8"\"\\ud800\"", JsonValueErrorCode::string_error);

    const std::vector<std::byte> noncharacter{
        std::byte{'"'}, std::byte{0xEF}, std::byte{0xB7}, std::byte{0x90},
        std::byte{'"'}};
    expect_invalid(noncharacter, JsonValueErrorCode::string_error);

    expect_invalid(u8"1.0", JsonValueErrorCode::primitive_error);
    expect_invalid(u8"1e0", JsonValueErrorCode::primitive_error);
    expect_invalid(u8"-0", JsonValueErrorCode::primitive_error);
    expect_invalid(u8"9007199254740992",
                   JsonValueErrorCode::primitive_error);
    expect_invalid(u8"{\"a\":1,}", JsonValueErrorCode::trailing_comma);
    expect_invalid(u8"null true", JsonValueErrorCode::unexpected_extra_token);

    const std::vector<std::byte> bom{
        std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}, std::byte{'n'},
        std::byte{'u'}, std::byte{'l'}, std::byte{'l'}};
    expect_invalid(bom, JsonValueErrorCode::leading_bom);
}

void test_resource_failures()
{
    std::u8string too_deep(max_json_container_depth + 1U, u8'[');
    too_deep.push_back(u8'0');
    too_deep.append(max_json_container_depth + 1U, u8']');
    expect_resource(bytes(too_deep),
                    JsonCanonicalityResource::nesting_limit_exceeded);

    std::vector<std::byte> oversized(max_manifest_bytes + 1U, std::byte{' '});
    expect_resource(oversized,
                    JsonCanonicalityResource::input_limit_exceeded);
}

void test_frozen_golden_manifest(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not open frozen golden manifest");
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};

    // Independently specified from AURAL Draft 0.2 member order and RFC 8785
    // UTF-16 property ordering; this is not produced by the verifier.
    constexpr auto expected =
        u8"{\"assets\":[{\"media_type\":\"audio/wav\","
        u8"\"path\":\"audio/original.wav\",\"role\":\"original\","
        u8"\"sha256\":\"125f39eda3fea4265c129447c18ea5a89ed7302255c7a33ef104393c1f716873\","
        u8"\"size\":52}],\"extensions\":{\"https://example.org/optional\":{"
        u8"\"\U0001d11e\":\"supplementary\",\"\ue000\":\"BMP\"}},"
        u8"\"format\":\"org.aural.audio-evidence\","
        u8"\"original\":\"audio/original.wav\","
        u8"\"recorded_at\":\"2026-10-13T18:00:00Z\","
        u8"\"title\":\"Caf\u00e9 \U0001d11e\",\"version\":\"0.2-draft\"}";
    const auto expected_bytes = bytes(expected);
    const auto source_bytes = bytes(std::string_view{source});
    require(source_bytes.size() == expected_bytes.size(),
            "golden manifest length differs from independent canonical bytes");
    const auto mismatch = std::mismatch(source_bytes.begin(), source_bytes.end(),
                                        expected_bytes.begin());
    if (mismatch.first != source_bytes.end()) {
        std::cerr << "golden manifest first independent mismatch at byte "
                  << std::distance(source_bytes.begin(), mismatch.first) << '\n';
        fail("frozen golden manifest is not independently canonical");
    }
    const auto result = checked_verify(source_bytes);
    require(result.status == JsonCanonicalityStatus::canonical,
            "frozen golden manifest failed canonicality verification");
}

} // namespace

int main(const int argc, const char* const argv[])
{
    require(argc == 2, "expected frozen golden manifest path argument");
    test_canonical_documents();
    test_valid_but_noncanonical_documents();
    test_invalid_documents();
    test_resource_failures();
    test_frozen_golden_manifest(argv[1]);
    std::cout << "CORE_JSON_CANONICALITY_TESTS_OK\n";
}
