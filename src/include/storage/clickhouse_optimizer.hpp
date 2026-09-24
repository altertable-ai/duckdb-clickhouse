#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! Moves LIMIT / OFFSET and ORDER BY ... LIMIT (TOP_N) that sit directly on top of a ClickHouse scan into the query
//! sent to ClickHouse
class ClickhouseOptimizer {
public:
	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
};

} // namespace duckdb
