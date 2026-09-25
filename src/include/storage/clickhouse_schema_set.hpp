#pragma once

#include "storage/clickhouse_catalog_set.hpp"

namespace duckdb {

//! The ClickHouse databases of an attached service, exposed as DuckDB schemas
class ClickhouseSchemaSet : public ClickhouseCatalogSet {
public:
	explicit ClickhouseSchemaSet(Catalog &catalog);

protected:
	void LoadEntries(ClientContext &context) override;
};

} // namespace duckdb
