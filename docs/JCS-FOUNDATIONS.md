# Internal RFC 8785 string and property-ordering foundations

`src/core/json/canonical_json_string.hpp` and
`src/core/json/json_property_order.hpp` are internal, experimental components.
They implement canonical JSON string serialization and canonical object
property ordering. The complete serializer described in
`CANONICAL-JSON-DOCUMENTS.md` reuses both foundations; none of these components
establish manifest or package validity.

## Normative behavior

RFC 8785 section 3.2.2.2 delegates JSON string serialization to ECMAScript's
`JSON.stringify` rules. A decoded string is surrounded by quotation marks.
Quotation mark and reverse solidus use `\"` and `\\`. U+0008, U+0009, U+000A,
U+000C, and U+000D use `\b`, `\t`, `\n`, `\f`, and `\r`; the other U+0000
through U+001F controls use lowercase `\uhhhh`. Every other scalar is copied
as its original UTF-8 bytes. In particular, solidus, U+2028, and U+2029 are
not escaped, supplementary-plane scalars remain UTF-8, and Unicode is never
normalized.

RFC 8785 section 3.2.3 orders object property names lexicographically by their
decoded UTF-16 code units. The comparator decodes UTF-8 scalars and emits one
conceptual unit for BMP values or the high and low surrogate units for a
supplementary-plane value. It performs unsigned numeric unit comparison, then
shorter-prefix comparison. It has no locale, case-folding, or normalization
dependency. Thus U+10000 (`D800 DC00`) sorts before U+E000, even though the
UTF-8 bytes for U+E000 sort first.

The AURAL Draft 0.2 profile additionally permits only Unicode scalar values,
rejects all Unicode noncharacters, and requires Unicode preservation without
normalization. Both components revalidate those rules rather than assuming a
trusted caller.

## Ownership and mutation

String serialization accepts a borrowed `std::u8string_view` and returns an
owned `std::u8string`, including quotation marks. It never changes the input.
Property ordering returns indices into the existing source-ordered
`JsonValue::Object`. It does not reorder members or modify names or values.
Stable sorting makes manually constructed equal names deterministic, although
the parser already rejects duplicate decoded names.

## Bounds and failures

The decoded string or individual property name is limited to the frozen
1,048,576-byte manifest ceiling. A canonical string has a distinct hard output
ceiling of 6,291,458 bytes: six bytes for each possible decoded input byte,
plus two quotation marks. The serializer first validates and computes the
exact output size with checked subtraction, and only then reserves that exact
size. It never reserves from an attacker-declared length or exposes partial
output. Callers may lower either limit but cannot raise it.

Object ordering accepts at most 1,048,576 members, matching the parser's hard
value/member bound. Validation and comparison are iterative. Comparator work
uses constant auxiliary space; the separate index vector and sorting require
linear index storage and bounded `O(n log n)` comparisons. Invalid UTF-8,
noncharacters, limit violations, and the offending side/member and byte offset
are reported deterministically. Allocation failure remains an exceptional
resource failure rather than malformed JSON.

Complete iterative JCS serialization is now implemented separately.
Original-byte comparison, manifest semantic validation, and the eventual
`aural::verify(path)` entry point remain future work.
