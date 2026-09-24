#include "storage/clickhouse_catalog_set.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

ClickhouseCatalogSet::ClickhouseCatalogSet(Catalog &catalog) : catalog(catalog) {
}

void ClickhouseCatalogSet::TryLoadEntries(ClientContext &context) {
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
	lock_guard<mutex> load_guard(load_lock);
	lock_guard<mutex> guard(entry_lock);
	// releasing the last reference to an entry here is safe: everything that keeps a raw pointer into one
	// past the clear owns it through GetEntryOwner() (see ClickhouseScanBindData::lifetime)
	entries.clear();
	ordered_entries.clear();
	is_loaded = false;
}

void ClickhouseCatalogSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	shared_ptr<CatalogEntry> shared_entry(std::move(entry));
	lock_guard<mutex> guard(entry_lock);
	entries[shared_entry->name] = shared_entry;
	ordered_entries.push_back(std::move(shared_entry));
}

} // namespace duckdb
