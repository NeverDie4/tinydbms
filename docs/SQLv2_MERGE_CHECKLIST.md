# tinydbms SQL v2 Merge Checklist

## Diff review

- [ ] Review `git status --short`.
- [ ] Review every untracked file and confirm it belongs to SQL v2.
- [ ] Review public contract changes in `common.hpp`, `compiler.hpp`,
      `core.hpp`, and `storage.hpp`.
- [ ] Confirm unsupported features remain outside the frozen SQL v2 scope.
- [ ] Confirm no build output, database, log, or temporary file is included.
- [ ] Run and review the secret scan.

## Module review

- [ ] Inspect Storage V1/V2 metadata and record compatibility.
- [ ] Inspect UPDATE record-page and heap-table changes.
- [ ] Inspect query-wide SlotId allocation and SlotRow invariants.
- [ ] Inspect JOIN, ORDER BY, and aggregate execution boundaries.
- [ ] Inspect diagnostics, SourceRange, suggestions, and FixIt handling.
- [ ] Confirm FixIts are advisory and never automatically applied.
- [ ] Inspect script recovery and shadow-catalog isolation.
- [ ] Confirm recovery keeps earlier effects, stops execution after the first
      failure, and does not promise rollback.

## Verification

- [ ] Run the full Debug build and test suite.
- [ ] Run the full Release build and test suite.
- [ ] Run the Real Modules ON build and test suite.
- [ ] Confirm strict compiler warning count is zero.
- [ ] Confirm valid, invalid, diagnostics, and recovery fuzz case counts.
- [ ] Confirm release stress seed `20260929` passes.
- [ ] Run `git diff --check`.
- [ ] Review the proposed commit sequence and targeted tests.

## Git and review workflow

- [ ] Stage files according to the reviewed commit plan.
- [ ] Create the reviewed commits without mixing unrelated scopes.
- [ ] Push the feature branch.
- [ ] Open the pull request and complete human diff review.
- [ ] Merge only after all required checks and approvals pass.
