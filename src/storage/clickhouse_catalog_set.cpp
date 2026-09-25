#include "storage/clickhouse_catalog_set.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

ClickhouseCatalogSet::ClickhouseCatalogSet(Catalog &catalog) : catalog(catalog) {
}

void ClickhouseCatalogSet::TryLoadEntries(ClientContext &context) {
	// before any entry is handed out: see ClickhouseTransactionManager (retire-don't-free). Starting this database's
	// transaction is what gives the caller an id below the stamp of any batch retiring the entries it is about to
	// get. Catalog lookups already run in it (CatalogTransaction starts it), but duckdb_tables(), duckdb_columns(),
	// information_schema and friends scan every attached catalog through ScanSchemas() without doing so
	if (context.transaction.HasActiveTransaction()) {
		Transaction::Get(context, catalog.GetAttached());
	}
	lock_guard<mutex> load_guard(load_lock);
	if (is_loaded) {
		return;
	}
	try {
		LoadEntries(context);
	} catch (...) {
		lock_guard<mutex> guard(entry_lock);
		entries.clear();
		ordered_entries.clear();
		throw;
	}
	is_loaded = true;
}

optional_ptr<CatalogEntry> ClickhouseCatalogSet::GetEntry(ClientContext &context, const string &name) {
	TryLoadEntries(context);
	lock_guard<mutex> guard(entry_lock);
	auto exact = entries.find(name);
	if (exact != entries.end()) {
		return exact->second.get();
	}
	for (auto &entry : ordered_entries) {
		if (StringUtil::CIEquals(entry->name, name)) {
			return entry.get();
		}
	}
	return nullptr;
}

shared_ptr<CatalogEntry> ClickhouseCatalogSet::GetEntryOwner(const string &name) {
	lock_guard<mutex> guard(entry_lock);
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second;
}

void ClickhouseCatalogSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	TryLoadEntries(context);
	vector<shared_ptr<CatalogEntry>> snapshot;
	{
		lock_guard<mutex> guard(entry_lock);
		snapshot = ordered_entries;
	}
	for (auto &entry : snapshot) {
		callback(*entry);
	}
}

void ClickhouseCatalogSet::ClearEntries() {
	vector<shared_ptr<CatalogEntry>> cleared;
	{
		lock_guard<mutex> load_guard(load_lock);
		lock_guard<mutex> guard(entry_lock);
		// ordered_entries holds every entry `entries` does
		cleared = std::move(ordered_entries);
		ordered_entries.clear();
		entries.clear();
		is_loaded = false;
	}
	// retired, not freed: other connections -- or the caller itself -- may still hold raw pointers or references into
	// these entries. They stay alive until every transaction active on this database now has ended (see
	// ClickhouseTransactionManager); only after the removal above, so that no transaction can start, look up one of
	// these entries and still get an id at or past the batch's stamp
	catalog.Cast<ClickhouseCatalog>().RetireEntries(std::move(cleared));
}

void ClickhouseCatalogSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	shared_ptr<CatalogEntry> shared_entry(std::move(entry));
	lock_guard<mutex> guard(entry_lock);
	entries[shared_entry->name] = shared_entry;
	ordered_entries.push_back(std::move(shared_entry));
}

} // namespace duckdb
