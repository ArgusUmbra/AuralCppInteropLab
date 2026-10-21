# Internal bounded JSON documents

`src/core/json/json_value_parser.hpp` is an internal, experimental C++20 API
for complete JSON documents. It accepts decoded strings, AURAL safe integers,
`true`, `false`, `null`, arrays, and objects. `parse_json_document` accepts any
of those root value types, permits only the four RFC 8259 whitespace bytes
outside strings, rejects a leading UTF-8 BOM, and rejects non-whitespace after
the single root value. `parse_json_value` remains available internally for a
bounded prefix parse when explicitly requested.

## Value and ownership model

`JsonValue` stores a `std::variant` of null, Boolean, signed 64-bit integer,
owned UTF-8 string, owned `std::vector<JsonValue>`, or an owned source-ordered
vector of `JsonObjectMember`. Each member owns its decoded property name and
value and records ranges for the name and complete member. Strings and names
contain decoded Unicode scalar values without normalization or case folding.
Every value records its half-open source byte range in the original input,
excluding surrounding JSON whitespace. The returned tree does not borrow the
input and contains no pointers into growable parser containers.

## Objects and duplicate names

Object grammar requires quoted names, colons, values, and exact comma/brace
placement. Initial parsing preserves source member order and never sorts or
overwrites members. Each live object frame has a parser-owned open-addressing
hash table containing member indices, never pointers into growable storage. A
later name is rejected if its decoded UTF-8 bytes already exist in that object,
so literal/escape and surrogate/literal spellings cannot bypass the check. The
error offset points to the later name's opening quote. Separate objects have
separate indexes. No Unicode normalization or case folding is performed.

The transient hash index provides expected constant-time lookup and avoids
quadratic duplicate detection. It stores no key copies and maintains at most a
50 percent load factor; except for the constant eight-slot initial table, its
slot capacity remains less than four times the indexed member count. The value
limit bounds indexed members and the aggregate decoded-string budget bounds the
owned names. Index storage is released when the object closes.

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
- total value count, including every array and object (and therefore bounding
  object-member count); and
- aggregate decoded bytes across string values and property names.

The root object or array has depth 1 and every nested container increments depth
by one, so depth 32 is accepted and the 33rd opening delimiter is rejected. The
parser uses an explicit variant frame vector rather than recursive descent. Its
live parse stack is therefore bounded by 32. Aggregate values and decoded
string bytes are accounted before their corresponding owned tree storage can
grow beyond the configured limit. The string decoder does not reserve storage
from the unparsed suffix.

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

Floating-point numbers are intentionally rejected by the AURAL integer-only
profile. RFC 8785 canonical serialization, canonical byte verification,
manifest root-object requirements, required properties, and all other manifest
semantics are unsupported. This internal API has no stable ABI promise and
must not be used as evidence that an AURAL manifest is valid or canonical.
