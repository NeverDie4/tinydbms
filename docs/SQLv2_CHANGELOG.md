# tinydbms SQL v2 Changelog

## Overview

SQL v2 extends the original single-table INT/VARCHAR database into a broader,
slot-bound query engine while preserving the readable Storage V1 format. The
release adds scalar types, nullable values and three-valued predicates, data
modification, ordering, joins, aggregation, diagnostics, and statement-level
script recovery.

## Public contract changes

- `Type` now describes INT, BIGINT, DOUBLE, BOOLEAN, and VARCHAR.
- `Value` can carry SQL NULL, `int32`, `int64`, `double`, `bool`, and string
  values.
- `ColumnMeta` records nullability.
- Query plans use query-local `SlotId` bindings through scans, expressions,
  projection, sorting, joins, aggregation, and final `QueryOutput` metadata.
- Diagnostics use `CompileStage` and statement-relative half-open
  `SourceRange` values with 1-based UTF-8 byte columns and 0-based byte
  offsets; they can carry advisory suggestions and one `FixIt` replacement.
- `SplitStatement` preserves exact script text and an absolute source range;
  script execution reports an ordered outcome for every analyzed statement.

## Compiler

- Added BIGINT, DOUBLE, BOOLEAN, and NULL syntax and semantic analysis.
- Added UPDATE assignments and validation.
- Added ORDER BY with deterministic slot binding.
- Added INNER JOIN and qualified column references.
- Added GROUP BY and COUNT, SUM, AVG, MIN, and MAX.
- Added deterministic numeric widening and nullable expression typing.
- Migrated public plans from physical `ColumnId` expression references to
  query-local `SlotId` dataflow bindings.
- Extended deterministic plan formatting and compiler fuzz invariants.

## Core

- Unified scalar and predicate expression results through `Value`.
- Added internal TRUE/FALSE/UNKNOWN predicate evaluation and SQL three-valued
  NOT, AND, and OR.
- Added explicit physical-row to `SlotRow` materialization.
- Added slot-aware filtering, projection, sorting, joining, grouping, and
  aggregate execution.
- Made `QueryPlan.outputs` the authoritative source of result-column names and
  types.
- Added UPDATE prevalidation before applying a batch of row changes.

## Storage

- Added Storage V2 metadata and record encoding.
- Added BIGINT, DOUBLE, BOOLEAN, nullable columns, and NULL bitmap encoding.
- Added variable-length record updates and page compaction support.
- Preserved decoding and operation of existing Storage V1 tables and rows.
- Storage V1 rows are not automatically migrated to V2.

## App

- Added display support for the SQL v2 scalar and NULL result values.
- Added rendering for advisory diagnostic suggestions and FixIt descriptions.
- Preserved the existing CLI and unavailable-real-modules modes.

## Diagnostics

- Removed `CompileErrorKind` in favor of the single `CompileStage` contract.
- Added deterministic keyword and identifier did-you-mean suggestions.
- Added edit-distance candidate ranking with stable tie handling.
- Added advisory FixIts for supported punctuation and keyword corrections.
- FixIts are never applied automatically.

## Script Recovery

- Compilation and analysis continue after the first statement error.
- Execution stops at the first failed statement.
- Effects of earlier successful statements remain; no rollback is performed.
- A shadow catalog supports later analysis without mutating persistent storage.
- Compiler diagnostic ranges are statement-relative and Core converts them to
  absolute script coordinates; split-statement ranges are absolute already.

## Compatibility

- Existing Storage V1 metadata and rows remain readable.
- V1 rows are not automatically rewritten or migrated.
- Storage V2 supports the SQL v2 type and nullability contract.
- V1 and V2 tables can coexist and participate in supported queries.
- There is no Storage V3 format.
- UPDATE validates the requested changes before applying them, but it is not
  transactional and is not crash-atomic. tinydbms still has no WAL.

## Testing and fuzzing

- Fresh Debug: 52/52 tests passed.
- Fresh Release: 52/52 tests passed.
- Real Modules enabled: 53/53 tests passed.
- Strict GCC warnings: zero.
- Feature fuzz suites cover scalar types, NULL, UPDATE, ORDER BY, JOIN,
  aggregates, diagnostics, and recovery with fixed reproducible seeds.
- Release stress seed `20260929` passed 500 scripts and 11,969 statements,
  including 230 injected errors and 115 shadow CREATE statements.
- Mixed Storage V1/V2 persistence is tested across close and reopen cycles.

## Known limitations

The following are outside the frozen SQL v2 scope and are not implemented:

- HAVING
- DISTINCT
- column or table aliases
- LEFT, RIGHT, and FULL OUTER JOIN
- subqueries
- UNION and other set operations
- LIMIT and OFFSET
- window functions
- arithmetic expressions
- transactions
- write-ahead logging and crash recovery
