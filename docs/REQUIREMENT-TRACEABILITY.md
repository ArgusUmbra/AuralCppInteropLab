# Initial requirement traceability

Status values are `Frozen`, `Foundation tested`, `Planned`, and `Out of scope`.
`Frozen` means the authority is present byte-for-byte; it does not mean the
requirement is implemented or tested.

| Published requirement | Authority | Planned component/test | Current status |
|---|---|---|---|
| Conformance terminology and Core Reader obligations | Specification §1 | Requirement review and future verifier acceptance tests | Frozen; planned |
| Scope and trust model | Specification §2 | API contracts and security review | Frozen; planned |
| Logical package membership | Specification §3 | Package verifier and ZIP inventory tests | Frozen; planned |
| RFC 3629 UTF-8 byte structure; no overlong, truncated, bad-continuation, or invalid-lead sequences | Specification §4.1 via RFC 8259/RFC 3629 | `core_utf8_validation`: `test_valid_inputs`, `test_invalid_structure` | Foundation tested |
| Unicode scalar values only; no UTF-8 surrogate encodings or values above U+10FFFF | Specification §4.1 | `core_utf8_validation`: `test_invalid_scalars_and_policy` | Foundation tested |
| All 66 Unicode noncharacters forbidden, including U+10FFFF | Specification §4.1; Unicode noncharacter definition | `core_utf8_validation`: literal boundaries plus exhaustive 66-value loops | Foundation tested |
| Preserve Unicode as-is; do not normalize | Specification §4.1; RFC 8785 §3.1 | `core_utf8_validation`: precomposed/decomposed byte-preservation cases | Foundation tested |
| UTF-8 validation is distinct from JSON control, escape, BOM, and syntax rules | Specification §4.1; RFC 8259 §§7-8 | Embedded-zero decoder case; `core_json_values` malformed UTF-8/escape integration cases | Foundation tested locally; new CI run pending |
| Structured non-throwing UTF-8 failures with exact byte offsets | Engineering API requirement | `core_utf8_validation`: offset, length, observed-byte, prefix/suffix, and determinism cases | Foundation tested |
| UTF-8 validation uses bounded caller storage and constant auxiliary memory | Specification §§6.6, 7, 10 | `core_utf8_validation`: zero-span, unchanged-input, 1 MiB boundary, sanitizer matrix after commit | Foundation tested locally; new sanitizer run pending |
| RFC 8259 string quotes, escapes, controls, and bounded token consumption | Specification §4.1; RFC 8259 §7 | `core_json_string_decoder`: valid strings, escape matrix, truncation, exact consumption | Foundation tested |
| Escaped UTF-16 surrogate pairs; rejection of isolated/reversed surrogates | Specification §4.1; RFC 8259 §8.2 | `core_json_string_decoder`: BMP, supplementary, and malformed-pair cases | Foundation tested |
| Literal UTF-8, scalar-only strings, noncharacter rejection, and no normalization | Specification §4.1 | `core_json_string_decoder`: UTF-8 detail, literal/escaped equivalence, noncharacters, normalization distinction | Foundation tested |
| Bounded JSON string source/output and structured exact-offset failures | Specification §§4.1, 6.6, 10 | `core_json_string_decoder`: reduced limits, 1 MiB overflow, malformed and unchanged-input cases | Foundation tested locally; new sanitizer run pending |
| RFC 8259 integer token syntax: optional minus, no plus, no leading zeros | Specification §4.1 via RFC 8259 §6 | `core_json_primitives`: valid values, plus, leading-zero, and truncation cases | Foundation tested |
| AURAL integer-only domain, safe range, and negative-zero rejection | Specification §4.1 item 4; RFC 7493 §2.2 | `core_json_primitives`: exact ±(2^53−1), one beyond each edge, fraction, exponent, and `-0` cases | Foundation tested locally; new CI run pending |
| Exact lowercase `true`, `false`, and `null` with bounded token consumption | RFC 8259 §3 | `core_json_primitives`: all literals, truncation, capitalization, mismatch, boundaries, and consumption | Foundation tested locally; new CI run pending |
| Allocation-free primitive conversion, checked arithmetic, exact source token, structured byte offsets | Specification §§4.1, 6.6, 10 | `core_json_primitives`: hostile digits, unchanged input, source borrowing, determinism, limits, and offsets | Foundation tested locally; new sanitizer run pending |
| Arrays, exact delimiters, whitespace, shared value dispatch, and complete consumption | Specification §4.1; RFC 8259 §§2-5 | `core_json_values`: empty/single/mixed/nested arrays, malformed delimiters, unsupported objects, extra tokens | Foundation tested locally; new CI run pending |
| Container depth 32 with root container at depth 1 | Specification §4.1 item 5 | `core_json_values`: exact 32/33 boundary and hostile nesting | Foundation tested locally; new CI run pending |
| Bounded complete input, aggregate decoded strings/value count, iterative parse stack, and no partial result | Specification §§4.1, 6.6, 10 | `core_json_values`: 1 MiB overflow, reduced budgets, 10,000 elements, failure/determinism checks | Foundation tested locally; new CI run pending |
| Objects and decoded duplicate-name rejection | Specification §4.1 item 1 | Milestone 8C2B2B object parser tests | Frozen; planned |
| Exact RFC 8785 canonical manifest bytes | Specification §4.2 | JCS component and canonicality vectors | Frozen; planned |
| Manifest members, assets, timestamps, extensions | Specification §5 | Immutable manifest model and semantic tests | Frozen; planned |
| ZIP records, names, header consistency, layout | Specification §§6.1-6.3 | Raw ZIP parser and malformed-layout tests | Frozen; planned |
| Extra fields and ZIP64 sentinel resolution | Specification §6.4 | ZIP64 parser and boundary tests | Frozen; planned |
| CRC32 and file metadata | Specification §6.5 | Streaming zlib integration and metadata tests | Dependency KAT only; planned |
| Resource limits | Specification §6.6 | Bounded I/O, allocation, depth, and aggregate tests | File I/O and JSON value/array foundations tested locally; new CI run pending |
| Ordered package verification algorithm | Specification §7 | Package-verifier integration tests | Frozen; planned |
| Version and optional Draft 0.1 behavior | Specification §8 | Version dispatch; optional legacy suite | Frozen; planned |
| Canonical identity and deterministic output | Specification §9 | Identity/determinism tests | Frozen; planned |
| Security and privacy considerations | Specification §10 | Threat-model review and negative testing | Frozen; planned |
| Adapter request/response and error semantics | Adapter Protocol 1 §§2-6 | Future adapter executable and external runner | Frozen; planned |
| Thirty required Core Reader cases | Core Reader case manifest v1 | Future external conformance run | Inputs frozen; not executed |
| Two Draft 0.1 cases | Core Reader case manifest v1 | Optional legacy run | Optional inputs frozen; not executed |
| SHA-256 provider availability | Engineering foundation | `dependency_known_answers` | Foundation tested |
| CRC32 provider availability | Engineering foundation | `dependency_known_answers` | Foundation tested |
| Frozen snapshot byte integrity | Snapshot metadata | `frozen_input_checksums` | Foundation tested |
| Core Writer, provenance, signed-event graphs | Conformance scope limitations | None in Core Reader foundation | Out of scope |
