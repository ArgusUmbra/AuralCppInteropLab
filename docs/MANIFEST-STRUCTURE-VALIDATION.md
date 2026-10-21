# Draft 0.2 manifest structure validation

## Authority and requirement map

This component is derived only from the frozen public Draft 0.2 specification,
its companion schema, the public conformance documentation, and the frozen
golden manifest. The prose specification is normative. Schema statements below
describe the companion structural constraints rather than a general-purpose
schema implementation.

| Property | Normative Draft 0.2 requirement | Companion schema | Implemented first-layer check |
|---|---|---|---|
| document root | One JSON object; exact canonical UTF-8 bytes are required by Specification section 4 | `type: object` | Combined entry point requires strict bounded parsing, exact JCS bytes, then an object root |
| `format` | Required string exactly `org.aural.audio-evidence` | Required `const` | Presence, string type, and exact token |
| `version` | Required string exactly `0.2-draft`; draft versions are exact capability identifiers with no fallback | Required `const` | Presence, string type, and exact supported version; other strings are `unsupported_version` |
| `original` | Required string exactly `audio/original.wav` | Required `const` | Presence, string type, and exact path token |
| `assets` | Required array containing exactly one section 5.2 descriptor | Required array with `minItems` and `maxItems` of one | Presence, array type, and exactly one descriptor |
| `title` | Optional string of at most 4,096 Unicode scalar values; null is not accepted | Optional string with `maxLength: 4096` | Type and scalar-count ceiling |
| `recorded_at` | Optional string using the section 5.3 RFC 3339 profile; real calendar/clock values, year and offset restrictions apply; null is not accepted | Optional string with a lexical pattern | Type, exact lexical profile, calendar validity, leap-year/day validity, clock ranges, 1-9 fraction digits, offset range, and prohibited `-00:00` |
| `extensions` | Optional object mapping conforming extension identifiers to section 4 JSON values | Optional object with constrained property names and interoperable values | Object type and identifier syntax/byte length; value profile is already guaranteed by the owned strict JSON tree |
| `critical_extensions` | Optional array of unique identifiers; every identifier must exist in `extensions`; an unsupported critical extension must be rejected | Optional unique string array with constrained items | Array/item types, identifier syntax, uniqueness, membership, then unsupported-critical rejection |
| unknown top-level member | A core member not listed in section 5.1 must be rejected; extensions exist only under `extensions` | `additionalProperties: false` | Rejected as `unknown_field` |

The sole asset object is closed and requires all five members:

| Asset property | Normative requirement | Companion schema | Implemented check |
|---|---|---|---|
| `path` | String exactly `audio/original.wav` | Required `const` | Presence, type, exact token |
| `role` | String exactly `original` | Required `const` | Presence, type, exact token |
| `media_type` | String exactly `audio/wav` | Required `const` | Presence, type, exact token |
| `size` | Integer from 0 through 8,589,934,592 inclusive | Required bounded integer | Presence, type, local range only |
| `sha256` | 64 lowercase ASCII hexadecimal characters | Required string pattern | Presence, type, and syntax only |
| unknown member | Descriptor contains exactly the five listed members | `additionalProperties: false` | Rejected as `unknown_field` |

There is no normative/schema contradiction in this bounded contract. The
schema cannot express exact bytes, duplicate-name rejection, JCS, calendar
validity, critical-extension membership/support, or ZIP/package constraints;
the prose explicitly identifies those procedural requirements. Schema
`maxLength` and prose byte length agree for extension identifiers because the
required pattern permits ASCII only.

## Component boundary

`validate_manifest_structure` accepts a caller-owned, previously validated
`JsonValue` by const reference. It allocates only bounded diagnostic paths,
does not retain references, and does not mutate or normalize the tree.

`validate_canonical_manifest_structure` is the first pipeline integration. It
parses once under the existing 1,048,576-byte, depth-32, value-count, decoded-
string, safe-integer, duplicate-name, UTF-8, scalar, and noncharacter limits;
serializes that owned tree to verify exact JCS bytes; and then calls the shape
validator. Its success spelling is `structurally_valid`, deliberately not
`valid`, `accepted`, or `verified`.

Diagnostic paths use RFC 6901 JSON Pointer escaping and additionally
percent-encode ASCII controls to prevent diagnostic injection. Diagnostics
contain field names and constraint categories, never rejected field values or
full manifest contents. Validation order is fixed by the published contract,
and repeated validation returns the same first failure.

## Exact supported scope

This milestone checks required/optional membership, JSON types, fixed format
and version tokens, closed core objects, local string/integer/pattern limits,
the complete published timestamp profile, extension identifier/relationship
structure, and the Draft 0.2 rule that no critical extension is currently
standardized.

It does not compare `size` or `sha256` with an archive member, validate ZIP or
WAVE structures, perform hashes, establish timestamp truth, interpret optional
extensions, validate provenance or signatures, accept a package, or implement
Draft 0.1 dispatch. A correctly typed descriptor with locally well-formed but
factually incorrect size/digest declarations can therefore be structurally
valid and must still fail or pass only in a future package-semantic stage.

## Error model

The combined result keeps malformed JSON, noncanonical JSON, invalid manifest
structure, and bounded resource failure distinct. Manifest errors further
separate missing fields, wrong types, unsupported versions, invalid fixed
tokens, unknown fields, local field constraints, invalid extension structure,
and unsupported critical extensions. Wrong-type errors include expected and
actual JSON types. Constraint details and safe property paths make failures
actionable without logging manifest values.

The frozen golden manifest is used as a published positive byte-and-shape
vector. It is not treated as evidence of package validity.
