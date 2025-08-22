# RFC: JDBC Table Predicate Pushdown in Apache Impala

## Overview
Enable predicate pushdown for JDBC-backed tables to reduce data transfer and improve query performance by applying filters at the source database when feasible and safe.

## Goals
- Push simple conjunctive predicates (e.g., `=`, `!=`, `>`, `>=`, `<`, `<=`, `BETWEEN`, `IN`) to JDBC sources.
- Respect source capabilities and type semantics.
- Preserve correctness; fall back when pushdown is unsafe.

## Non-Goals (initial phase)
- Complex expressions requiring Impala-specific functions.
- Cross-source predicate pushdown beyond a single JDBC table scan.

## Design Sketch
- Capability detection per JDBC dialect/driver.
- Expression analyzer to translate Impala conjuncts into source SQL with parameters.
- Planner changes in the JDBC scan node to annotate pushable predicates and residuals.
- Metrics and debug flags to observe pushdown decisions.

## Risks and Mitigations
- Type mismatches and timezone issues → explicit cast strategies, integration tests per type.
- SQL injection concerns → prepared statements with bound parameters only.
- Driver-specific quirks → per-dialect adapters with allow-lists.

## Testing
- Add planner unit tests for pushdown decisions.
- End-to-end tests against a reference JDBC source (e.g., Postgres/MySQL) in CI.

## Rollout
- Feature flag gated; disabled by default initially.
- Incremental dialect enablement.

