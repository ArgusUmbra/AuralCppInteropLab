#include "canonical_json_verifier.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace aural::core::json {
namespace {

JsonCanonicalityResult resource_failure(
    const JsonCanonicalityResource resource,
    std::optional<JsonValueError> parse_error = std::nullopt,
    std::optional<CanonicalDocumentError> serialization_error = std::nullopt)
{
    return {JsonCanonicalityStatus::resource_failure, std::nullopt,
            std::move(parse_error), std::move(serialization_error), resource};
}

std::optional<JsonCanonicalityResource> parser_resource(
    const JsonValueErrorCode code) noexcept
{
    switch (code) {
    case JsonValueErrorCode::source_limit_exceeded:
        return JsonCanonicalityResource::input_limit_exceeded;
    case JsonValueErrorCode::nesting_limit_exceeded:
        return JsonCanonicalityResource::nesting_limit_exceeded;
    case JsonValueErrorCode::value_limit_exceeded:
        return JsonCanonicalityResource::value_limit_exceeded;
    case JsonValueErrorCode::decoded_string_limit_exceeded:
        return JsonCanonicalityResource::decoded_string_limit_exceeded;
    case JsonValueErrorCode::invalid_limits:
        return JsonCanonicalityResource::internal_serialization_failure;
    default: return std::nullopt;
    }
}

std::optional<JsonCanonicalityResource> serializer_resource(
    const CanonicalDocumentErrorCode code) noexcept
{
    switch (code) {
    case CanonicalDocumentErrorCode::nesting_limit_exceeded:
        return JsonCanonicalityResource::nesting_limit_exceeded;
    case CanonicalDocumentErrorCode::value_limit_exceeded:
        return JsonCanonicalityResource::value_limit_exceeded;
    case CanonicalDocumentErrorCode::decoded_string_limit_exceeded:
        return JsonCanonicalityResource::decoded_string_limit_exceeded;
    case CanonicalDocumentErrorCode::output_limit_exceeded:
        return JsonCanonicalityResource::canonical_output_limit_exceeded;
    case CanonicalDocumentErrorCode::invalid_limits:
    case CanonicalDocumentErrorCode::internal_serialization_failure:
        return JsonCanonicalityResource::internal_serialization_failure;
    default: return std::nullopt;
    }
}

} // namespace

JsonCanonicalityResult verify_canonical_json(
    const std::span<const std::byte> input) noexcept
{
    if (input.size() > max_manifest_bytes) {
        return resource_failure(
            JsonCanonicalityResource::input_limit_exceeded,
            JsonValueError{JsonValueErrorCode::source_limit_exceeded,
                           max_manifest_bytes, std::nullopt, std::nullopt});
    }

    try {
        auto parsed = parse_json_document(input);
        if (!parsed) {
            const auto detail = std::move(*parsed.error);
            if (const auto resource = parser_resource(detail.code)) {
                return resource_failure(*resource, detail);
            }
            return {JsonCanonicalityStatus::invalid_document, std::nullopt,
                    detail, std::nullopt, std::nullopt};
        }

        auto serialized =
            serialize_canonical_json_document(parsed.parsed->value);
        if (!serialized) {
            const auto detail = std::move(*serialized.error);
            if (const auto resource = serializer_resource(detail.code)) {
                return resource_failure(*resource, std::nullopt, detail);
            }
            return {JsonCanonicalityStatus::invalid_document, std::nullopt,
                    std::nullopt, detail, std::nullopt};
        }

        const auto& canonical = *serialized.serialized;
        const auto common_size = std::min(input.size(), canonical.size());
        std::size_t offset = 0U;
        while (offset < common_size &&
               input[offset] == static_cast<std::byte>(canonical[offset])) {
            ++offset;
        }
        if (offset != common_size || input.size() != canonical.size()) {
            return {JsonCanonicalityStatus::noncanonical, offset, std::nullopt,
                    std::nullopt, std::nullopt};
        }
        return {JsonCanonicalityStatus::canonical, std::nullopt, std::nullopt,
                std::nullopt, std::nullopt};
    } catch (const std::bad_alloc&) {
        return resource_failure(JsonCanonicalityResource::allocation_failure);
    } catch (const std::length_error&) {
        return resource_failure(JsonCanonicalityResource::allocation_failure);
    }
}

std::string_view json_canonicality_status_message(
    const JsonCanonicalityStatus status) noexcept
{
    switch (status) {
    case JsonCanonicalityStatus::canonical:
        return "JSON bytes are canonical";
    case JsonCanonicalityStatus::invalid_document:
        return "JSON document is syntactically invalid or outside the AURAL profile";
    case JsonCanonicalityStatus::noncanonical:
        return "JSON document is valid but its bytes are not canonical";
    case JsonCanonicalityStatus::resource_failure:
        return "JSON canonicality check exceeded a bounded resource";
    }
    return "unknown JSON canonicality status";
}

std::string_view json_canonicality_resource_message(
    const JsonCanonicalityResource resource) noexcept
{
    switch (resource) {
    case JsonCanonicalityResource::input_limit_exceeded:
        return "JSON input exceeds the 1048576-byte AURAL limit";
    case JsonCanonicalityResource::nesting_limit_exceeded:
        return "JSON container depth exceeds 32";
    case JsonCanonicalityResource::value_limit_exceeded:
        return "JSON value count exceeds the bounded limit";
    case JsonCanonicalityResource::decoded_string_limit_exceeded:
        return "aggregate decoded JSON string bytes exceed the bounded limit";
    case JsonCanonicalityResource::canonical_output_limit_exceeded:
        return "temporary canonical JSON exceeds the bounded output limit";
    case JsonCanonicalityResource::allocation_failure:
        return "memory allocation failed during canonicality checking";
    case JsonCanonicalityResource::internal_serialization_failure:
        return "validated JSON could not be serialized consistently";
    }
    return "unknown JSON canonicality resource failure";
}

} // namespace aural::core::json
