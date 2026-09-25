# ClickHouse writes — Phase 3 (UPDATE / DELETE / TRUNCATE) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Translate DuckDB `DELETE`, `TRUNCATE` and `UPDATE` on attached ClickHouse tables into single ClickHouse statements:
- `DELETE FROM … WHERE`, a synchronous lightweight delete;
- `TRUNCATE TABLE`;
- `ALTER TABLE … UPDATE … WHERE` with `mutations_sync`.

Each statement reports the affected-row count from a `SELECT count()` run just before it.

**Architecture:**
- DuckDB calls the catalog's logical-level `PlanDelete(context, planner, LogicalDelete &)` / `PlanUpdate(…, LogicalUpdate &)` before planning the child. By then `ColumnBindingResolver` has run, so every expression is a `BoundReferenceExpression` indexing its child operator's output.
- `ClickhouseDml` walks the child chain (Projection / Filter operators ending at the ClickHouse `LogicalGet` of the target table). It translates:
  - the scan's pushed `table_filters`, with a *strict* filter translator;
  - every `LogicalFilter` expression, with the new allow-list translator `ClickhouseExpression`.
- An exact WHERE results, or the statement is rejected. Anything else is rejected too.
- `ClickhouseDmlOperator`, a one-shot source, runs the count and then the statement on one write connection.

**Tech Stack:** C++17, DuckDB v1.5.4, clickhouse-cpp 2.6.2 (patched), sqllogictest via `make smoke` against ClickHouse 25.8.

**Spec:** `docs/superpowers/specs/2026-09-24-clickhouse-writes-design.md`. This plan implements Phase 3 (§10.3); read §1 (D3, D6, D7), §2, §6, §8 and §9. Phases 1 and 2 are on the branch; see `docs/superpowers/plans/2026-09-24-clickhouse-writes-phase{1,2}.md`.

## Global Constraints

- **Branch:** work on `su/init`. Commit there; never push.
- **Statements** (spec §2):
  - `DELETE FROM ch.db.t WHERE p` → `DELETE FROM db.t WHERE p`, with the query setting `lightweight_deletes_sync=2`.
  - `DELETE FROM ch.db.t` with no WHERE, and `TRUNCATE ch.db.t` → `TRUNCATE TABLE db.t`.
  - `UPDATE ch.db.t SET c = e, … WHERE p` → `ALTER TABLE db.t UPDATE c = CAST(e AS <column's ClickHouse type>), … WHERE p`. It is sent with the query setting `mutations_sync=<ch_mutations_sync>`, and `WHERE 1` is used when there is no filter.
  - The affected-row count comes from `SELECT count() FROM db.t [WHERE p]`, run on the same connection just before the statement. It is not atomic with respect to concurrent writers.
- **Accepted plan shape** (spec §6): `LogicalDelete/LogicalUpdate → [LogicalProjection | LogicalFilter]* → LogicalGet`, where the get is a ClickHouse scan of the modified table. Anything else — joins, subqueries, `USING`, `UPDATE … FROM`, other tables, `RETURNING` — raises:

  `NotImplementedException("<UPDATE|DELETE> on ClickHouse tables must filter only the modified table with translatable expressions (<reason>); use clickhouse_execute() for anything else")`

  The spec says the error must name the statement. `RETURNING` keeps its own message, `RETURNING is not supported for ClickHouse tables`, which the Phase 1 tests already use.
- **Predicate = AND of:**
  - every `get.table_filters` entry, translated strictly. An `OPTIONAL_FILTER` is skipped, because it is a hint and DuckDB keeps the exact predicate in a `LogicalFilter`. A dynamic filter, a bloom filter, an OR with an untranslatable branch, or a filter on `rowid` rejects the statement.
  - every `LogicalFilter` expression, through `ClickhouseExpression`.

  **No part of a predicate is ever dropped.** If anything is untranslatable, the whole statement is rejected before anything runs.
- **Expression allow-list** (spec §6), in `ClickhouseExpression`:
  - column references, resolved through the chain to base column names (never `rowid`);
  - constants: `ClickhouseExpression::Literal`, which moves the DDL literal writer from Phase 2;
  - `= != < <= > >=`; `AND`, `OR`, `NOT`; `IS [NOT] NULL`;
  - `IN` / `NOT IN` with a constant, non-NULL list, and `BETWEEN`;
  - `+ - * / // %` (unary `-` too); `//` becomes `intDiv`;
  - `LIKE` / `ILIKE` / `NOT LIKE` / `NOT ILIKE` with a constant pattern. A backslash is doubled, because DuckDB has no default escape character;
  - `starts_with`/`prefix` → `startsWith`, `ends_with`/`suffix` → `endsWith`, `contains` on strings → `position(a, b) > 0`, `lower`/`upper` → `lowerUTF8`/`upperUTF8`, `length` on strings → `lengthUTF8`;
  - `coalesce`, `CASE WHEN`;
  - `CAST` (not `TRY_CAST`) to types with a reverse mapping (`ClickhouseDdlTypes::ToClickhouse`).

  Anything else rejects the statement.
- **Semantics:** translated DML follows ClickHouse semantics where they differ from DuckDB's (NaN comparisons, UUID ordering, integer division and division by zero, LIKE collation). The README documents this. SELECT pushdown stays exact-only and is not changed.
- **View-like engines** (View, MaterializedView, LiveView, WindowView, Dictionary) reject UPDATE and DELETE, with the same kind of message DROP and ALTER use.
- **New setting:** `ch_mutations_sync`, UBIGINT, default 2, must be 0, 1 or 2. It is sent as `mutations_sync` with UPDATE.
- **Writes go through `ClickhouseCatalog::StartWrite`** (READ_ONLY check, marks the transaction written). The catalog's Plan* hooks call `ThrowIfReadOnly()` first, because DuckDB checks READ_ONLY only after planning.
- **Errors:**
  - no `InternalException` on user-triggerable paths, because it invalidates the DuckDB instance;
  - server errors keep the `ClickHouse error <code> (<NAME>): …` format;
  - passwords never appear.
- **Every src/ object library** keeps `target_compile_features(<lib> PUBLIC cxx_std_17)`.
- **Tests:**
  - Each file creates and drops its own ClickHouse database (`w_dml`, `w_dml2`), and never modifies `test_db` / `other_db`.
  - Every DML is checked through DuckDB and through `clickhouse_query` (ClickHouse's own view), with the affected-row counts asserted.
- **Commit trailer:** use the Co-Authored-By trailer your own harness instructs you to use.

## Build & test cheat sheet

```bash
cd /Users/redox/dev/altertable-ai/duckdb-clickhouse
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release && make smoke ARGS=test/sql/write/dml_delete.test   # focused
make smoke                                                        # all tests, fresh ClickHouse 25.8 container
cmake --build build/debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'   # debug (use cmake --build: make debug once kept a stale object)
./build/release/test/unittest "test/*"                            # no-server suite
CLICKHOUSE_TEST_KEEP=1 make smoke ARGS=...                        # keep the container; docker rm -f clickhouse-scanner-test after
```
- Run long builds and tests in the background and poll every few minutes, printing progress. An agent that is silent for 10 minutes is killed.
- Check Docker with `timeout 20 docker info`. Never restart Docker Desktop. Remove only a leftover `clickhouse-scanner-test` container.

**Expected-output rule:** if a value differs only in formatting, fix the expectation. If the value itself is different, fix the code. List every such change in your report.

## File Structure

```
src/include/clickhouse_expression.hpp, src/clickhouse_expression.cpp   NEW  ClickhouseExpression::Translate / Literal
src/include/storage/clickhouse_dml.hpp, src/storage/clickhouse_dml.cpp  NEW  ClickhouseDml (plan analysis) + ClickhouseDmlOperator
src/include/clickhouse_filter_pushdown.hpp, src/clickhouse_filter_pushdown.cpp  MOD  TransformFilterStrict
src/storage/clickhouse_ddl.cpp                            MOD  DEFAULT literal via ClickhouseExpression::Literal (Task 1)
src/include/storage/clickhouse_table_entry.hpp, src/storage/clickhouse_table_entry.cpp  MOD  IsViewLike()/ViewLikeKind() moved here
src/storage/clickhouse_schema_entry.cpp                   MOD  use the moved helpers
src/include/storage/clickhouse_catalog.hpp, src/storage/clickhouse_catalog.cpp  MOD  logical PlanDelete (T1) / PlanUpdate (T2)
src/clickhouse_scanner_extension.cpp                      MOD  ch_mutations_sync (T2)
src/CMakeLists.txt, src/storage/CMakeLists.txt            MOD  new sources (T1)
test/sql/write/dml_delete.test (T1), dml_update.test (T2)   NEW
test/sql/write/unsupported.test                           MOD  drop the DELETE (T1) / UPDATE (T2) blocks
README.md                                                 MOD
```

## Task Order
1. Expression translator, strict filters, DML analysis and operator: DELETE and TRUNCATE.
2. UPDATE and `ch_mutations_sync`.

---

### Task 1: DELETE and TRUNCATE (with the expression translator and the DML machinery)

**Files:**
- Create: `src/include/clickhouse_expression.hpp`, `src/clickhouse_expression.cpp`
- Create: `src/include/storage/clickhouse_dml.hpp`, `src/storage/clickhouse_dml.cpp`
- Modify: `src/include/clickhouse_filter_pushdown.hpp`, `src/clickhouse_filter_pushdown.cpp`
- Modify: `src/storage/clickhouse_ddl.cpp`
- Modify: `src/include/storage/clickhouse_table_entry.hpp`, `src/storage/clickhouse_table_entry.cpp`, `src/storage/clickhouse_schema_entry.cpp`
- Modify: `src/include/storage/clickhouse_catalog.hpp`, `src/storage/clickhouse_catalog.cpp`
- Modify: `src/CMakeLists.txt`, `src/storage/CMakeLists.txt`
- Modify: `test/sql/write/unsupported.test`
- Create: `test/sql/write/dml_delete.test`
- Modify: `README.md`

**Interfaces:**
- Consumes (Phases 1–2):
  - `ClickhouseCatalog::StartWrite`, `ThrowIfReadOnly`, `GetAttachedDatabase(context, name, fn)`
  - `ClickhouseConnection::Query(sql) → vector<clickhouse::Block>`, `Execute(sql, query_settings)`
  - `ClickhouseFilterPushdown::TransformFilter` / `TransformConstant`
  - `ClickhouseDdlTypes::ToClickhouse`
  - `ClickhouseScanBindData` (`columns`, `database`, `table`, `table_entry`), `ClickhouseScanFunction::IsClickhouseScan(name)`
  - `ClickhouseTableEntry::GetEngine()`, `GetClickhouseColumns()`
  - the static `IsViewLikeEngine` / `ViewLikeKind` helpers in `clickhouse_schema_entry.cpp`
- Produces:
  - `static string ClickhouseExpression::Translate(const Expression &expr, const std::function<string(idx_t)> &resolve_reference)`
  - `static string ClickhouseExpression::Literal(const Value &value)`
  - `static string ClickhouseFilterPushdown::TransformFilterStrict(const string &column, const TableFilter &filter)`
  - `bool ClickhouseTableEntry::IsViewLike() const`, `string ClickhouseTableEntry::ViewLikeKind() const`
  - `struct ClickhouseDmlStatement { string catalog_name; string description; string count_sql; string sql; vector<std::pair<string, string>> settings; }`
  - `struct ClickhouseDmlTarget { ClickhouseTableEntry &table; string qualified_name; string predicate; }`, where the predicate is empty for "every row"
  - `static ClickhouseDmlTarget ClickhouseDml::AnalyzeTarget(const string &statement, TableCatalogEntry &table, LogicalOperator &child)`
  - `static string ClickhouseDml::ResolveOutput(const LogicalOperator &op, idx_t index)`
  - `static ClickhouseDmlStatement ClickhouseDml::PlanDelete(LogicalDelete &op)`
  - `class ClickhouseDmlOperator` (a one-shot source)

- [ ] **Step 1: Write the failing test**

In `test/sql/write/unsupported.test`, **delete** the `DELETE FROM ch.test_db.t1;` block. After this task it would really truncate the shared fixture table.

`test/sql/write/dml_delete.test`:
```
# name: test/sql/write/dml_delete.test
# description: DELETE and TRUNCATE on attached ClickHouse tables, translated into single ClickHouse statements
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_dml');

statement ok
CALL clickhouse_execute('ch', 'CREATE DATABASE w_dml');

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_dml.t (id Int32, name String, v Nullable(Float64), tag Enum8(''a'' = 1, ''b'' = 2)) ENGINE = MergeTree ORDER BY id');

statement ok
INSERT INTO ch.w_dml.t SELECT range::INTEGER, 'n' || range, CASE WHEN range % 3 = 0 THEN NULL ELSE range * 1.5 END,
    CASE WHEN range % 2 = 0 THEN 'a' ELSE 'b' END FROM range(10);

# ---------------------------------------------------------------------------
# DELETE … WHERE, with the affected-row count
# ---------------------------------------------------------------------------
# a filter pushed into the scan (table_filters)
query I
DELETE FROM ch.w_dml.t WHERE id = 3;
----
1

# a non-dense IN list becomes an OPTIONAL_FILTER hint plus a LogicalFilter: nothing may be dropped
query I
DELETE FROM ch.w_dml.t WHERE id IN (1, 5, 9);
----
3

query I
SELECT list(id ORDER BY id) FROM ch.w_dml.t;
----
[0, 2, 4, 6, 7, 8]

# a filter on a type the scan does not push (Float64): stays in a LogicalFilter
query I
DELETE FROM ch.w_dml.t WHERE v > 10.0;
----
2

# functions, LIKE, OR
query I
DELETE FROM ch.w_dml.t WHERE lower(name) LIKE 'n2%' OR starts_with(name, 'n6');
----
2

# IS NULL, enum comparison, BETWEEN, arithmetic
query I
DELETE FROM ch.w_dml.t WHERE v IS NULL AND tag = 'a' AND id + 1 BETWEEN 1 AND 5;
----
1

query I
SELECT list(id ORDER BY id) FROM ch.w_dml.t;
----
[4]

# ClickHouse's own view agrees
query I
SELECT * FROM clickhouse_query('ch', 'SELECT groupArray(id) FROM (SELECT id FROM w_dml.t ORDER BY id)');
----
[4]

query I
DELETE FROM ch.w_dml.t WHERE id = 100;
----
0

# the same with filter pushdown off: the predicate arrives as a LogicalFilter only
statement ok
SET ch_filter_pushdown = false;

query I
DELETE FROM ch.w_dml.t WHERE id = 4 AND name = 'n4';
----
1

statement ok
RESET ch_filter_pushdown;

# ---------------------------------------------------------------------------
# DELETE without WHERE and TRUNCATE -> TRUNCATE TABLE
# ---------------------------------------------------------------------------
statement ok
INSERT INTO ch.w_dml.t SELECT range::INTEGER, 'n' || range, NULL, 'a' FROM range(5);

query I
DELETE FROM ch.w_dml.t;
----
5

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM w_dml.t');
----
0

statement ok
INSERT INTO ch.w_dml.t SELECT range::INTEGER, 'n' || range, NULL, 'a' FROM range(3);

query I
TRUNCATE ch.w_dml.t;
----
3

query I
SELECT count(*) FROM ch.w_dml.t;
----
0

# ---------------------------------------------------------------------------
# rejected: nothing runs, nothing changes
# ---------------------------------------------------------------------------
statement ok
INSERT INTO ch.w_dml.t SELECT range::INTEGER, 'n' || range, range * 1.0, 'b' FROM range(4);

statement error
DELETE FROM ch.w_dml.t WHERE md5(name) = 'x';
----
DELETE on ClickHouse tables must filter only the modified table with translatable expressions

statement error
DELETE FROM ch.w_dml.t WHERE id IN (SELECT 1);
----
DELETE on ClickHouse tables must filter only the modified table with translatable expressions

statement error
DELETE FROM ch.w_dml.t USING (SELECT 1 AS k) s WHERE t.id = s.k;
----
DELETE on ClickHouse tables must filter only the modified table with translatable expressions

# an untranslatable branch of an OR must not be dropped (that would widen the DELETE)
statement error
DELETE FROM ch.w_dml.t WHERE id = 0 OR md5(name) = 'x';
----
DELETE on ClickHouse tables must filter only the modified table with translatable expressions

# NOT IN with a NULL differs between DuckDB and ClickHouse: rejected
statement error
DELETE FROM ch.w_dml.t WHERE id NOT IN (1, NULL);
----
DELETE on ClickHouse tables must filter only the modified table with translatable expressions

statement error
DELETE FROM ch.w_dml.t WHERE id = 0 RETURNING id;
----
RETURNING is not supported for ClickHouse tables

query I
SELECT count(*) FROM ch.w_dml.t;
----
4

# ---------------------------------------------------------------------------
# views, engines without lightweight DELETE, transactions
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE VIEW w_dml.v AS SELECT 1 AS one');

statement error
DELETE FROM ch.w_dml.v WHERE one = 1;
----
is a ClickHouse view

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_dml.mem (id Int32) ENGINE = Memory');

statement ok
INSERT INTO ch.w_dml.mem VALUES (1), (2);

statement error
DELETE FROM ch.w_dml.mem WHERE id = 1;
----
ClickHouse error

statement ok
CALL enable_logging(level = 'warning');

statement ok
BEGIN;

query I
DELETE FROM ch.w_dml.t WHERE id = 0;
----
1

statement ok
ROLLBACK;

query I
SELECT count(*) FROM ch.w_dml.t;
----
3

query I
SELECT count(*) FROM duckdb_logs WHERE message LIKE '%cannot be rolled back (database "ch")%';
----
1

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE w_dml');
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/dml_delete.test`
Expected: FAIL at the first DELETE with `DELETE is not supported on attached ClickHouse databases yet`.

- [ ] **Step 3: Move the view-like helpers onto the table entry**

In `src/storage/clickhouse_schema_entry.cpp`, the static `IsViewLikeEngine` / `ViewLikeKind` helpers become public methods of `ClickhouseTableEntry`:
```cpp
	//! ClickHouse engines that are not tables (View, MaterializedView, LiveView, WindowView, Dictionary): DROP TABLE,
	//! ALTER TABLE, UPDATE and DELETE refuse them
	bool IsViewLike() const;
	//! "dictionary" for the Dictionary engine, "view" for the other view-like engines
	string ViewLikeKind() const;
```
Keep the bodies the same. `DropEntry` and `Alter` call the methods. Keep their messages unchanged: the Phase 2 tests assert them.

- [ ] **Step 4: `ClickhouseExpression`**

`src/include/clickhouse_expression.hpp`:
```cpp
#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"

#include <functional>

namespace duckdb {
class Expression;

//! Translates bound DuckDB expressions into ClickHouse SQL through an allow-list (UPDATE/DELETE predicates and SET
//! values). Throws NotImplementedException for anything outside it: no expression is ever partially translated
class ClickhouseExpression {
public:
	//! `resolve_reference` maps a BoundReferenceExpression index (into the output of the operator the expression
	//! is evaluated over) to ClickHouse SQL, e.g. a quoted column name
	static string Translate(const Expression &expr, const std::function<string(idx_t)> &resolve_reference);
	//! A constant as a ClickHouse literal. Throws NotImplementedException for types without one
	static string Literal(const Value &value);
};

} // namespace duckdb
```
`src/clickhouse_expression.cpp`:
```cpp
#include "clickhouse_expression.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_filter_pushdown.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/list.hpp"

#include <cmath>

namespace duckdb {

[[noreturn]] static void ThrowUntranslatable(const Expression &expr) {
	throw NotImplementedException("untranslatable expression: %s", expr.ToString());
}

string ClickhouseExpression::Literal(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	switch (value.type().id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto number = value.GetValue<double>();
		if (std::isnan(number)) {
			return "nan";
		}
		if (std::isinf(number)) {
			return number > 0 ? "inf" : "-inf";
		}
		return value.ToString();
	}
	case LogicalTypeId::UUID:
		return "toUUID(" + ClickhouseUtils::QuoteLiteral(value.ToString()) + ")";
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		// naive timestamps are stored as UTC (see ClickhouseDdlTypes)
		return ClickhouseFilterPushdown::TransformConstant(
		    Value::TIMESTAMPTZ(timestamp_tz_t(value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>())));
	default:
		return ClickhouseFilterPushdown::TransformConstant(value);
	}
}

static string ComparisonOperator(const Expression &expr) {
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "!=";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		ThrowUntranslatable(expr);
	}
}

//! A constant LIKE pattern: DuckDB has no default escape character, ClickHouse treats backslash as one
static string LikePattern(const Expression &pattern) {
	if (pattern.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		ThrowUntranslatable(pattern);
	}
	auto &value = pattern.Cast<BoundConstantExpression>().value;
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		ThrowUntranslatable(pattern);
	}
	return ClickhouseUtils::QuoteLiteral(StringUtil::Replace(StringValue::Get(value), "\\", "\\\\"));
}

static string TranslateFunction(const BoundFunctionExpression &function,
                                const std::function<string(idx_t)> &resolve) {
	auto &name = function.function.name;
	auto &children = function.children;
	auto arg = [&](idx_t i) {
		return ClickhouseExpression::Translate(*children[i], resolve);
	};
	auto is_string = [&](idx_t i) {
		return children[i]->return_type.id() == LogicalTypeId::VARCHAR;
	};
	if (children.size() == 2 && (name == "+" || name == "-" || name == "*" || name == "/" || name == "%")) {
		return "(" + arg(0) + " " + name + " " + arg(1) + ")";
	}
	if (children.size() == 1 && name == "-") {
		return "(-" + arg(0) + ")";
	}
	if (children.size() == 2 && name == "//") {
		return "intDiv(" + arg(0) + ", " + arg(1) + ")";
	}
	if (children.size() == 2 && (name == "~~" || name == "!~~" || name == "~~*" || name == "!~~*")) {
		auto negated = name[0] == '!';
		auto keyword = StringUtil::EndsWith(name, "*") ? "ILIKE" : "LIKE";
		return "(" + arg(0) + (negated ? " NOT " : " ") + keyword + " " + LikePattern(*children[1]) + ")";
	}
	if (children.size() == 2 && (name == "starts_with" || name == "prefix")) {
		return "startsWith(" + arg(0) + ", " + arg(1) + ")";
	}
	if (children.size() == 2 && (name == "ends_with" || name == "suffix")) {
		return "endsWith(" + arg(0) + ", " + arg(1) + ")";
	}
	if (children.size() == 2 && name == "contains" && is_string(0) && is_string(1)) {
		return "(position(" + arg(0) + ", " + arg(1) + ") > 0)";
	}
	if (children.size() == 1 && (name == "lower" || name == "lcase")) {
		return "lowerUTF8(" + arg(0) + ")";
	}
	if (children.size() == 1 && (name == "upper" || name == "ucase")) {
		return "upperUTF8(" + arg(0) + ")";
	}
	if (children.size() == 1 && (name == "length" || name == "len") && is_string(0)) {
		return "lengthUTF8(" + arg(0) + ")";
	}
	ThrowUntranslatable(function);
}

static string TranslateOperator(const BoundOperatorExpression &op, const std::function<string(idx_t)> &resolve) {
	auto arg = [&](idx_t i) {
		return ClickhouseExpression::Translate(*op.children[i], resolve);
	};
	switch (op.GetExpressionType()) {
	case ExpressionType::OPERATOR_NOT:
		return "(NOT " + arg(0) + ")";
	case ExpressionType::OPERATOR_IS_NULL:
		return "(" + arg(0) + " IS NULL)";
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		return "(" + arg(0) + " IS NOT NULL)";
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN: {
		vector<string> values;
		for (idx_t i = 1; i < op.children.size(); i++) {
			auto &child = *op.children[i];
			// NULLs in the list: DuckDB yields NULL where ClickHouse (transform_null_in=0) yields 0/1, which flips
			// NOT IN; only constant, non-NULL lists translate exactly
			if (child.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
			    child.Cast<BoundConstantExpression>().value.IsNull()) {
				ThrowUntranslatable(op);
			}
			values.push_back(arg(i));
		}
		auto keyword = op.GetExpressionType() == ExpressionType::COMPARE_IN ? " IN (" : " NOT IN (";
		return "(" + arg(0) + keyword + StringUtil::Join(values, ", ") + "))";
	}
	case ExpressionType::OPERATOR_COALESCE: {
		vector<string> values;
		for (idx_t i = 0; i < op.children.size(); i++) {
			values.push_back(arg(i));
		}
		return "coalesce(" + StringUtil::Join(values, ", ") + ")";
	}
	default:
		ThrowUntranslatable(op);
	}
}

string ClickhouseExpression::Translate(const Expression &expr, const std::function<string(idx_t)> &resolve) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
		return resolve(expr.Cast<BoundReferenceExpression>().index);
	case ExpressionClass::BOUND_CONSTANT:
		try {
			return Literal(expr.Cast<BoundConstantExpression>().value);
		} catch (NotImplementedException &) {
			ThrowUntranslatable(expr);
		}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comparison = expr.Cast<BoundComparisonExpression>();
		return "(" + Translate(*comparison.left, resolve) + " " + ComparisonOperator(expr) + " " +
		       Translate(*comparison.right, resolve) + ")";
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		vector<string> parts;
		for (auto &child : conjunction.children) {
			parts.push_back(Translate(*child, resolve));
		}
		auto separator = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? " AND " : " OR ";
		return "(" + StringUtil::Join(parts, separator) + ")";
	}
	case ExpressionClass::BOUND_OPERATOR:
		return TranslateOperator(expr.Cast<BoundOperatorExpression>(), resolve);
	case ExpressionClass::BOUND_BETWEEN: {
		auto &between = expr.Cast<BoundBetweenExpression>();
		auto input = Translate(*between.input, resolve);
		return "(" + input + (between.lower_inclusive ? " >= " : " > ") + Translate(*between.lower, resolve) +
		       " AND " + input + (between.upper_inclusive ? " <= " : " < ") + Translate(*between.upper, resolve) +
		       ")";
	}
	case ExpressionClass::BOUND_FUNCTION:
		return TranslateFunction(expr.Cast<BoundFunctionExpression>(), resolve);
	case ExpressionClass::BOUND_CAST: {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (cast.try_cast) {
			ThrowUntranslatable(expr);
		}
		string type;
		try {
			type = ClickhouseDdlTypes::ToClickhouse(cast.return_type, true);
		} catch (NotImplementedException &) {
			ThrowUntranslatable(expr);
		}
		return "CAST(" + Translate(*cast.child, resolve) + " AS " + type + ")";
	}
	case ExpressionClass::BOUND_CASE: {
		auto &case_expr = expr.Cast<BoundCaseExpression>();
		string sql = "(CASE";
		for (auto &check : case_expr.case_checks) {
			sql += " WHEN " + Translate(*check.when_expr, resolve) + " THEN " + Translate(*check.then_expr, resolve);
		}
		return sql + " ELSE " + Translate(*case_expr.else_expr, resolve) + " END)";
	}
	default:
		ThrowUntranslatable(expr);
	}
}

} // namespace duckdb
```
Things to adapt, keeping the behaviour, if they differ in v1.5.4:
- `BoundOperatorExpression` stores `NOT` as `OPERATOR_NOT`;
- the function names (`~~` etc.): check `duckdb/src/function/scalar/string/like.cpp` and friends;
- `planner/expression/list.hpp`.

Some forms DuckDB never produces after optimisation, e.g. `BETWEEN` may already be split. Keep those branches anyway, because they are correct.

In `src/storage/clickhouse_ddl.cpp`:
- Delete the file-local `LiteralSql`.
- In `DefaultValueSql`, call `ClickhouseExpression::Literal(value)`, wrapped in the same `try { … } catch (NotImplementedException &) { throw NotImplementedException("DEFAULT value %s of column \"%s\" (type %s) cannot be written as a ClickHouse literal; …") }`. The message stays byte-for-byte, because Phase 2's ddl_table.test asserts it.

- [ ] **Step 5: Strict table-filter translation**

Add to `ClickhouseFilterPushdown` (header and .cpp):
```cpp
	//! For UPDATE/DELETE: like TransformFilter, but never narrows a predicate by dropping a part. Returns "" only for
	//! an OPTIONAL_FILTER (a hint: DuckDB keeps its exact predicate in a LogicalFilter above the scan), which the
	//! caller skips; throws NotImplementedException for anything it cannot translate exactly
	static string TransformFilterStrict(const string &column, const TableFilter &filter);
```
```cpp
string ClickhouseFilterPushdown::TransformFilterStrict(const string &column, const TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::OPTIONAL_FILTER:
		return string();
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::BLOOM_FILTER:
		throw NotImplementedException("runtime filter %s", EnumUtil::ToString(filter.filter_type));
	case TableFilterType::CONJUNCTION_AND: {
		vector<string> parts;
		for (auto &child : filter.Cast<ConjunctionAndFilter>().child_filters) {
			auto part = TransformFilterStrict(column, *child);
			if (!part.empty()) {
				parts.push_back(part);
			}
		}
		return parts.empty() ? string() : "(" + StringUtil::Join(parts, " AND ") + ")";
	}
	case TableFilterType::CONJUNCTION_OR: {
		vector<string> parts;
		for (auto &child : filter.Cast<ConjunctionOrFilter>().child_filters) {
			auto part = TransformFilterStrict(column, *child);
			if (part.empty()) {
				// dropping a branch of an OR would narrow it; dropping the whole OR would widen the statement
				throw NotImplementedException("OR filter with a non-exact branch");
			}
			parts.push_back(part);
		}
		return "(" + StringUtil::Join(parts, " OR ") + ")";
	}
	default: {
		auto sql = TransformFilter(column, filter);
		if (sql.empty()) {
			throw NotImplementedException("filter %s", EnumUtil::ToString(filter.filter_type));
		}
		return sql;
	}
	}
}
```

- [ ] **Step 6: `ClickhouseDml` and `ClickhouseDmlOperator`**

`src/include/storage/clickhouse_dml.hpp`:
```cpp
#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class ClickhouseTableEntry;
class LogicalDelete;
class TableCatalogEntry;

//! A DELETE / TRUNCATE / UPDATE translated into ClickHouse SQL
struct ClickhouseDmlStatement {
	//! The attached database (DuckDB catalog), resolved again when the statement runs
	string catalog_name;
	//! For errors and EXPLAIN, e.g. DELETE FROM `db`.`t`
	string description;
	//! SELECT count() FROM `db`.`t` [WHERE p]: the affected-row count
	string count_sql;
	string sql;
	//! Query-level settings sent with `sql`, e.g. lightweight_deletes_sync
	vector<std::pair<string, string>> settings;
};

//! The modified table and the exact ClickHouse WHERE predicate of an UPDATE/DELETE plan
struct ClickhouseDmlTarget {
	ClickhouseTableEntry &table;
	//! `db`.`t`
	string qualified_name;
	//! Empty: every row
	string predicate;
};

class ClickhouseDml {
public:
	//! Checks the plan below a LogicalDelete/LogicalUpdate: [LogicalProjection | LogicalFilter]* ending at a
	//! ClickHouse scan of `table`. Collects the exact predicate. Throws NotImplementedException ("<statement> on
	//! ClickHouse tables must filter only the modified table with translatable expressions (…); …") otherwise
	static ClickhouseDmlTarget AnalyzeTarget(const string &statement, TableCatalogEntry &table,
	                                         LogicalOperator &child);
	//! ClickHouse SQL for output column `index` of `op` (a LogicalGet, LogicalFilter or LogicalProjection)
	static string ResolveOutput(const LogicalOperator &op, idx_t index);
	static ClickhouseDmlStatement PlanDelete(LogicalDelete &op);
	//! Throws the "must filter only the modified table" error for `statement`, with `reason`
	[[noreturn]] static void ThrowUnsupportedShape(const string &statement, const string &reason);
};

//! Runs a ClickhouseDmlStatement once: the count, then the statement, on one write connection. Emits the count
class ClickhouseDmlOperator : public PhysicalOperator {
public:
	ClickhouseDmlOperator(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseDmlStatement statement);

	ClickhouseDmlStatement statement;

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return false;
	}
	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
```
`src/storage/clickhouse_dml.cpp`:
```cpp
#include "storage/clickhouse_dml.hpp"

#include "clickhouse_expression.hpp"
#include "clickhouse_filter_pushdown.hpp"
#include "clickhouse_scanner.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

void ClickhouseDml::ThrowUnsupportedShape(const string &statement, const string &reason) {
	throw NotImplementedException("%s on ClickHouse tables must filter only the modified table with translatable "
	                              "expressions (%s); use clickhouse_execute() for anything else",
	                              statement, reason);
}

static const ClickhouseScanBindData &ScanBindData(const LogicalGet &get) {
	return get.bind_data->Cast<ClickhouseScanBindData>();
}

string ClickhouseDml::ResolveOutput(const LogicalOperator &op, idx_t index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		auto position = get.projection_ids.empty() ? index : get.projection_ids[index];
		auto column_id = get.GetColumnIds()[position].GetPrimaryIndex();
		auto &bind_data = ScanBindData(get);
		if (column_id >= bind_data.columns.size()) {
			// rowid (always NULL for ClickHouse tables) or another virtual column
			throw NotImplementedException("reference to a virtual column");
		}
		return ClickhouseUtils::QuoteIdentifier(bind_data.columns[column_id].name);
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		auto child_index = filter.projection_map.empty() ? index : filter.projection_map[index];
		return ResolveOutput(*op.children[0], child_index);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &child = *op.children[0];
		return "(" +
		       ClickhouseExpression::Translate(*op.expressions[index],
		                                       [&](idx_t i) { return ResolveOutput(child, i); }) +
		       ")";
	}
	default:
		throw NotImplementedException("operator %s", LogicalOperatorToString(op.type));
	}
}

ClickhouseDmlTarget ClickhouseDml::AnalyzeTarget(const string &statement, TableCatalogEntry &table,
                                                 LogicalOperator &child) {
	auto &ch_table = table.Cast<ClickhouseTableEntry>();
	if (ch_table.IsViewLike()) {
		throw NotImplementedException("\"%s\" is a ClickHouse %s (engine %s), which %s does not modify; run the "
		                              "statement with clickhouse_execute() instead",
		                              ch_table.name, ch_table.ViewLikeKind(), ch_table.GetEngine(), statement);
	}
	vector<string> conditions;
	try {
		vector<reference<LogicalFilter>> filters;
		reference<LogicalOperator> current = child;
		while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION ||
		       current.get().type == LogicalOperatorType::LOGICAL_FILTER) {
			if (current.get().children.size() != 1) {
				throw NotImplementedException("operator with several inputs");
			}
			if (current.get().type == LogicalOperatorType::LOGICAL_FILTER) {
				filters.push_back(current.get().Cast<LogicalFilter>());
			}
			current = *current.get().children[0];
		}
		if (current.get().type != LogicalOperatorType::LOGICAL_GET) {
			throw NotImplementedException("operator %s", LogicalOperatorToString(current.get().type));
		}
		auto &get = current.get().Cast<LogicalGet>();
		if (!ClickhouseScanFunction::IsClickhouseScan(get.function.name) ||
		    ScanBindData(get).table_entry != &table) {
			throw NotImplementedException("reads another table");
		}
		auto &column_ids = get.GetColumnIds();
		auto &bind_data = ScanBindData(get);
		for (auto &entry : get.table_filters.filters) {
			auto column_id = column_ids[entry.first].GetPrimaryIndex();
			if (column_id >= bind_data.columns.size()) {
				throw NotImplementedException("filter on a virtual column");
			}
			auto sql = ClickhouseFilterPushdown::TransformFilterStrict(
			    ClickhouseUtils::QuoteIdentifier(bind_data.columns[column_id].name), *entry.second);
			if (!sql.empty()) {
				conditions.push_back(sql);
			}
		}
		for (auto &filter : filters) {
			auto &filter_child = *filter.get().children[0];
			for (auto &expr : filter.get().expressions) {
				conditions.push_back(ClickhouseExpression::Translate(
				    *expr, [&](idx_t i) { return ResolveOutput(filter_child, i); }));
			}
		}
	} catch (NotImplementedException &ex) {
		ErrorData error(ex);
		ThrowUnsupportedShape(statement, error.RawMessage());
	}
	return ClickhouseDmlTarget {ch_table,
	                            ClickhouseUtils::QuoteIdentifier(ch_table.schema.name) + "." +
	                                ClickhouseUtils::QuoteIdentifier(ch_table.name),
	                            StringUtil::Join(conditions, " AND ")};
}

ClickhouseDmlStatement ClickhouseDml::PlanDelete(LogicalDelete &op) {
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING is not supported for ClickHouse tables");
	}
	auto target = AnalyzeTarget("DELETE", op.table, *op.children[0]);
	ClickhouseDmlStatement result;
	result.catalog_name = op.table.catalog.GetName();
	result.count_sql = "SELECT count() FROM " + target.qualified_name;
	if (target.predicate.empty()) {
		result.description = "TRUNCATE TABLE " + target.qualified_name;
		result.sql = result.description;
	} else {
		result.count_sql += " WHERE " + target.predicate;
		result.description = "DELETE FROM " + target.qualified_name;
		result.sql = result.description + " WHERE " + target.predicate;
		// lightweight DELETE, synchronous (spec D3)
		result.settings.emplace_back("lightweight_deletes_sync", "2");
	}
	return result;
}

ClickhouseDmlOperator::ClickhouseDmlOperator(PhysicalPlan &physical_plan, LogicalOperator &op,
                                             ClickhouseDmlStatement statement_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      statement(std::move(statement_p)) {
}

SourceResultType ClickhouseDmlOperator::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                        OperatorSourceInput &input) const {
	auto &catalog = ClickhouseCatalog::GetAttachedDatabase(context.client, statement.catalog_name,
	                                                       statement.description);
	auto connection = catalog.StartWrite(context.client);
	uint64_t count = 0;
	for (auto &block : connection->Query(statement.count_sql)) {
		if (block.GetRowCount() > 0) {
			count = block[0]->As<clickhouse::ColumnUInt64>()->At(0);
		}
	}
	connection->Execute(statement.sql, statement.settings);
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(count)));
	return SourceResultType::FINISHED;
}

string ClickhouseDmlOperator::GetName() const {
	return "CLICKHOUSE_DML";
}

InsertionOrderPreservingMap<string> ClickhouseDmlOperator::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Statement"] = statement.sql;
	return result;
}

} // namespace duckdb
```
Check each of these against the v1.5.4 headers:
- `LogicalGet::GetColumnIds()` returns `ColumnIndex`es with `GetPrimaryIndex()`, and the key type of `table_filters.filters`. The Phase 1 filter pushdown indexes `column_ids[entry.first]`; follow the same.
- `LogicalOperatorToString`.
- `ErrorData::RawMessage`.
- the `ClickhouseScanBindData` field names in `src/include/clickhouse_scanner.hpp`: `columns`, `table_entry`.

`#include <clickhouse/columns/numeric.h>` if `ColumnUInt64` isn't visible.

- [ ] **Step 7: Hook `PlanDelete`**

In `src/include/storage/clickhouse_catalog.hpp`:
- Replace the 4-argument `PlanDelete` override with the logical-level one:
```cpp
	//! Logical level: DuckDB calls this before planning the child, so the logical plan (filters and scan) is still
	//! there to be translated (see ClickhouseDml)
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) override;
```
- Keep the 4-argument `PlanUpdate` for now; Task 2 replaces it.

In `src/storage/clickhouse_catalog.cpp`, include `storage/clickhouse_dml.hpp` and `duckdb/planner/operator/logical_delete.hpp`:
```cpp
PhysicalOperator &ClickhouseCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                                LogicalDelete &op) {
	ThrowIfReadOnly();
	return planner.Make<ClickhouseDmlOperator>(op, ClickhouseDml::PlanDelete(op));
}
```
Add `clickhouse_expression.cpp` to `src/CMakeLists.txt` and `clickhouse_dml.cpp` to `src/storage/CMakeLists.txt`, in alphabetical order.

- [ ] **Step 8: README**

- In Limitations, reword the unsupported-yet bullet to: `` `UPDATE` is not supported yet; run it with `clickhouse_execute()`. ``
- In `## Writing`, add after the DDL paragraph:
```markdown
`DELETE` and `TRUNCATE` are translated into one ClickHouse statement each: `DELETE FROM … WHERE …` (a synchronous
lightweight delete, MergeTree family only) or, with no `WHERE`, `TRUNCATE TABLE`. The reported row count comes from a
`SELECT count()` run just before the statement, so it can be off if other clients write at the same time.

- The `WHERE` clause must only use the modified table's columns, constants, comparisons, `AND`/`OR`/`NOT`,
  `IS [NOT] NULL`, `IN`/`NOT IN` lists without `NULL`, `BETWEEN`, arithmetic, `LIKE`/`ILIKE`, `starts_with`,
  `ends_with`, `contains`, `lower`, `upper`, `length`, `coalesce`, `CASE` and `CAST`. Anything else — other
  functions, subqueries, `USING`, `RETURNING` — is rejected before anything runs; use `clickhouse_execute`.
- The translated statement follows ClickHouse semantics where they differ from DuckDB's (e.g. `NaN` comparisons,
  `UUID` ordering, integer division and division by zero).
```

- [ ] **Step 9: Run the tests to verify they pass**

Run `make release && make smoke`, then `cmake --build build/debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'`, then the no-server suite. Expected: all pass.

The `IN (1, 5, 9)` case must delete exactly 3 rows. If it deletes more, a predicate was dropped. Treat that as a blocker, never as an expectation to adjust.

- [ ] **Step 10: Commit**

```bash
git add -A src test README.md
git commit -m "feat: DELETE and TRUNCATE on attached ClickHouse tables

<your harness's Co-Authored-By trailer>"
```

---

### Task 2: UPDATE and `ch_mutations_sync`

**Files:**
- Modify: `src/include/storage/clickhouse_dml.hpp`, `src/storage/clickhouse_dml.cpp` (PlanUpdate)
- Modify: `src/include/storage/clickhouse_catalog.hpp`, `src/storage/clickhouse_catalog.cpp` (logical PlanUpdate)
- Modify: `src/clickhouse_scanner_extension.cpp` (ch_mutations_sync)
- Modify: `test/sql/write/unsupported.test`
- Create: `test/sql/write/dml_update.test`
- Modify: `README.md`

**Interfaces:**
- Consumes (Task 1):
  - `ClickhouseDml::AnalyzeTarget`, `ResolveOutput`, `ThrowUnsupportedShape`
  - `ClickhouseDmlStatement`, `ClickhouseDmlOperator`
  - `ClickhouseExpression::Translate`
  - `ClickhouseTableEntry::GetClickhouseColumns()` (`name`, `clickhouse_type`)
- Produces: `static ClickhouseDmlStatement ClickhouseDml::PlanUpdate(ClientContext &context, LogicalUpdate &op)`

- [ ] **Step 1: Write the failing test**

In `test/sql/write/unsupported.test`, **delete** the `UPDATE ch.test_db.t1 SET name = 'x';` block, which would now modify the fixture. If `unsupported.test` is left with no statement-error blocks that apply to writable attaches, keep it anyway for the permanent rejections (CREATE VIEW, CREATE INDEX, MERGE INTO, unsupported ALTER).

`test/sql/write/dml_update.test`:
```
# name: test/sql/write/dml_update.test
# description: UPDATE on attached ClickHouse tables, translated into ALTER TABLE … UPDATE
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_dml2');

statement ok
CALL clickhouse_execute('ch', 'CREATE DATABASE w_dml2');

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_dml2.u (id Int32, name String, n Nullable(Int64), ip IPv4, e Enum8(''x'' = 1, ''y'' = 2)) ENGINE = MergeTree ORDER BY id');

statement ok
INSERT INTO ch.w_dml2.u SELECT range::INTEGER, 'name' || range, range, '10.0.0.' || range, 'x' FROM range(5);

# several columns, expressions over the row, affected-row count
query I
UPDATE ch.w_dml2.u SET name = upper(name), n = n * 10 WHERE id >= 3;
----
2

query III
SELECT id, name, n FROM ch.w_dml2.u ORDER BY id;
----
0	name0	0
1	name1	1
2	name2	2
3	NAME3	30
4	NAME4	40

query I
SELECT * FROM clickhouse_query('ch', 'SELECT groupArray(n) FROM (SELECT n FROM w_dml2.u ORDER BY id)');
----
[0, 1, 2, 30, 40]

# NULL, coalesce, no WHERE
query I
UPDATE ch.w_dml2.u SET n = NULL WHERE id = 1;
----
1

query I
UPDATE ch.w_dml2.u SET n = coalesce(n, 0) + 1;
----
5

query I
SELECT list(n ORDER BY id) FROM ch.w_dml2.u;
----
[1, 1, 3, 31, 41]

# enums, server-converted types, CASE
query I
UPDATE ch.w_dml2.u SET e = 'y', ip = '192.168.1.' || id::VARCHAR WHERE e = 'x' AND id < 2;
----
2

query I
UPDATE ch.w_dml2.u SET name = CASE WHEN id % 2 = 0 THEN 'even' ELSE 'odd' END;
----
5

query IIII
SELECT * FROM clickhouse_query('ch', 'SELECT id, name, toString(e), toString(ip) FROM w_dml2.u WHERE id <= 2 ORDER BY id');
----
0	even	y	192.168.1.0
1	odd	y	192.168.1.1
2	even	x	10.0.0.2

# ClickHouse refuses to update a sorting-key column; the error is ClickHouse's
statement error
UPDATE ch.w_dml2.u SET id = 99 WHERE id = 1;
----
ClickHouse error

# rejected before anything runs
statement error
UPDATE ch.w_dml2.u SET name = md5(name);
----
UPDATE on ClickHouse tables must filter only the modified table with translatable expressions

statement error
UPDATE ch.w_dml2.u SET name = 'z' FROM (SELECT 1 AS k) s WHERE u.id = s.k;
----
UPDATE on ClickHouse tables must filter only the modified table with translatable expressions

statement error
UPDATE ch.w_dml2.u SET name = 'z' WHERE id = 1 RETURNING id;
----
RETURNING is not supported for ClickHouse tables

statement error
UPDATE ch.w_dml2.u SET n = DEFAULT;
----
UPDATE on ClickHouse tables must filter only the modified table with translatable expressions

query I
SELECT count(*) FROM ch.w_dml2.u WHERE name = 'z';
----
0

# ch_mutations_sync
statement error
SET ch_mutations_sync = 3;
----
ch_mutations_sync must be 0, 1 or 2

statement ok
SET ch_mutations_sync = 1;

query I
UPDATE ch.w_dml2.u SET name = 'synced' WHERE id = 4;
----
1

statement ok
RESET ch_mutations_sync;

query I
SELECT name FROM ch.w_dml2.u WHERE id = 4;
----
synced

# views are refused
statement ok
CALL clickhouse_execute('ch', 'CREATE VIEW w_dml2.v AS SELECT 1 AS one');

statement error
UPDATE ch.w_dml2.v SET one = 2;
----
is a ClickHouse view

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE w_dml2');
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/dml_update.test`
Expected: FAIL at the first UPDATE with `UPDATE is not supported on attached ClickHouse databases yet`.

- [ ] **Step 3: `ClickhouseDml::PlanUpdate`**

Add the declaration to the header, including `duckdb/planner/operator/logical_update.hpp` in the .cpp:
```cpp
	static ClickhouseDmlStatement PlanUpdate(ClientContext &context, LogicalUpdate &op);
```
```cpp
ClickhouseDmlStatement ClickhouseDml::PlanUpdate(ClientContext &context, LogicalUpdate &op) {
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING is not supported for ClickHouse tables");
	}
	auto target = AnalyzeTarget("UPDATE", op.table, *op.children[0]);
	auto &columns = target.table.GetClickhouseColumns();
	auto &child = *op.children[0];
	vector<string> assignments;
	try {
		for (idx_t i = 0; i < op.columns.size(); i++) {
			auto &column = columns[op.columns[i].index];
			auto &expr = *op.expressions[i];
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_DEFAULT) {
				throw NotImplementedException("SET %s = DEFAULT", column.name);
			}
			auto value = ClickhouseExpression::Translate(expr, [&](idx_t index) { return ResolveOutput(child, index); });
			auto quoted = ClickhouseUtils::QuoteIdentifier(column.name);
			if (value == quoted || value == "(" + quoted + ")") {
				// DuckDB can add unchanged columns (col = col) to an UPDATE; sending them would fail on key columns
				continue;
			}
			assignments.push_back(quoted + " = CAST(" + value + " AS " + column.clickhouse_type + ")");
		}
	} catch (NotImplementedException &ex) {
		ErrorData error(ex);
		ThrowUnsupportedShape("UPDATE", error.RawMessage());
	}
	ClickhouseDmlStatement result;
	result.catalog_name = op.table.catalog.GetName();
	auto where = target.predicate.empty() ? string("1") : target.predicate;
	result.count_sql = "SELECT count() FROM " + target.qualified_name + " WHERE " + where;
	result.description = "ALTER TABLE " + target.qualified_name + " UPDATE";
	if (assignments.empty()) {
		// nothing changes: count only
		result.sql.clear();
		return result;
	}
	result.sql = result.description + " " + StringUtil::Join(assignments, ", ") + " WHERE " + where;
	Value mutations_sync;
	string sync = "2";
	if (context.TryGetCurrentSetting("ch_mutations_sync", mutations_sync) && !mutations_sync.IsNull()) {
		sync = mutations_sync.ToString();
	}
	result.settings.emplace_back("mutations_sync", sync);
	return result;
}
```
Make `ClickhouseDmlOperator::GetDataInternal` skip `connection->Execute` when `statement.sql` is empty.

In the catalog, replace the 4-argument `PlanUpdate` override with the logical one:
```cpp
PhysicalOperator &ClickhouseCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                                LogicalUpdate &op) {
	ThrowIfReadOnly();
	return planner.Make<ClickhouseDmlOperator>(op, ClickhouseDml::PlanUpdate(context, op));
}
```
Update the header to match: the 3-argument signature, with the same comment as `PlanDelete`.

Check the `LogicalUpdate` members in v1.5.4: `columns` (a `PhysicalIndex` vector) and `expressions`. The `columns` indexes follow the table's column order, which matches `GetClickhouseColumns()`.

- [ ] **Step 4: `ch_mutations_sync`**

In `src/clickhouse_scanner_extension.cpp`:
```cpp
static void SetClickhouseMutationsSync(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull() || UBigIntValue::Get(parameter) > 2) {
		throw InvalidInputException("ch_mutations_sync must be 0, 1 or 2");
	}
}
```
```cpp
	config.AddExtensionOption("ch_mutations_sync",
	                          "mutations_sync sent with UPDATE: 0 = do not wait, 1 = wait on this replica, 2 = wait on "
	                          "all replicas",
	                          LogicalType::UBIGINT, Value::UBIGINT(2), SetClickhouseMutationsSync);
```

- [ ] **Step 5: README**

- In Limitations, remove the UPDATE bullet.
- In `## Writing`, change the DELETE paragraph's first sentence to cover UPDATE:
```markdown
`UPDATE`, `DELETE` and `TRUNCATE` are translated into one ClickHouse statement each: `ALTER TABLE … UPDATE … WHERE …`
(a mutation; `ch_mutations_sync`, default `2`, decides whether it waits), `DELETE FROM … WHERE …` (a synchronous
lightweight delete, MergeTree family only) or, with no `WHERE`, `TRUNCATE TABLE`.
```
- Extend the WHERE bullet: "`SET` values follow the same rules as `WHERE`. ClickHouse does not update sorting-key columns."
- Add a Settings row after `ch_default_table_engine`:
```markdown
| `ch_mutations_sync` | `2` | `mutations_sync` for `UPDATE`: 0 = do not wait, 1 = this replica, 2 = all replicas |
```

- [ ] **Step 6: Run the tests to verify they pass**

Run `make release && make smoke`, then `cmake --build build/debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'`, then the no-server suite. Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add -A src test README.md
git commit -m "feat: UPDATE on attached ClickHouse tables

<your harness's Co-Authored-By trailer>"
```

---

## Spec coverage (Phase 3, spec §10.3)

| Spec item | Task |
|---|---|
| `clickhouse_expression` allow-list (§6) | 1 |
| Logical PlanDelete / PlanUpdate hooks, accepted shape, rejections (§6) | 1, 2 |
| Predicate from table_filters (strict) and LogicalFilter; nothing dropped (§6) | 1 |
| DELETE → lightweight delete, synchronous; no WHERE / TRUNCATE → TRUNCATE TABLE (§2, D3) | 1 |
| UPDATE → ALTER TABLE … UPDATE with CAST and mutations_sync (§2, D3) | 2 |
| `ch_mutations_sync` (§2) | 2 |
| Pre-count affected rows (D7) | 1, 2 |
| ClickHouse server errors surface, e.g. a sorting-key update (§6) | 2 |
| READ_ONLY rejection (§6) | 1, 2 (ThrowIfReadOnly; existing read_only_attach tests) |
| Regression test: an untranslatable predicate never yields a partial statement (§9) | 1 (the `OR md5` case, and `IN` exactness) |
| README semantics note (§6) | 1, 2 |
