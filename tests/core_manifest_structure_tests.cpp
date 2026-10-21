#include "../src/core/manifest/manifest_validator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using aural::core::json::JsonValue;
using aural::core::manifest::JsonFieldType;
using aural::core::manifest::ManifestConstraint;
using aural::core::manifest::ManifestDocumentStatus;
using aural::core::manifest::ManifestErrorCode;
using aural::core::manifest::ManifestStructureResult;
using aural::core::manifest::ManifestStructureStatus;
using aural::core::manifest::validate_canonical_manifest_structure;
using aural::core::manifest::validate_manifest_structure;

constexpr auto minimal =
    u8"{\"assets\":[{\"media_type\":\"audio/wav\","
    u8"\"path\":\"audio/original.wav\",\"role\":\"original\","
    u8"\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
    u8"\"size\":0}],\"format\":\"org.aural.audio-evidence\","
    u8"\"original\":\"audio/original.wav\",\"version\":\"0.2-draft\"}";

[[noreturn]] void fail(const std::string_view message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void require(const bool condition, const std::string_view message)
{
    if (!condition) fail(message);
}

std::span<const std::byte> bytes(const std::u8string_view input)
{
    return std::as_bytes(std::span{input.data(), input.size()});
}

std::span<const std::byte> bytes(const std::string_view input)
{
    return std::as_bytes(std::span{input.data(), input.size()});
}

JsonValue parse_owned(const std::u8string_view input)
{
    auto parsed = aural::core::json::parse_json_document(bytes(input));
    require(parsed.valid(), "test JSON did not parse");
    return std::move(parsed.parsed->value);
}

std::u8string replace_once(
    std::u8string input,
    const std::u8string_view from,
    const std::u8string_view to)
{
    const auto position = input.find(from);
    require(position != std::u8string::npos, "test replacement source missing");
    input.replace(position, from.size(), to);
    return input;
}

std::u8string manifest_with_assets(const std::u8string_view assets)
{
    std::u8string result = u8"{\"assets\":";
    result.append(assets);
    result.append(
        u8",\"format\":\"org.aural.audio-evidence\","
        u8"\"original\":\"audio/original.wav\",\"version\":\"0.2-draft\"}");
    return result;
}

void require_result_shape(const ManifestStructureResult& result)
{
    switch (result.status) {
    case ManifestStructureStatus::structurally_valid:
        require(result.structurally_valid() && !result.error.has_value() &&
                    !result.resource.has_value(),
                "valid structure result exposed failure detail");
        break;
    case ManifestStructureStatus::invalid_structure:
        require(!result.structurally_valid() && result.error.has_value() &&
                    !result.resource.has_value(),
                "invalid structure result shape mismatch");
        require(!aural::core::manifest::manifest_error_message(
                     result.error->code)
                     .empty(),
                "manifest error has no message");
        break;
    case ManifestStructureStatus::resource_failure:
        require(!result.structurally_valid() && !result.error.has_value() &&
                    result.resource.has_value(),
                "resource result shape mismatch");
        break;
    }
}

ManifestStructureResult checked_structure(const std::u8string_view input)
{
    auto document = parse_owned(input);
    const auto before =
        aural::core::json::serialize_canonical_json_document(document);
    require(before.valid(), "could not snapshot owned test tree");
    const auto first = validate_manifest_structure(document);
    const auto second = validate_manifest_structure(document);
    require_result_shape(first);
    require_result_shape(second);
    require(first.status == second.status &&
                first.resource == second.resource &&
                first.error.has_value() == second.error.has_value(),
            "manifest validation was nondeterministic");
    if (first.error.has_value()) {
        require(first.error->code == second.error->code &&
                    first.error->path == second.error->path &&
                    first.error->constraint == second.error->constraint &&
                    first.error->expected_type == second.error->expected_type &&
                    first.error->actual_type == second.error->actual_type,
                "manifest error detail was nondeterministic");
    }
    const auto after =
        aural::core::json::serialize_canonical_json_document(document);
    require(after.valid() && before.serialized == after.serialized,
            "manifest validation modified the owned input tree");
    return first;
}

void expect_valid(const std::u8string_view input)
{
    require(checked_structure(input).structurally_valid(),
            "expected structurally valid manifest");
}

void expect_error(
    const std::u8string_view input,
    const ManifestErrorCode code,
    const std::u8string_view path,
    const ManifestConstraint constraint = ManifestConstraint::none)
{
    const auto result = checked_structure(input);
    require(result.status == ManifestStructureStatus::invalid_structure &&
                result.error->code == code && result.error->path == path &&
                result.error->constraint == constraint,
            "wrong manifest error categorization or path");
}

void test_minimal_and_semantically_incomplete()
{
    // Zero size and an all-zero digest are locally well formed. Agreement
    // with actual package bytes belongs to the later semantic milestone.
    expect_valid(minimal);
    expect_valid(replace_once(
        std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
        u8"\"critical_extensions\":[],\"extensions\":{},"
        u8"\"title\":\"\",\"version\":\"0.2-draft\""));
}

void test_required_fields_and_types()
{
    expect_error(
        replace_once(std::u8string{minimal},
                     u8"\"format\":\"org.aural.audio-evidence\",", u8""),
        ManifestErrorCode::missing_required_field, u8"/format");
    expect_error(
        replace_once(std::u8string{minimal},
                     u8",\"version\":\"0.2-draft\"", u8""),
        ManifestErrorCode::missing_required_field, u8"/version");
    expect_error(
        replace_once(std::u8string{minimal},
                     u8"\"original\":\"audio/original.wav\",", u8""),
        ManifestErrorCode::missing_required_field, u8"/original");
    expect_error(
        replace_once(std::u8string{minimal},
                     u8"\"assets\":[{\"media_type\"", u8"\"not_assets\":[{\"media_type\""),
        ManifestErrorCode::missing_required_field, u8"/assets");
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"version\":\"0.2-draft\"",
                              u8"\"version\":null"),
                 ManifestErrorCode::wrong_field_type, u8"/version");
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"size\":0", u8"\"size\":\"0\""),
                 ManifestErrorCode::wrong_field_type, u8"/assets/0/size");
    expect_error(manifest_with_assets(u8"[[]]"),
                 ManifestErrorCode::wrong_field_type, u8"/assets/0");
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"media_type\":\"audio/wav\",", u8""),
                 ManifestErrorCode::missing_required_field,
                 u8"/assets/0/media_type");
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"version\":\"0.2-draft\"",
                              u8"\"title\":null,\"version\":\"0.2-draft\""),
                 ManifestErrorCode::wrong_field_type, u8"/title");
    expect_error(u8"[]", ManifestErrorCode::wrong_field_type, u8"");
}

void test_fixed_values_and_asset_constraints()
{
    expect_error(replace_once(std::u8string{minimal},
                              u8"org.aural.audio-evidence", u8"wrong"),
                 ManifestErrorCode::invalid_fixed_value, u8"/format",
                 ManifestConstraint::format_identifier);
    expect_error(replace_once(std::u8string{minimal}, u8"0.2-draft",
                              u8"0.3-draft"),
                 ManifestErrorCode::unsupported_version, u8"/version");
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"original\":\"audio/original.wav\"",
                              u8"\"original\":\"other.wav\""),
                 ManifestErrorCode::invalid_fixed_value, u8"/original",
                 ManifestConstraint::original_path);
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"assets\":[{", u8"\"assets\":[{},{"),
                 ManifestErrorCode::invalid_field_constraint, u8"/assets",
                 ManifestConstraint::asset_count);
    expect_error(replace_once(std::u8string{minimal}, u8"\"role\":\"original\"",
                              u8"\"role\":\"derived\""),
                 ManifestErrorCode::invalid_fixed_value, u8"/assets/0/role",
                 ManifestConstraint::asset_role);
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"media_type\":\"audio/wav\"",
                              u8"\"media_type\":\"audio/x-wav\""),
                 ManifestErrorCode::invalid_fixed_value,
                 u8"/assets/0/media_type",
                 ManifestConstraint::asset_media_type);
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"",
                              u8"\"sha256\":\"ABC\""),
                 ManifestErrorCode::invalid_field_constraint,
                 u8"/assets/0/sha256", ManifestConstraint::sha256_syntax);
    expect_error(replace_once(std::u8string{minimal}, u8"\"size\":0",
                              u8"\"size\":8589934593"),
                 ManifestErrorCode::invalid_field_constraint,
                 u8"/assets/0/size", ManifestConstraint::asset_size_range);
}

void test_unknown_fields_and_safe_paths()
{
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"version\":\"0.2-draft\"",
                              u8"\"bad/name~\":0,\"version\":\"0.2-draft\""),
                 ManifestErrorCode::unknown_field, u8"/bad~1name~0");
    expect_error(replace_once(std::u8string{minimal},
                              u8"\"version\":\"0.2-draft\"",
                              u8"\"bad\\nname\":0,\"version\":\"0.2-draft\""),
                 ManifestErrorCode::unknown_field, u8"/bad%0Aname");
    expect_error(replace_once(std::u8string{minimal}, u8"\"size\":0",
                              u8"\"size\":0,\"surprise\":true"),
                 ManifestErrorCode::unknown_field,
                 u8"/assets/0/surprise");
}

void test_optional_metadata()
{
    expect_valid(replace_once(
        std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
        u8"\"recorded_at\":\"2000-02-29T23:59:59.123456789+14:00\","
        u8"\"version\":\"0.2-draft\""));
    for (const auto timestamp :
         {u8"0000-01-01T00:00:00Z", u8"2026-02-29T00:00:00Z",
          u8"2026-01-01T00:00:60Z", u8"2026-01-01t00:00:00z",
          u8"2026-01-01T00:00:00-00:00",
          u8"2026-01-01T00:00:00+14:01"}) {
        auto input = replace_once(
            std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
            u8"\"recorded_at\":\"X\",\"version\":\"0.2-draft\"");
        input = replace_once(std::move(input), u8"X", timestamp);
        expect_error(input, ManifestErrorCode::invalid_field_constraint,
                     u8"/recorded_at",
                     ManifestConstraint::recorded_at_timestamp);
    }
    std::u8string boundary_title(4096U, u8'a');
    auto input = replace_once(
        std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
        u8"\"title\":\"X\",\"version\":\"0.2-draft\"");
    input = replace_once(std::move(input), u8"X", boundary_title);
    expect_valid(input);
    std::u8string long_title(4097U, u8'a');
    input = replace_once(
        std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
        u8"\"title\":\"X\",\"version\":\"0.2-draft\"");
    input = replace_once(std::move(input), u8"X", long_title);
    expect_error(input, ManifestErrorCode::invalid_field_constraint,
                 u8"/title", ManifestConstraint::title_scalar_limit);
}

void test_extensions()
{
    expect_valid(replace_once(
        std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
        u8"\"extensions\":{\"https://example.org/optional\":{\"any\":[null,true,1]}},"
        u8"\"version\":\"0.2-draft\""));
    expect_error(replace_once(
                     std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
                     u8"\"extensions\":{\"Bad\":0},\"version\":\"0.2-draft\""),
                 ManifestErrorCode::invalid_extension_structure,
                 u8"/extensions/Bad", ManifestConstraint::extension_identifier);
    expect_error(replace_once(
                     std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
                     u8"\"extensions\":[],\"version\":\"0.2-draft\""),
                 ManifestErrorCode::wrong_field_type, u8"/extensions");
    expect_error(replace_once(
                     std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
                     u8"\"critical_extensions\":[0],\"version\":\"0.2-draft\""),
                 ManifestErrorCode::wrong_field_type,
                 u8"/critical_extensions/0");
    expect_error(replace_once(
                     std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
                     u8"\"critical_extensions\":[\"urn:x\"],\"version\":\"0.2-draft\""),
                 ManifestErrorCode::invalid_extension_structure,
                 u8"/critical_extensions/0",
                 ManifestConstraint::critical_extension_missing_value);
    expect_error(replace_once(
                     std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
                     u8"\"critical_extensions\":[\"urn:x\",\"urn:x\"],"
                     u8"\"extensions\":{\"urn:x\":0},\"version\":\"0.2-draft\""),
                 ManifestErrorCode::invalid_extension_structure,
                 u8"/critical_extensions/1",
                 ManifestConstraint::duplicate_critical_extension);
    expect_error(replace_once(
                     std::u8string{minimal}, u8"\"version\":\"0.2-draft\"",
                     u8"\"critical_extensions\":[\"urn:x\"],"
                     u8"\"extensions\":{\"urn:x\":0},\"version\":\"0.2-draft\""),
                 ManifestErrorCode::unsupported_critical_extension,
                 u8"/critical_extensions/0");
}

void test_combined_pipeline()
{
    const auto good = validate_canonical_manifest_structure(bytes(minimal));
    require(good.status == ManifestDocumentStatus::structurally_valid &&
                good.structurally_valid(),
            "canonical structured manifest failed combined pipeline");

    constexpr auto canonical_but_not_manifest = u8"{}";
    const auto missing =
        validate_canonical_manifest_structure(bytes(canonical_but_not_manifest));
    require(missing.status ==
                ManifestDocumentStatus::invalid_manifest_structure &&
                missing.manifest_error->code ==
                    ManifestErrorCode::missing_required_field,
            "canonical non-manifest passed combined pipeline");

    const auto pretty = std::u8string{u8" "} + minimal;
    const auto noncanonical = validate_canonical_manifest_structure(bytes(pretty));
    require(noncanonical.status == ManifestDocumentStatus::noncanonical_json &&
                noncanonical.mismatch_offset == 0U,
            "noncanonical structured manifest passed combined pipeline");

    const auto malformed = validate_canonical_manifest_structure(bytes(u8"{"));
    require(malformed.status == ManifestDocumentStatus::malformed_json &&
                malformed.parse_error.has_value(),
            "malformed JSON had wrong pipeline category");

    std::vector<std::byte> oversized(
        aural::core::json::max_manifest_bytes + 1U, std::byte{' '});
    const auto resource = validate_canonical_manifest_structure(oversized);
    require(resource.status == ManifestDocumentStatus::resource_failure &&
                resource.resource.has_value(),
            "oversized manifest had wrong pipeline category");
}

void test_frozen_golden_manifest(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not open frozen golden manifest");
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const auto snapshot = source;
    const auto result =
        validate_canonical_manifest_structure(bytes(std::string_view{source}));
    require(result.status == ManifestDocumentStatus::structurally_valid,
            "frozen golden manifest failed structure pipeline");
    require(source == snapshot, "combined validation modified source bytes");
}

} // namespace

int main(const int argc, const char* const argv[])
{
    require(argc == 2, "expected frozen golden manifest path argument");
    test_minimal_and_semantically_incomplete();
    test_required_fields_and_types();
    test_fixed_values_and_asset_constraints();
    test_unknown_fields_and_safe_paths();
    test_optional_metadata();
    test_extensions();
    test_combined_pipeline();
    test_frozen_golden_manifest(argv[1]);
    std::cout << "CORE_MANIFEST_STRUCTURE_TESTS_OK\n";
}
