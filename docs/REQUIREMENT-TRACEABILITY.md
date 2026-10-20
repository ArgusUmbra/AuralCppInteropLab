# Initial requirement traceability

Status values are `Frozen`, `Foundation tested`, `Planned`, and `Out of scope`.
`Frozen` means the authority is present byte-for-byte; it does not mean the
requirement is implemented or tested.

| Published requirement | Authority | Planned component/test | Current status |
|---|---|---|---|
| Conformance terminology and Core Reader obligations | Specification §1 | Requirement review and future verifier acceptance tests | Frozen; planned |
| Scope and trust model | Specification §2 | API contracts and security review | Frozen; planned |
| Logical package membership | Specification §3 | Package verifier and ZIP inventory tests | Frozen; planned |
| UTF-8, I-JSON, duplicate names, integer domain | Specification §4.1 | Strict UTF-8/JSON parser tests | Frozen; planned |
| Exact RFC 8785 canonical manifest bytes | Specification §4.2 | JCS component and canonicality vectors | Frozen; planned |
| Manifest members, assets, timestamps, extensions | Specification §5 | Immutable manifest model and semantic tests | Frozen; planned |
| ZIP records, names, header consistency, layout | Specification §§6.1-6.3 | Raw ZIP parser and malformed-layout tests | Frozen; planned |
| Extra fields and ZIP64 sentinel resolution | Specification §6.4 | ZIP64 parser and boundary tests | Frozen; planned |
| CRC32 and file metadata | Specification §6.5 | Streaming zlib integration and metadata tests | Dependency KAT only; planned |
| Resource limits | Specification §6.6 | Bounded I/O, allocation, depth, and aggregate tests | Bounded native file I/O foundation tested; parser limits planned |
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
