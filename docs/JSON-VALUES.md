# Internal bounded JSON values and arrays

`src/core/json/json_value_parser.hpp` is an internal, experimental C++20 API
for one deliberately incomplete JSON subset. It accepts decoded strings,
AURAL safe integers, `true`, `false`, `null`, and arrays containing those same
values. Objects are rejected with `unsupported_object`; object parsing and
decoded duplicate-name rejection belong to milestone 8C2B2B. Floating-point
numbers, manifest validation, and canonical serialization are not implemented.

## Value and ownership model

`JsonValue` stores a `std::variant` of null, Boolean, signed 64-bit integer,
owned UTF-8 string, or owned `std::vector<JsonValue>`. Strings contain decoded
Unicode scalar values without normalization. Every value also records its
half-open source byte range in the original input, excluding surrounding JSON
whitespace. The returned tree does not borrow the input.

Parsing reuses the strict string decoder and primitive parser. Expected input
failures return `JsonValueError`; allocation failure remains an exception and
is not misreported as invalid JSON. A failure result never contains a partial
tree. Nested string and primitive details are retained with document-absolute
byte offsets.

## Bounds and depth

The default and hard document ceiling is 1,048,576 bytes. Callers may lower,
but cannot raise, these limits:

- source bytes;
- maximum container depth, whose hard ceiling is 32;
- total value count, including every array; and
- aggregate decoded bytes across every string.

The root array has depth 1 and each nested array increments depth by one, so 32
nested arrays are accepted and the 33rd opening bracket is rejected. The parser
uses an explicit frame vector rather than recursive descent. Its live parse
stack is therefore bounded by 32. Aggregate values and decoded string bytes are
accounted before their corresponding storage can grow beyond the configured
limit. The string decoder does not reserve storage from the unparsed suffix.

## Consumption and errors

Leading and trailing RFC 8259 whitespace is accepted. Complete mode (the
default) requires the entire input after trailing whitespace to be consumed.
Prefix mode stops immediately after one complete value and reports the consumed
byte count. Token spellings and boundaries remain exact; malformed suffixes,
missing/repeated commas, trailing commas, missing brackets, and extra values are
rejected deterministically.

`JsonValueError::offset` is always a zero-based byte offset in the supplied
document. If a required byte is missing, the offset equals the document size.
`string_error` or `primitive_error` is populated when the corresponding reused
component rejected a token. `json_value_error_message` supplies a stable,
non-empty developer description, but callers should branch on the typed code.

This component is not a complete AURAL JSON parser and must not be used as
evidence that an AURAL manifest is valid or canonical.
