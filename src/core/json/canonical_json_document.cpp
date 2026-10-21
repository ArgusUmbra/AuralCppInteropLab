#include "canonical_json_document.hpp"

#include "json_primitive_parser.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace aural::core::json {
namespace {

struct MeasureTask final {
    const JsonValue* value;
    std::size_t containing_depth;
};

struct Measurement final {
    std::optional<std::size_t> output_size;
    std::optional<CanonicalDocumentError> error;
};

CanonicalDocumentError make_error(
    const CanonicalDocumentErrorCode code,
    const std::size_t value_index,
    const std::size_t depth,
    const std::size_t output_bytes,
    std::optional<std::size_t> member_index = std::nullopt,
    std::optional<CanonicalStringError> string_error = std::nullopt,
    std::optional<CanonicalPropertyOrderError> property_order_error =
        std::nullopt)
{
    return CanonicalDocumentError{code, value_index, depth, member_index,
                                  output_bytes, std::move(string_error),
                                  std::move(property_order_error)};
}

Measurement measure_document(
    const JsonValue& root,
    const CanonicalDocumentLimits limits)
{
    std::vector<MeasureTask> work;
    work.reserve(limits.max_depth);
    work.push_back(MeasureTask{&root, 0U});

    std::size_t scheduled_values = 1U;
    std::size_t visited_values = 0U;
    std::size_t decoded_string_bytes = 0U;
    std::size_t output_bytes = 0U;

    const auto fail = [&](const CanonicalDocumentErrorCode code,
                          const std::size_t value_index,
                          const std::size_t depth,
                          std::optional<std::size_t> member_index = std::nullopt,
                          std::optional<CanonicalStringError> string_error =
                              std::nullopt,
                          std::optional<CanonicalPropertyOrderError>
                              property_error = std::nullopt) {
        return Measurement{
            std::nullopt,
            make_error(code, value_index, depth, output_bytes, member_index,
                       std::move(string_error), std::move(property_error))};
    };
    const auto add_output = [&](const std::size_t amount) {
        if (output_bytes > limits.max_output_bytes ||
            amount > limits.max_output_bytes - output_bytes) {
            return false;
        }
        output_bytes += amount;
        return true;
    };
    const auto account_decoded = [&](const std::size_t amount) {
        if (decoded_string_bytes > limits.max_decoded_string_bytes ||
            amount > limits.max_decoded_string_bytes - decoded_string_bytes) {
            return false;
        }
        decoded_string_bytes += amount;
        return true;
    };
    const auto schedule_children = [&](const std::size_t count) {
        if (scheduled_values > limits.max_values ||
            count > limits.max_values - scheduled_values) {
            return false;
        }
        scheduled_values += count;
        return true;
    };

    while (!work.empty()) {
        const auto task = work.back();
        work.pop_back();
        const auto value_index = visited_values++;
        const auto& storage = task.value->storage;

        if (storage.valueless_by_exception()) {
            return fail(CanonicalDocumentErrorCode::unsupported_internal_value,
                        value_index, task.containing_depth);
        }
        if (std::holds_alternative<std::nullptr_t>(storage)) {
            if (!add_output(4U)) {
                return fail(CanonicalDocumentErrorCode::output_limit_exceeded,
                            value_index, task.containing_depth);
            }
            continue;
        }
        if (const auto* boolean = std::get_if<bool>(&storage)) {
            if (!add_output(*boolean ? 4U : 5U)) {
                return fail(CanonicalDocumentErrorCode::output_limit_exceeded,
                            value_index, task.containing_depth);
            }
            continue;
        }
        if (const auto* integer = std::get_if<std::int64_t>(&storage)) {
            if (*integer < min_aural_json_integer ||
                *integer > max_aural_json_integer) {
                return fail(CanonicalDocumentErrorCode::integer_out_of_range,
                            value_index, task.containing_depth);
            }
            char buffer[32]{};
            const auto converted =
                std::to_chars(buffer, buffer + sizeof(buffer), *integer);
            if (converted.ec != std::errc{} ||
                !add_output(static_cast<std::size_t>(converted.ptr - buffer))) {
                return fail(CanonicalDocumentErrorCode::output_limit_exceeded,
                            value_index, task.containing_depth);
            }
            continue;
        }
        if (const auto* string = std::get_if<std::u8string>(&storage)) {
            const auto serialized = serialize_canonical_json_string(*string);
            if (!serialized) {
                return fail(CanonicalDocumentErrorCode::invalid_string,
                            value_index, task.containing_depth, std::nullopt,
                            serialized.error);
            }
            if (!account_decoded(string->size())) {
                return fail(
                    CanonicalDocumentErrorCode::decoded_string_limit_exceeded,
                    value_index, task.containing_depth);
            }
            if (!add_output(serialized.serialized->size())) {
                return fail(CanonicalDocumentErrorCode::output_limit_exceeded,
                            value_index, task.containing_depth);
            }
            continue;
        }

        const auto container_depth = task.containing_depth + 1U;
        if (container_depth > limits.max_depth) {
            return fail(CanonicalDocumentErrorCode::nesting_limit_exceeded,
                        value_index, container_depth);
        }
        if (const auto* array = std::get_if<JsonValue::Array>(&storage)) {
            if (!schedule_children(array->size())) {
                return fail(CanonicalDocumentErrorCode::value_limit_exceeded,
                            value_index, container_depth);
            }
            if (!add_output(2U) ||
                (!array->empty() && !add_output(array->size() - 1U))) {
                return fail(CanonicalDocumentErrorCode::output_limit_exceeded,
                            value_index, container_depth);
            }
            for (auto item = array->rbegin(); item != array->rend(); ++item) {
                work.push_back(MeasureTask{&*item, container_depth});
            }
            continue;
        }
        if (const auto* object = std::get_if<JsonValue::Object>(&storage)) {
            if (!schedule_children(object->size())) {
                return fail(CanonicalDocumentErrorCode::value_limit_exceeded,
                            value_index, container_depth);
            }
            if (!add_output(2U) ||
                (!object->empty() &&
                 (!add_output(object->size()) ||
                  !add_output(object->size() - 1U)))) {
                return fail(CanonicalDocumentErrorCode::output_limit_exceeded,
                            value_index, container_depth);
            }

            const auto ordered = canonical_property_order(*object);
            if (!ordered) {
                return fail(CanonicalDocumentErrorCode::invalid_property_name,
                            value_index, container_depth,
                            ordered.error->member_index, std::nullopt,
                            ordered.error);
            }
            for (std::size_t position = 1U;
                 position < ordered.indices->size(); ++position) {
                const auto previous = (*ordered.indices)[position - 1U];
                const auto current = (*ordered.indices)[position];
                if ((*object)[previous].name == (*object)[current].name) {
                    return fail(
                        CanonicalDocumentErrorCode::duplicate_property_name,
                        value_index, container_depth, current);
                }
            }
            for (std::size_t member_index = 0U;
                 member_index < object->size(); ++member_index) {
                const auto& name = (*object)[member_index].name;
                const auto serialized = serialize_canonical_json_string(name);
                if (!serialized) {
                    return fail(
                        CanonicalDocumentErrorCode::invalid_property_name,
                        value_index, container_depth, member_index,
                        serialized.error);
                }
                if (!account_decoded(name.size())) {
                    return fail(
                        CanonicalDocumentErrorCode::decoded_string_limit_exceeded,
                        value_index, container_depth, member_index);
                }
                if (!add_output(serialized.serialized->size())) {
                    return fail(
                        CanonicalDocumentErrorCode::output_limit_exceeded,
                        value_index, container_depth, member_index);
                }
            }
            for (auto member = object->rbegin(); member != object->rend();
                 ++member) {
                work.push_back(MeasureTask{&member->value, container_depth});
            }
            continue;
        }

        return fail(CanonicalDocumentErrorCode::unsupported_internal_value,
                    value_index, task.containing_depth);
    }

    return {output_bytes, std::nullopt};
}

bool append_string(std::u8string& output, const std::u8string_view value)
{
    const auto serialized = serialize_canonical_json_string(value);
    if (!serialized) {
        return false;
    }
    output.append(*serialized.serialized);
    return true;
}

bool append_integer(std::u8string& output, const std::int64_t value)
{
    char buffer[32]{};
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (converted.ec != std::errc{}) {
        return false;
    }
    for (auto current = buffer; current != converted.ptr; ++current) {
        output.push_back(static_cast<char8_t>(*current));
    }
    return true;
}

struct ArrayEmitFrame final {
    const JsonValue::Array* array;
    std::size_t next_index;
};

struct ObjectEmitFrame final {
    const JsonValue::Object* object;
    std::vector<std::size_t> order;
    std::size_t next_position;
};

using EmitFrame = std::variant<ArrayEmitFrame, ObjectEmitFrame>;

bool emit_document(
    const JsonValue& root,
    const std::size_t output_size,
    std::u8string& output)
{
    output.reserve(output_size);
    std::vector<EmitFrame> frames;
    frames.reserve(max_json_container_depth);
    const JsonValue* current = &root;

    while (true) {
        if (current != nullptr) {
            const auto& storage = current->storage;
            if (std::holds_alternative<std::nullptr_t>(storage)) {
                output.append(u8"null");
            } else if (const auto* boolean = std::get_if<bool>(&storage)) {
                output.append(*boolean ? u8"true" : u8"false");
            } else if (const auto* integer =
                           std::get_if<std::int64_t>(&storage)) {
                if (!append_integer(output, *integer)) {
                    return false;
                }
            } else if (const auto* string =
                           std::get_if<std::u8string>(&storage)) {
                if (!append_string(output, *string)) {
                    return false;
                }
            } else if (const auto* array =
                           std::get_if<JsonValue::Array>(&storage)) {
                output.push_back(u8'[');
                if (array->empty()) {
                    output.push_back(u8']');
                } else {
                    frames.emplace_back(ArrayEmitFrame{array, 1U});
                    current = &(*array)[0];
                    continue;
                }
            } else if (const auto* object =
                           std::get_if<JsonValue::Object>(&storage)) {
                output.push_back(u8'{');
                if (object->empty()) {
                    output.push_back(u8'}');
                } else {
                    auto ordered = canonical_property_order(*object);
                    if (!ordered) {
                        return false;
                    }
                    const auto first = (*ordered.indices)[0];
                    frames.emplace_back(ObjectEmitFrame{
                        object, std::move(*ordered.indices), 1U});
                    if (!append_string(output, (*object)[first].name)) {
                        return false;
                    }
                    output.push_back(u8':');
                    current = &(*object)[first].value;
                    continue;
                }
            } else {
                return false;
            }
            current = nullptr;
        }

        while (current == nullptr) {
            if (frames.empty()) {
                return output.size() == output_size;
            }
            auto& frame = frames.back();
            if (auto* array = std::get_if<ArrayEmitFrame>(&frame)) {
                if (array->next_index < array->array->size()) {
                    output.push_back(u8',');
                    current = &(*array->array)[array->next_index++];
                    break;
                }
                output.push_back(u8']');
                frames.pop_back();
                continue;
            }

            auto& object = std::get<ObjectEmitFrame>(frame);
            if (object.next_position < object.order.size()) {
                output.push_back(u8',');
                const auto index = object.order[object.next_position++];
                if (!append_string(output, (*object.object)[index].name)) {
                    return false;
                }
                output.push_back(u8':');
                current = &(*object.object)[index].value;
                break;
            }
            output.push_back(u8'}');
            frames.pop_back();
        }
    }
}

} // namespace

CanonicalDocumentResult serialize_canonical_json_document(
    const JsonValue& value,
    const CanonicalDocumentLimits limits)
{
    if (limits.max_depth > max_json_container_depth ||
        limits.max_values == 0U || limits.max_values > max_manifest_bytes ||
        limits.max_decoded_string_bytes > max_manifest_bytes ||
        limits.max_output_bytes > max_canonical_document_bytes) {
        return {std::nullopt,
                make_error(CanonicalDocumentErrorCode::invalid_limits, 0U, 0U,
                           0U)};
    }

    const auto measurement = measure_document(value, limits);
    if (!measurement.output_size.has_value()) {
        return {std::nullopt, measurement.error};
    }

    std::u8string output;
    if (!emit_document(value, *measurement.output_size, output)) {
        return {std::nullopt,
                make_error(
                    CanonicalDocumentErrorCode::internal_serialization_failure,
                    0U, 0U, 0U)};
    }
    return {std::move(output), std::nullopt};
}

std::string_view canonical_document_error_message(
    const CanonicalDocumentErrorCode code) noexcept
{
    switch (code) {
    case CanonicalDocumentErrorCode::invalid_limits:
        return "canonical document limits exceed hard bounds";
    case CanonicalDocumentErrorCode::unsupported_internal_value:
        return "JSON value storage is unsupported or invalid";
    case CanonicalDocumentErrorCode::nesting_limit_exceeded:
        return "canonical document nesting limit exceeded";
    case CanonicalDocumentErrorCode::value_limit_exceeded:
        return "canonical document value limit exceeded";
    case CanonicalDocumentErrorCode::decoded_string_limit_exceeded:
        return "canonical document decoded-string budget exceeded";
    case CanonicalDocumentErrorCode::output_limit_exceeded:
        return "canonical document output-size limit exceeded";
    case CanonicalDocumentErrorCode::integer_out_of_range:
        return "integer is outside the AURAL safe-integer range";
    case CanonicalDocumentErrorCode::invalid_string:
        return "string value is not valid AURAL Unicode";
    case CanonicalDocumentErrorCode::invalid_property_name:
        return "object property name is not valid AURAL Unicode";
    case CanonicalDocumentErrorCode::duplicate_property_name:
        return "object contains a duplicate decoded property name";
    case CanonicalDocumentErrorCode::internal_serialization_failure:
        return "validated JSON value could not be serialized consistently";
    }
    return "unknown canonical document error";
}

} // namespace aural::core::json
