# Internal complete canonical JSON serialization

`src/core/json/canonical_json_document.hpp` is an internal, experimental
C++20 component. It serializes the existing owned `JsonValue` tree to complete
RFC 8785 canonical JSON. It is not a public JSON API, a manifest validator, or
an original-byte canonicality check, and it has no stable ABI promise.

## Canonicalization rules

The serializer implements RFC 8785 for AURAL Draft 0.2's deliberately narrower
data model:

- `null`, `true`, and `false` use their lowercase JSON tokens;
- integers use the shortest base-10 representation with no leading zero or
  positive sign and must remain in the inclusive safe-integer range
  −9,007,199,254,740,991 through 9,007,199,254,740,991;
- decoded strings reuse the 8C3A canonical string serializer, including the
  ECMAScript control escapes, lowercase `\uhhhh`, literal solidus and
  non-ASCII scalars, UTF-8 output, and no Unicode normalization;
- arrays retain their original element order;
- every object independently reuses the 8C3A non-mutating sorted-index view,
  comparing decoded names lexicographically as unsigned UTF-16 code units;
- objects emit names in that order, without changing their source-ordered
  member vectors; and
- no insignificant whitespace or trailing bytes are emitted.

Floating-point values are not supported because the existing AURAL JSON model
does not contain them. This is the AURAL safe-integer profile narrowing RFC
8785; it is not a replacement number algorithm. The closed variant currently
contains every supported value kind. A valueless or otherwise inconsistent
manually constructed internal value fails as an unsupported internal value.

## Validation, bounds, and memory

Serialization has two iterative passes. The first validates the entire tree,
counts values and aggregate decoded name/string bytes, verifies nesting,
revalidates Unicode, detects manually constructed duplicate names, obtains
per-object canonical index order, and computes the exact output length with
checked subtraction before every addition. The second reserves exactly that
length and emits tokens with an explicit container-frame stack. A validation
failure therefore returns no partial output, and neither pass uses recursive
descent.

The default and hard limits reuse the parser's maximum depth of 32, total value
count of 1,048,576, and aggregate decoded string/name budget of 1,048,576
bytes. A separate 33,554,432-byte canonical-output ceiling bounds temporary
serialization of an in-memory tree. It is intentionally distinct from—and
does not raise—the 1,048,576-byte maximum for an on-disk AURAL manifest.
Canonical escaping can make output larger than decoded input. Callers may
lower any serializer limit but cannot raise a hard limit, and output is never
truncated.

Temporary traversal storage is bounded by the value limit. Live emission
frames are bounded by depth 32, while their non-mutating object-order index
vectors collectively contain no more entries than the bounded tree. String
serialization uses the independently bounded 8C3A helper. Allocation failure
remains an exceptional resource failure, distinct from structured validation
rejection.

## Errors and future verification

`CanonicalDocumentResult` contains either owned canonical UTF-8 or a typed
`CanonicalDocumentError`. Errors distinguish invalid limits, depth/value/
decoded-string/output exhaustion, out-of-profile integers, invalid strings or
names, duplicate names, unsupported internal state, and a defensive emission
invariant failure. They include deterministic value/depth context, optional
original member index, measured output bytes, and nested string/property-order
details where applicable.

The full-document CTest target uses independently stated expected bytes for
primitive, mixed, Unicode, RFC 8785, depth, large-container, limit, invalid
internal-value, and frozen golden-manifest cases. Comparing canonical output
to original input bytes is implemented separately by the internal experimental
canonicality verifier; this serializer alone does not establish manifest
semantics or canonicality of an input document.

The implementation is validated locally with the complete Windows MSYS2
UCRT64 GCC Debug and Release suites and the GCC static analyzer. Baseline
commit `611fe4c` passed the Linux GCC, Clang, and Clang ASan/UBSan workflow,
including the GCC analyzer. Later uncommitted work is not claimed as
Linux-validated.
