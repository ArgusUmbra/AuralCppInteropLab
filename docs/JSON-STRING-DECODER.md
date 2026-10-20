# Internal strict JSON string decoder

`src/core/json/json_string_decoder.hpp` is an internal, experimental component
for decoding one RFC 8259 JSON string token from a caller-owned byte span. It
does not parse objects, arrays, numbers, whitespace, or an entire JSON text.

## Normative split

RFC 8259 defines a string as beginning and ending with quotation marks. Every
Unicode character may appear literally except quotation mark, reverse solidus,
and U+0000 through U+001F. The only two-character escapes are `\"`, `\\`, `\/`,
`\b`, `\f`, `\n`, `\r`, and `\t`. A `\u` escape has exactly four hexadecimal
digits. A supplementary-plane scalar is represented in escape syntax by a
UTF-16 high surrogate immediately followed by a `\u` low surrogate. JSON text
exchanged between open systems is UTF-8.

RFC 7493's I-JSON profile requires UTF-8 and forbids surrogate and
noncharacter code points in both literal and escaped content. AURAL Draft 0.2
adopts I-JSON, explicitly requires invalid UTF-8, isolated surrogates, and
noncharacters to be rejected, and additionally requires Unicode to be
preserved without normalization. AURAL limits the complete uncompressed
manifest to 1,048,576 bytes. The decoder applies that ceiling to its source
span and, as an explicit engineering safety bound, to decoded UTF-8 output; an
internal caller may lower either ceiling but cannot raise it.

RFC 8259's grammar alone permits escape spellings for isolated surrogates.
Rejecting them, and rejecting noncharacters, are I-JSON rules reiterated by
AURAL.
Escaped controls including U+0000 remain valid string content; only their raw,
unescaped forms violate the JSON string grammar.

## Representation and ownership

Successful output is an owned `std::u8string`. It is canonical UTF-8 for the
decoded scalar sequence, not canonical JSON and not normalized Unicode. This
makes literal and escaped spellings compare equally while precomposed and
decomposed strings remain distinct.

The result also carries a borrowed view of the exact source token, including
quotation marks, and the number of input bytes consumed. The source view never
outlives caller storage. Bytes following the closing quotation mark remain
unconsumed. The decoder does not modify its input.

## Failure and allocation behavior

Malformed input returns a typed `JsonStringError` and exact source-byte offset;
no partial decoded value is exposed. Missing bytes are located at the end of
the span. Literal UTF-8 failures retain the complete 8C1 `Utf8Error`. The loop
is iterative and checks every read and append against its span or output bound.
Checked subtraction avoids size arithmetic wraparound.

Limits are validated before `reserve`. Expected malformed input is represented
as data rather than an exception. A genuine `std::bad_alloc` from the owned
decoded string is intentionally allowed to propagate separately.
