#pragma once

#include "duckdb/common/types/data_chunk.hpp"

#include <clickhouse/block.h>

namespace duckdb {

class ClickhouseConversion {
public:
	//! Converts rows [offset, offset + count) of every column of the block into the output chunk. column_names
	//! are only used for error messages. projection_ids selects and orders which of the block's columns (indices
	//! into column_names, matching the block's own column order) end up in output; empty means all of them, in
	//! order -- see TableFunctionInitInput::projection_ids.
	static void ConvertBlock(const clickhouse::Block &block, DataChunk &output, idx_t offset, idx_t count,
	                          const vector<string> &column_names, const vector<idx_t> &projection_ids);
	//! Converts rows [offset, offset + count) of a column into a flat vector, starting at row 0
	static void ConvertColumn(const clickhouse::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
	                           const string &column_name);
};

} // namespace duckdb
