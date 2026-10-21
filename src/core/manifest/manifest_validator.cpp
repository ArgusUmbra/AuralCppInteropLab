#include "manifest_validator.hpp"

#include "../json/json_limits.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace aural::core::manifest {
namespace {

using json::JsonObjectMember;
using json::JsonValue;

constexpr std::u8string_view format_value = u8"org.aural.audio-evidence";
constexpr std::u8string_view version_value = u8"0.2-draft";
constexpr std::u8string_view original_value = u8"audio/original.wav";
constexpr std::u8string_view role_value = u8"original";
constexpr std::u8string_view media_type_value = u8"audio/wav";
constexpr std::int64_t maximum_asset_size = INT64_C(8589934592);

JsonFieldType field_type(const JsonValue& value) noexcept
{
    const auto& storage = value.storage;
    if (std::holds_alternative<std::nullptr_t>(storage)) {
        return JsonFieldType::null_value;
    }
    if (std::holds_alternative<bool>(storage)) {
        return JsonFieldType::boolean;
    }
    if (std::holds_alternative<std::int64_t>(storage)) {
        return JsonFieldType::integer;
    }
    if (std::holds_alternative<std::u8string>(storage)) {
        return JsonFieldType::string;
    }
    if (std::holds_alternative<JsonValue::Array>(storage)) {
        return JsonFieldType::array;
    }
    return JsonFieldType::object;
}

std::u8string pointer_child(
    const std::u8string_view parent,
    const std::u8string_view token)
{
    constexpr char8_t hexadecimal[] = u8"0123456789ABCDEF";
    std::u8string result(parent);
    result.push_back(u8'/');
    for (const auto character : token) {
        if (character == u8'~') {
            result.append(u8"~0");
        } else if (character == u8'/') {
            result.append(u8"~1");
        } else if (static_cast<std::uint8_t>(character) < UINT8_C(0x20) ||
                   character == u8'\x7f') {
            const auto byte = static_cast<std::uint8_t>(character);
            result.push_back(u8'%');
            result.push_back(hexadecimal[byte >> 4U]);
            result.push_back(hexadecimal[byte & UINT8_C(0x0F)]);
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::u8string pointer_index(
    const std::u8string_view parent,
    const std::size_t index)
{
    char buffer[32]{};
    const auto converted =
        std::to_chars(buffer, buffer + sizeof(buffer), index);
    if (converted.ec != std::errc{}) {
        throw std::length_error("manifest array index is too large");
    }
    std::u8string result(parent);
    result.push_back(u8'/');
    for (auto current = buffer; current != converted.ptr; ++current) {
        result.push_back(static_cast<char8_t>(*current));
    }
    return result;
}

ManifestStructureResult valid_result() noexcept
{
    return {ManifestStructureStatus::structurally_valid, std::nullopt,
            std::nullopt};
}

ManifestStructureResult resource_result() noexcept
{
    return {ManifestStructureStatus::resource_failure, std::nullopt,
            ManifestResource::allocation_failure};
}

ManifestStructureResult invalid_result(
    const ManifestErrorCode code,
    std::u8string path,
    const ManifestConstraint constraint = ManifestConstraint::none,
    const std::optional<JsonFieldType> expected = std::nullopt,
    const std::optional<JsonFieldType> actual = std::nullopt)
{
    return {ManifestStructureStatus::invalid_structure,
            ManifestError{code, std::move(path), constraint, expected, actual},
            std::nullopt};
}

const JsonObjectMember* find_member(
    const JsonValue::Object& object,
    const std::u8string_view name) noexcept
{
    const auto found = std::find_if(
        object.begin(), object.end(), [name](const JsonObjectMember& member) {
            return member.name == name;
        });
    return found == object.end() ? nullptr : &*found;
}

bool is_top_level_name(const std::u8string_view name) noexcept
{
    return name == u8"format" || name == u8"version" ||
           name == u8"original" || name == u8"assets" ||
           name == u8"title" || name == u8"recorded_at" ||
           name == u8"extensions" || name == u8"critical_extensions";
}

bool is_asset_name(const std::u8string_view name) noexcept
{
    return name == u8"path" || name == u8"role" ||
           name == u8"media_type" || name == u8"size" ||
           name == u8"sha256";
}

const JsonObjectMember* smallest_unknown(
    const JsonValue::Object& object,
    const bool top_level) noexcept
{
    const JsonObjectMember* selected = nullptr;
    for (const auto& member : object) {
        const bool known = top_level ? is_top_level_name(member.name)
                                     : is_asset_name(member.name);
        if (!known &&
            (selected == nullptr || member.name < selected->name)) {
            selected = &member;
        }
    }
    return selected;
}

ManifestStructureResult require_member(
    const JsonValue::Object& object,
    const std::u8string_view name,
    const JsonFieldType expected,
    const JsonObjectMember*& member,
    const std::u8string_view parent = {})
{
    auto path = pointer_child(parent, name);
    member = find_member(object, name);
    if (member == nullptr) {
        return invalid_result(ManifestErrorCode::missing_required_field,
                              std::move(path));
    }
    const auto actual = field_type(member->value);
    if (actual != expected) {
        return invalid_result(ManifestErrorCode::wrong_field_type,
                              std::move(path), ManifestConstraint::none,
                              expected, actual);
    }
    return valid_result();
}

ManifestStructureResult optional_type(
    const JsonValue::Object& object,
    const std::u8string_view name,
    const JsonFieldType expected,
    const JsonObjectMember*& member)
{
    member = find_member(object, name);
    if (member == nullptr) {
        return valid_result();
    }
    const auto actual = field_type(member->value);
    if (actual != expected) {
        return invalid_result(ManifestErrorCode::wrong_field_type,
                              pointer_child({}, name),
                              ManifestConstraint::none, expected, actual);
    }
    return valid_result();
}

bool fixed_string(
    const JsonObjectMember& member,
    const std::u8string_view expected) noexcept
{
    return std::get<std::u8string>(member.value.storage) == expected;
}

std::size_t scalar_count(const std::u8string_view value) noexcept
{
    std::size_t count = 0U;
    for (const auto byte : value) {
        if ((static_cast<std::uint8_t>(byte) & UINT8_C(0xC0)) !=
            UINT8_C(0x80)) {
            ++count;
        }
    }
    return count;
}

bool digits(const std::u8string_view value, const std::size_t offset,
            const std::size_t count) noexcept
{
    if (offset > value.size() || count > value.size() - offset) {
        return false;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const auto character = value[offset + index];
        if (character < u8'0' || character > u8'9') {
            return false;
        }
    }
    return true;
}

unsigned decimal(const std::u8string_view value, const std::size_t offset,
                 const std::size_t count) noexcept
{
    unsigned result = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        result = result * 10U +
                 static_cast<unsigned>(value[offset + index] - u8'0');
    }
    return result;
}

bool leap_year(const unsigned year) noexcept
{
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

bool valid_timestamp(const std::u8string_view value) noexcept
{
    if (value.size() < 20U || !digits(value, 0U, 4U) ||
        value[4] != u8'-' || !digits(value, 5U, 2U) ||
        value[7] != u8'-' || !digits(value, 8U, 2U) ||
        value[10] != u8'T' || !digits(value, 11U, 2U) ||
        value[13] != u8':' || !digits(value, 14U, 2U) ||
        value[16] != u8':' || !digits(value, 17U, 2U)) {
        return false;
    }
    const auto year = decimal(value, 0U, 4U);
    const auto month = decimal(value, 5U, 2U);
    const auto day = decimal(value, 8U, 2U);
    const auto hour = decimal(value, 11U, 2U);
    const auto minute = decimal(value, 14U, 2U);
    const auto second = decimal(value, 17U, 2U);
    if (year == 0U || month == 0U || month > 12U || hour > 23U ||
        minute > 59U || second > 59U) {
        return false;
    }
    constexpr unsigned month_days[] =
        {0U, 31U, 28U, 31U, 30U, 31U, 30U,
         31U, 31U, 30U, 31U, 30U, 31U};
    auto maximum_day = month_days[month];
    if (month == 2U && leap_year(year)) {
        maximum_day = 29U;
    }
    if (day == 0U || day > maximum_day) {
        return false;
    }

    std::size_t offset = 19U;
    if (offset < value.size() && value[offset] == u8'.') {
        const auto fraction_begin = ++offset;
        while (offset < value.size() && value[offset] >= u8'0' &&
               value[offset] <= u8'9') {
            ++offset;
        }
        const auto fraction_size = offset - fraction_begin;
        if (fraction_size == 0U || fraction_size > 9U) {
            return false;
        }
    }
    if (offset == value.size()) {
        return false;
    }
    if (value[offset] == u8'Z') {
        return offset + 1U == value.size();
    }
    if ((value[offset] != u8'+' && value[offset] != u8'-') ||
        value.size() - offset != 6U || !digits(value, offset + 1U, 2U) ||
        value[offset + 3U] != u8':' || !digits(value, offset + 4U, 2U)) {
        return false;
    }
    const auto offset_hour = decimal(value, offset + 1U, 2U);
    const auto offset_minute = decimal(value, offset + 4U, 2U);
    if (offset_hour > 14U || offset_minute > 59U ||
        (offset_hour == 14U && offset_minute != 0U)) {
        return false;
    }
    return !(value[offset] == u8'-' && offset_hour == 0U &&
             offset_minute == 0U);
}

bool valid_extension_identifier(const std::u8string_view value) noexcept
{
    if (value.empty() || value.size() > 255U || value.front() < u8'a' ||
        value.front() > u8'z') {
        return false;
    }
    std::size_t offset = 1U;
    while (offset < value.size() && value[offset] != u8':') {
        const auto character = value[offset];
        const bool allowed =
            (character >= u8'a' && character <= u8'z') ||
            (character >= u8'0' && character <= u8'9') ||
            character == u8'+' || character == u8'.' || character == u8'-';
        if (!allowed) {
            return false;
        }
        ++offset;
    }
    if (offset == value.size() || offset + 1U == value.size()) {
        return false;
    }
    for (++offset; offset < value.size(); ++offset) {
        if (value[offset] < u8'!' || value[offset] > u8'~') {
            return false;
        }
    }
    return true;
}

bool valid_sha256(const std::u8string_view value) noexcept
{
    if (value.size() != 64U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char8_t character) {
        return (character >= u8'0' && character <= u8'9') ||
               (character >= u8'a' && character <= u8'f');
    });
}

ManifestStructureResult validate_asset(const JsonValue& value)
{
    const auto* object = std::get_if<JsonValue::Object>(&value.storage);
    if (object == nullptr) {
        return invalid_result(ManifestErrorCode::wrong_field_type,
                              u8"/assets/0", ManifestConstraint::none,
                              JsonFieldType::object, field_type(value));
    }
    const JsonObjectMember* path = nullptr;
    auto result = require_member(*object, u8"path", JsonFieldType::string,
                                 path, u8"/assets/0");
    if (!result.structurally_valid()) return result;
    if (!fixed_string(*path, original_value)) {
        return invalid_result(ManifestErrorCode::invalid_fixed_value,
                              u8"/assets/0/path",
                              ManifestConstraint::asset_path);
    }
    const JsonObjectMember* role = nullptr;
    result = require_member(*object, u8"role", JsonFieldType::string, role,
                            u8"/assets/0");
    if (!result.structurally_valid()) return result;
    if (!fixed_string(*role, role_value)) {
        return invalid_result(ManifestErrorCode::invalid_fixed_value,
                              u8"/assets/0/role",
                              ManifestConstraint::asset_role);
    }
    const JsonObjectMember* media_type = nullptr;
    result = require_member(*object, u8"media_type", JsonFieldType::string,
                            media_type, u8"/assets/0");
    if (!result.structurally_valid()) return result;
    if (!fixed_string(*media_type, media_type_value)) {
        return invalid_result(ManifestErrorCode::invalid_fixed_value,
                              u8"/assets/0/media_type",
                              ManifestConstraint::asset_media_type);
    }
    const JsonObjectMember* size = nullptr;
    result = require_member(*object, u8"size", JsonFieldType::integer, size,
                            u8"/assets/0");
    if (!result.structurally_valid()) return result;
    const auto size_value = std::get<std::int64_t>(size->value.storage);
    if (size_value < 0 || size_value > maximum_asset_size) {
        return invalid_result(ManifestErrorCode::invalid_field_constraint,
                              u8"/assets/0/size",
                              ManifestConstraint::asset_size_range);
    }
    const JsonObjectMember* sha256 = nullptr;
    result = require_member(*object, u8"sha256", JsonFieldType::string,
                            sha256, u8"/assets/0");
    if (!result.structurally_valid()) return result;
    if (!valid_sha256(std::get<std::u8string>(sha256->value.storage))) {
        return invalid_result(ManifestErrorCode::invalid_field_constraint,
                              u8"/assets/0/sha256",
                              ManifestConstraint::sha256_syntax);
    }
    if (const auto* unknown = smallest_unknown(*object, false)) {
        return invalid_result(ManifestErrorCode::unknown_field,
                              pointer_child(u8"/assets/0", unknown->name));
    }
    return valid_result();
}

ManifestStructureResult validate_extensions(
    const JsonValue::Object& root)
{
    const JsonObjectMember* extensions_member = nullptr;
    auto result = optional_type(root, u8"extensions", JsonFieldType::object,
                                extensions_member);
    if (!result.structurally_valid()) return result;
    const JsonValue::Object* extensions = nullptr;
    if (extensions_member != nullptr) {
        extensions = &std::get<JsonValue::Object>(extensions_member->value.storage);
        for (const auto& extension : *extensions) {
            if (!valid_extension_identifier(extension.name)) {
                return invalid_result(
                    ManifestErrorCode::invalid_extension_structure,
                    pointer_child(u8"/extensions", extension.name),
                    ManifestConstraint::extension_identifier);
            }
        }
    }

    const JsonObjectMember* critical_member = nullptr;
    result = optional_type(root, u8"critical_extensions", JsonFieldType::array,
                           critical_member);
    if (!result.structurally_valid()) return result;
    if (critical_member == nullptr) {
        return valid_result();
    }
    const auto& critical =
        std::get<JsonValue::Array>(critical_member->value.storage);
    for (std::size_t index = 0U; index < critical.size(); ++index) {
        const auto path = pointer_index(u8"/critical_extensions", index);
        if (field_type(critical[index]) != JsonFieldType::string) {
            return invalid_result(ManifestErrorCode::wrong_field_type, path,
                                  ManifestConstraint::none,
                                  JsonFieldType::string,
                                  field_type(critical[index]));
        }
        const auto& identifier =
            std::get<std::u8string>(critical[index].storage);
        if (!valid_extension_identifier(identifier)) {
            return invalid_result(
                ManifestErrorCode::invalid_extension_structure, path,
                ManifestConstraint::extension_identifier);
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (std::get<std::u8string>(critical[previous].storage) ==
                identifier) {
                return invalid_result(
                    ManifestErrorCode::invalid_extension_structure, path,
                    ManifestConstraint::duplicate_critical_extension);
            }
        }
        if (extensions == nullptr ||
            find_member(*extensions, identifier) == nullptr) {
            return invalid_result(
                ManifestErrorCode::invalid_extension_structure, path,
                ManifestConstraint::critical_extension_missing_value);
        }
    }
    if (!critical.empty()) {
        // Draft 0.2 standardizes no extensions, so every structurally valid
        // critical identifier is unsupported by this bounded core validator.
        return invalid_result(
            ManifestErrorCode::unsupported_critical_extension,
            pointer_index(u8"/critical_extensions", 0U));
    }
    return valid_result();
}

ManifestStructureResult validate_structure(const JsonValue& document)
{
    const auto* root = std::get_if<JsonValue::Object>(&document.storage);
    if (root == nullptr) {
        return invalid_result(ManifestErrorCode::wrong_field_type, {},
                              ManifestConstraint::none, JsonFieldType::object,
                              field_type(document));
    }

    const JsonObjectMember* format = nullptr;
    auto result = require_member(*root, u8"format", JsonFieldType::string,
                                 format);
    if (!result.structurally_valid()) return result;
    if (!fixed_string(*format, format_value)) {
        return invalid_result(ManifestErrorCode::invalid_fixed_value,
                              u8"/format",
                              ManifestConstraint::format_identifier);
    }

    const JsonObjectMember* version = nullptr;
    result = require_member(*root, u8"version", JsonFieldType::string,
                            version);
    if (!result.structurally_valid()) return result;
    if (!fixed_string(*version, version_value)) {
        return invalid_result(ManifestErrorCode::unsupported_version,
                              u8"/version");
    }

    const JsonObjectMember* original = nullptr;
    result = require_member(*root, u8"original", JsonFieldType::string,
                            original);
    if (!result.structurally_valid()) return result;
    if (!fixed_string(*original, original_value)) {
        return invalid_result(ManifestErrorCode::invalid_fixed_value,
                              u8"/original",
                              ManifestConstraint::original_path);
    }

    const JsonObjectMember* assets = nullptr;
    result = require_member(*root, u8"assets", JsonFieldType::array, assets);
    if (!result.structurally_valid()) return result;
    const auto& asset_array = std::get<JsonValue::Array>(assets->value.storage);
    if (asset_array.size() != 1U) {
        return invalid_result(ManifestErrorCode::invalid_field_constraint,
                              u8"/assets", ManifestConstraint::asset_count);
    }
    result = validate_asset(asset_array.front());
    if (!result.structurally_valid()) return result;

    const JsonObjectMember* title = nullptr;
    result = optional_type(*root, u8"title", JsonFieldType::string, title);
    if (!result.structurally_valid()) return result;
    if (title != nullptr &&
        scalar_count(std::get<std::u8string>(title->value.storage)) > 4096U) {
        return invalid_result(ManifestErrorCode::invalid_field_constraint,
                              u8"/title",
                              ManifestConstraint::title_scalar_limit);
    }

    const JsonObjectMember* recorded_at = nullptr;
    result = optional_type(*root, u8"recorded_at", JsonFieldType::string,
                           recorded_at);
    if (!result.structurally_valid()) return result;
    if (recorded_at != nullptr &&
        !valid_timestamp(
            std::get<std::u8string>(recorded_at->value.storage))) {
        return invalid_result(ManifestErrorCode::invalid_field_constraint,
                              u8"/recorded_at",
                              ManifestConstraint::recorded_at_timestamp);
    }

    result = validate_extensions(*root);
    if (!result.structurally_valid()) return result;
    if (const auto* unknown = smallest_unknown(*root, true)) {
        return invalid_result(ManifestErrorCode::unknown_field,
                              pointer_child({}, unknown->name));
    }
    return valid_result();
}

std::optional<ManifestDocumentResource> parser_resource(
    const json::JsonValueErrorCode code) noexcept
{
    switch (code) {
    case json::JsonValueErrorCode::source_limit_exceeded:
        return ManifestDocumentResource::input_limit_exceeded;
    case json::JsonValueErrorCode::nesting_limit_exceeded:
        return ManifestDocumentResource::nesting_limit_exceeded;
    case json::JsonValueErrorCode::value_limit_exceeded:
        return ManifestDocumentResource::value_limit_exceeded;
    case json::JsonValueErrorCode::decoded_string_limit_exceeded:
        return ManifestDocumentResource::decoded_string_limit_exceeded;
    case json::JsonValueErrorCode::invalid_limits:
        return ManifestDocumentResource::internal_serialization_failure;
    default: return std::nullopt;
    }
}

std::optional<ManifestDocumentResource> serializer_resource(
    const json::CanonicalDocumentErrorCode code) noexcept
{
    switch (code) {
    case json::CanonicalDocumentErrorCode::nesting_limit_exceeded:
        return ManifestDocumentResource::nesting_limit_exceeded;
    case json::CanonicalDocumentErrorCode::value_limit_exceeded:
        return ManifestDocumentResource::value_limit_exceeded;
    case json::CanonicalDocumentErrorCode::decoded_string_limit_exceeded:
        return ManifestDocumentResource::decoded_string_limit_exceeded;
    case json::CanonicalDocumentErrorCode::output_limit_exceeded:
        return ManifestDocumentResource::canonical_output_limit_exceeded;
    case json::CanonicalDocumentErrorCode::invalid_limits:
    case json::CanonicalDocumentErrorCode::internal_serialization_failure:
        return ManifestDocumentResource::internal_serialization_failure;
    default: return std::nullopt;
    }
}

ManifestDocumentResult document_resource(
    const ManifestDocumentResource resource,
    std::optional<json::JsonValueError> parse_error = std::nullopt,
    std::optional<json::CanonicalDocumentError> serialization_error =
        std::nullopt) noexcept
{
    return {ManifestDocumentStatus::resource_failure, std::nullopt,
            std::move(parse_error), std::move(serialization_error),
            std::nullopt, resource};
}

} // namespace

ManifestStructureResult validate_manifest_structure(
    const json::JsonValue& document) noexcept
{
    try {
        return validate_structure(document);
    } catch (const std::bad_alloc&) {
        return resource_result();
    } catch (const std::length_error&) {
        return resource_result();
    }
}

ManifestDocumentResult validate_canonical_manifest_structure(
    const std::span<const std::byte> input) noexcept
{
    if (input.size() > json::max_manifest_bytes) {
        return document_resource(
            ManifestDocumentResource::input_limit_exceeded,
            json::JsonValueError{json::JsonValueErrorCode::source_limit_exceeded,
                                 json::max_manifest_bytes, std::nullopt,
                                 std::nullopt});
    }
    try {
        auto parsed = json::parse_json_document(input);
        if (!parsed) {
            auto detail = std::move(*parsed.error);
            if (const auto resource = parser_resource(detail.code)) {
                return document_resource(*resource, std::move(detail));
            }
            return {ManifestDocumentStatus::malformed_json, std::nullopt,
                    std::move(detail), std::nullopt, std::nullopt,
                    std::nullopt};
        }
        auto canonical =
            json::serialize_canonical_json_document(parsed.parsed->value);
        if (!canonical) {
            auto detail = std::move(*canonical.error);
            if (const auto resource = serializer_resource(detail.code)) {
                return document_resource(*resource, std::nullopt,
                                         std::move(detail));
            }
            return {ManifestDocumentStatus::malformed_json, std::nullopt,
                    std::nullopt, std::move(detail), std::nullopt,
                    std::nullopt};
        }
        const auto& expected = *canonical.serialized;
        const auto common = std::min(input.size(), expected.size());
        std::size_t offset = 0U;
        while (offset < common &&
               input[offset] == static_cast<std::byte>(expected[offset])) {
            ++offset;
        }
        if (offset != common || input.size() != expected.size()) {
            return {ManifestDocumentStatus::noncanonical_json, offset,
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt};
        }
        auto structure = validate_manifest_structure(parsed.parsed->value);
        if (structure.status == ManifestStructureStatus::resource_failure) {
            return document_resource(
                ManifestDocumentResource::allocation_failure);
        }
        if (!structure.structurally_valid()) {
            return {ManifestDocumentStatus::invalid_manifest_structure,
                    std::nullopt, std::nullopt, std::nullopt,
                    std::move(structure.error), std::nullopt};
        }
        return {ManifestDocumentStatus::structurally_valid, std::nullopt,
                std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    } catch (const std::bad_alloc&) {
        return document_resource(ManifestDocumentResource::allocation_failure);
    } catch (const std::length_error&) {
        return document_resource(ManifestDocumentResource::allocation_failure);
    }
}

std::string_view manifest_error_message(const ManifestErrorCode code) noexcept
{
    switch (code) {
    case ManifestErrorCode::missing_required_field:
        return "required manifest field is missing";
    case ManifestErrorCode::wrong_field_type:
        return "manifest field has the wrong JSON type";
    case ManifestErrorCode::unsupported_version:
        return "manifest version is unsupported";
    case ManifestErrorCode::invalid_fixed_value:
        return "manifest field has an invalid fixed identifier";
    case ManifestErrorCode::unknown_field:
        return "manifest contains an unknown core field";
    case ManifestErrorCode::invalid_field_constraint:
        return "manifest field violates a local constraint";
    case ManifestErrorCode::invalid_extension_structure:
        return "manifest extension structure is invalid";
    case ManifestErrorCode::unsupported_critical_extension:
        return "manifest requires an unsupported critical extension";
    }
    return "unknown manifest error";
}

std::string_view manifest_constraint_message(
    const ManifestConstraint constraint) noexcept
{
    switch (constraint) {
    case ManifestConstraint::none: return "no additional constraint detail";
    case ManifestConstraint::format_identifier:
        return "format must be org.aural.audio-evidence";
    case ManifestConstraint::original_path:
        return "original must be audio/original.wav";
    case ManifestConstraint::asset_count:
        return "assets must contain exactly one descriptor";
    case ManifestConstraint::asset_path:
        return "asset path must be audio/original.wav";
    case ManifestConstraint::asset_role:
        return "asset role must be original";
    case ManifestConstraint::asset_media_type:
        return "asset media_type must be audio/wav";
    case ManifestConstraint::sha256_syntax:
        return "sha256 must contain 64 lowercase hexadecimal characters";
    case ManifestConstraint::asset_size_range:
        return "asset size must be in the inclusive range 0 through 8589934592";
    case ManifestConstraint::title_scalar_limit:
        return "title exceeds 4096 Unicode scalar values";
    case ManifestConstraint::recorded_at_timestamp:
        return "recorded_at violates the Draft 0.2 RFC 3339 profile";
    case ManifestConstraint::extension_identifier:
        return "extension identifier violates the Draft 0.2 syntax";
    case ManifestConstraint::duplicate_critical_extension:
        return "critical extension identifiers must be unique";
    case ManifestConstraint::critical_extension_missing_value:
        return "critical extension identifier has no extensions entry";
    }
    return "unknown manifest constraint";
}

std::string_view manifest_document_status_message(
    const ManifestDocumentStatus status) noexcept
{
    switch (status) {
    case ManifestDocumentStatus::structurally_valid:
        return "canonical JSON has valid Draft 0.2 manifest structure only";
    case ManifestDocumentStatus::malformed_json:
        return "manifest JSON is malformed or outside the AURAL JSON profile";
    case ManifestDocumentStatus::noncanonical_json:
        return "manifest JSON bytes are not canonical";
    case ManifestDocumentStatus::invalid_manifest_structure:
        return "canonical JSON does not have valid Draft 0.2 manifest structure";
    case ManifestDocumentStatus::resource_failure:
        return "manifest validation exceeded a bounded resource";
    }
    return "unknown manifest document status";
}

} // namespace aural::core::manifest
