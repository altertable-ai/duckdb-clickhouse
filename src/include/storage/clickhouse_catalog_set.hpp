#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <functional>

namespace duckdb {
class Catalog;
class ClientContext;

//! A lazily loaded, cached set of catalog entries (the schemas of a catalog, or the tables of a schema)
class ClickhouseCatalogSet {
public:
	explicit ClickhouseCatalogSet(Catalog &catalog);
	virtual ~ClickhouseCatalogSet() = default;

	//! Exact (case-sensitive) match first, then the first case-insensitive match. GetEntry() and Scan() start the
	//! database's DuckDB transaction for `context` first, if it has not started yet (see ClearEntries())
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const string &name);
	//! The shared_ptr that owns the currently cached entry named `name`, for a caller that has to keep it
	//! alive past a ClearEntries() (see ClickhouseScanBindData::lifetime). Exact match only, and never
	//! loads: the caller already holds the entry it is asking about. Null if the cache was cleared in the
	//! meantime.
	shared_ptr<CatalogEntry> GetEntryOwner(const string &name);
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	//! Drops the cache; the next access reloads from ClickHouse. The dropped entries are retired, not freed: they stay
	//! alive until every DuckDB transaction active on the database at this point has ended (see
	//! ClickhouseTransactionManager), so a raw pointer or reference another connection -- or the caller -- obtained
	//! from this set within its transaction stays valid until that transaction ends
	void ClearEntries();

protected:
	virtual void LoadEntries(ClientContext &context) = 0;
	void CreateEntry(unique_ptr<CatalogEntry> entry);

	Catalog &catalog;

private:
	void TryLoadEntries(ClientContext &context);

	mutex load_lock;
	mutex entry_lock;
	bool is_loaded = false;
	vector<shared_ptr<CatalogEntry>> ordered_entries;
	unordered_map<string, shared_ptr<CatalogEntry>> entries;
};

} // namespace duckdb
