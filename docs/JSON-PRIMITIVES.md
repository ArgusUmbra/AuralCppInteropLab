# Internal strict JSON primitive parser

`src/core/json/json_primitive_parser.hpp` parses one integer or one exact
`true`, `false`, or `null` token at the start of a caller-owned byte span. It
does not skip whitespace or parse strings, arrays, objects, member names, or a
complete JSON text. The header remains internal and experimental.

## Normative rules and profile split

RFC 8259 permits an optional minus sign followed by either zero or a nonzero
digit and further digits. It forbids a leading plus sign and leading zeros.
Its ordinary number grammar also permits a fraction and exponent. AURAL Draft
0.2 deliberately narrows that grammar: every number must be an integer token
in the inclusive range `-9007199254740991` through `9007199254740991`, and
fraction syntax, exponent syntax, negative zero, NaN, and infinities are
rejected. This is the exact-interchange interval identified by RFC 8259 and
RFC 7493, not the wider capacity of C++ `std::int64_t`.

RFC 8259 defines only the lowercase literal names `true`, `false`, and `null`.
The parser therefore rejects misspellings, incorrect capitalization, and a
valid literal prefix immediately followed by another token character.

JSON's grammar makes token boundaries contextual rather than defining a
standalone primitive-parser boundary API. For safe composition, this component
requires the next byte after a primitive to be end-of-span, JSON whitespace,
`,`, `]`, or `}`. The boundary remains unconsumed. This choice rejects inputs
such as `trueish`, `1x`, and `null:` at the primitive layer. A future complete
document parser remains responsible for deciding which accepted delimiter is
legal in its precise array, object, or root context.

## Representation, arithmetic, and ownership

Integer conversion is decimal, locale-independent, and uses checked unsigned
accumulation against AURAL's safe-integer maximum before applying the sign.
There is no floating-point conversion. Success returns `std::int64_t`, the
exact consumed byte count, and a borrowed span containing the original token
spelling. The parser never changes caller storage.

Literal results carry a strongly typed `JsonLiteral` value plus the same
borrowed-source and consumed-count information. Separate entry points for
`true`, `false`, and `null` keep call sites explicit without creating a public
generic JSON value framework.

## Failure and resource behavior

Expected invalid input is returned as `JsonPrimitiveError`, with a typed code
and byte offset. Missing bytes point to end-of-span; range failure points to
the first digit that would exceed the safe-integer limit; syntax/profile
failures point to the offending byte; negative zero points to its leading
minus; and boundary failure points to the first byte after the token. Error
codes have stable explanatory messages for diagnostics.

The parser is `noexcept`, iterative, and allocation-free. Extremely long digit
sequences fail as soon as the checked accumulator would exceed the permitted
range. The input-span ceiling defaults to AURAL's 1,048,576-byte manifest
limit; callers may lower it but cannot raise it.

## Deferred scope

Whitespace orchestration, containers, complete-document termination, duplicate
member detection, nesting depth, JCS byte validation, manifest semantics, and
package verification are intentionally deferred.
