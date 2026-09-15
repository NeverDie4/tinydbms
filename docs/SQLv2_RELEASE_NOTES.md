# tinydbms SQL v2 Release Notes

## Release status

SQL v2 Release Candidate: **GREEN**

The final integration, hardening, and packaging audits have no remaining
Critical, High, Medium, or Low release issue. The previous stale `SlotId`
comment has been corrected without changing code or ABI.

## Highlights

- BIGINT, DOUBLE, BOOLEAN, VARCHAR, and SQL NULL values
- nullable schemas and SQL three-valued predicate logic
- UPDATE with prevalidation-safe execution
- ORDER BY
- INNER JOIN and qualified column references
- GROUP BY with COUNT, SUM, AVG, MIN, and MAX
- query-local SlotId execution and plan-owned result metadata
- did-you-mean diagnostics and advisory FixIts
- unified CompileStage diagnostics with precise half-open source ranges
- statement-level script recovery with shadow-catalog analysis
- Storage V2 with continued Storage V1 readability

UPDATE is not transactional or crash-atomic. Script recovery continues
analysis after an error, stops execution at the first failed statement, keeps
earlier successful effects, and performs no rollback. FixIts are advisory and
are never applied automatically.

Compiler diagnostics use statement-relative ranges, while split statements
carry absolute script ranges. Locations use 1-based UTF-8 byte columns and
0-based byte offsets; Core performs the only relative-to-absolute conversion.

## Compatibility

- Storage V1 metadata and rows remain readable and are not automatically
  migrated.
- Storage V1 and V2 tables can coexist in supported queries.
- Storage V2 carries SQL v2 types and NULL metadata.
- Storage V3 does not exist.

## Release evidence

| Gate | Result |
|---|---:|
| Fresh Debug | 52/52 PASS |
| Fresh Release | 52/52 PASS |
| Real Modules ON | 53/53 PASS |
| Strict compiler warnings | 0 |
| Diagnostics fuzz | seed 20260927, 1,200 PASS |
| Recovery fuzz | seed 20260928, 1,000 PASS |
| Release stress | seed 20260929, 500 scripts PASS |

Release stress executed 11,969 statements with 230 injected errors and 115
shadow CREATE statements.

## Frozen-scope exclusions

HAVING, DISTINCT, aliases, outer joins, subqueries, UNION, LIMIT/OFFSET,
window functions, arithmetic expressions, transactions, WAL, and crash
atomicity are not part of SQL v2.
