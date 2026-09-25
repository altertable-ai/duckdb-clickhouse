#include "storage/clickhouse_dml.hpp"

#include "clickhouse_expression.hpp"
#include "clickhouse_scanner.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_table_entry.hpp"

#include <clickhouse/columns/numeric.h>

namespace duckdb {

void ClickhouseDml::ThrowUnsupportedShape(const string &statement, const string &reason) {
	throw NotImplementedException("%s on ClickHouse tables must filter only the modified table with translatable "
	                              "expressions (%s); use clickhouse_execute() for anything else",
	                              statement, reason);
}

static const ClickhouseScanBindData &ScanBindData(const LogicalGet &get) {
	return get.bind_data->Cast<ClickhouseScanBindData>();
}

//! ClickHouse SQL for table column `column_id` of the scan: the same expression the scan reads it through (see
//! ClickhouseScanFunction::BuildQuery), so a predicate sees the values DuckDB sees. The bare quoted column for
//! every natively read type
static string ColumnSql(const ClickhouseScanBindData &bind_data, column_t column_id) {
	if (IsVirtualColumn(column_id) || column_id >= bind_data.columns.size()) {
		// rowid (always NULL for ClickHouse tables) or another virtual column
		throw NotImplementedException("reference to a virtual column");
	}
	auto &column = bind_data.columns[column_id];
	if (!column.readable) {
		throw NotImplementedException("column \"%s\" of type %s cannot be read", column.name, column.clickhouse_type);
	}
	return ClickhouseTypes::ReadExpression(column.type_node, ClickhouseUtils::QuoteIdentifier(column.name));
}

string ClickhouseDml::ResolveOutput(const LogicalOperator &op, idx_t index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		auto &column_ids = get.GetColumnIds();
		idx_t position = index;
		if (!get.projection_ids.empty()) {
			if (index >= get.projection_ids.size()) {
				throw NotImplementedException("reference outside the scan's output");
			}
			position = get.projection_ids[index];
		}
		if (position >= column_ids.size()) {
			throw NotImplementedException("reference outside the scan's output");
		}
		auto &column_index = column_ids[position];
		if (column_index.HasChildren()) {
			throw NotImplementedException("reference to a nested field pushed into the scan");
		}
		return ColumnSql(ScanBindData(get), column_index.GetPrimaryIndex());
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		idx_t child_index = index;
		if (!filter.projection_map.empty()) {
			if (index >= filter.projection_map.size()) {
				throw NotImplementedException("reference outside the filter's output");
			}
			child_index = filter.projection_map[index];
		}
		return ResolveOutput(*op.children[0], child_index);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		if (index >= op.expressions.size()) {
			throw NotImplementedException("reference outside the projection's output");
		}
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
		if (!ClickhouseScanFunction::IsClickhouseScan(get.function.name) || !get.bind_data ||
		    ScanBindData(get).table_entry != &table) {
			throw NotImplementedException("reads another table");
		}
		if (!get.children.empty()) {
			throw NotImplementedException("scan with an input");
		}
		auto &bind_data = ScanBindData(get);
		// at the logical level (before PhysicalPlanGenerator::CreatePlan(LogicalGet) renumbers them for the
		// physical scan), table_filters is keyed by the table column id itself (TableFilterSet::PushFilter keys by
		// ColumnIndex::GetPrimaryIndex()), not by position in the get's column_ids. Every filter is here, whether
		// or not the scan would push it into ClickHouse for a SELECT (supports_pushdown_type is only consulted
		// when the get is planned)
		for (auto &entry : get.table_filters.filters) {
			auto column_id = entry.first;
			if (IsVirtualColumn(column_id) || column_id >= bind_data.columns.size()) {
				throw NotImplementedException("filter on a virtual column");
			}
			auto sql = ClickhouseExpression::TranslateTableFilter(*entry.second, bind_data.columns[column_id].type,
			                                                      ColumnSql(bind_data, column_id));
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
	return ClickhouseDmlTarget {ch_table, ClickhouseUtils::QualifiedName(ch_table.schema.name, ch_table.name),
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
	auto &catalog =
	    ClickhouseCatalog::GetAttachedDatabase(context.client, statement.catalog_name, statement.description);
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
