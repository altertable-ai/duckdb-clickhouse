#pragma once

#include "storage/clickhouse_catalog_set.hpp"

namespace duckdb {
class SchemaCatalogEntry;

//! The tables and views of one ClickHouse database
class ClickhouseTableSet : public ClickhouseCatalogSet {
public:
	ClickhouseTableSet(SchemaCatalogEntry &schema, Catalog &catalog);

protected:
	void LoadEntries(ClientContext &context) override;

private:
	SchemaCatalogEntry &schema;
};

} // namespace duckdb
