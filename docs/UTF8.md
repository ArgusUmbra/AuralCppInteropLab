# Experimental UTF-8 API

`<aural/core/utf8.hpp>` provides the bounded UTF-8 foundation for the future
strict JSON parser. The API is experimental: source and ABI compatibility are
not yet promised.

## Example

```cpp
#include <aural/core/utf8.hpp>

#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

int main()
{
    constexpr std::string_view text = "AURAL metadata";
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    const auto result = aural::core::validate_utf8(bytes);

    if (!result) {
        std::cerr << "invalid UTF-8 at byte " << result.error->offset << '\n';
        return 1;
    }

    std::cout << "valid UTF-8\n";
    return 0;
}
```

The repository builds this example as `validate_utf8_example`.

## Contract

`validate_utf8(std::span<const std::byte>)` borrows a caller-owned bounded byte
span. It never retains a pointer, writes to the input, reads beyond the span,
allocates, normalizes text, invokes locale conversion, or substitutes U+FFFD
for malformed input. It is `noexcept`; expected malformed input is returned as
a `Utf8ValidationResult`.

Success contains no error. Failure contains a strongly typed `Utf8ErrorCode`
and byte offset. Truncation reports the end offset where a byte was required
and provides expected and observed sequence lengths. A bad continuation
reports the offending byte and its offset. Errors involving a decoded scalar
report the leading-byte offset.

The categories are:

- `unexpected_continuation`
- `invalid_leading_byte`
- `truncated_sequence`
- `invalid_continuation_byte`
- `overlong_encoding`
- `surrogate_code_point`
- `code_point_out_of_range`
- `noncharacter`

The internal one-scalar primitive decodes into `char32_t` plus a width of one
through four bytes. It checks that each required byte is present immediately
before reading it, validates its continuation form, and only then combines its
bounded bit field. The public loop advances only by a successfully decoded
width. Work is linear in the input length and auxiliary storage is constant.

## UTF-8 versus JSON

The decoder implements RFC 3629 byte structure and Unicode scalar rules, then
applies AURAL Draft 0.2's additional rule forbidding all 66 Unicode
noncharacters. Unicode defines noncharacters as well-formed scalar values, so
`noncharacter` is deliberately distinct from a malformed UTF-8 category.
Consequently U+10FFFF has a structurally valid four-byte encoding but is
rejected by AURAL, while U+10FFFD is accepted.

This component is not a JSON parser. In particular:

- embedded U+0000 and other controls are valid at this decoder layer; a JSON
  parser must reject them when they occur unescaped in a string;
- an initial encoded U+FEFF is a valid scalar here; the manifest layer must
  separately enforce AURAL's no-BOM rule;
- ASCII text such as `"\\uD800"` is valid UTF-8; the JSON escape layer must
  reject an isolated escaped surrogate;
- quotes, backslashes, duplicate member names, nesting, number syntax, and
  trailing data are outside this component;
- normalization and transcoding are unsupported and intentionally absent.

The component validates and decodes for inspection but does not materialize a
decoded string. The future JSON parser can reuse the internal scalar primitive
while retaining the original bytes needed for exact RFC 8785 comparison.
