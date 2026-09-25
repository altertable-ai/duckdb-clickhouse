#include "storage/clickhouse_transaction.hpp"

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/attached_database.hpp"
// required, not just tidy: without ClientContext's definition, context.lock() below instantiates
// duckdb::shared_ptr<ClientContext>'s converting constructor with the no-op enable_shared_from_this hook, and
// the linker may keep that instantiation for the whole binary (it does at -O0), breaking shared_from_this()
#include "duckdb/main/client_context.hpp"

namespace duckdb {

ClickhouseTransaction::ClickhouseTransaction(TransactionManager &manager, ClientContext &context,
                                             idx_t transaction_id_p)
    : Transaction(manager, context), transaction_id(transaction_id_p) {
}

ClickhouseTransaction::~ClickhouseTransaction() = default;

ClickhouseTransaction &ClickhouseTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<ClickhouseTransaction>();
}

ClickhouseTransactionManager::ClickhouseTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
}

// batches still retired here (a transaction left open until DETACH or shutdown) are released with the manager. The
// catalog their entries point to outlives it: AttachedDatabase declares its catalog before its transaction manager,
// so it destroys the manager first
ClickhouseTransactionManager::~ClickhouseTransactionManager() = default;

Transaction &ClickhouseTransactionManager::StartTransaction(ClientContext &context) {
	lock_guard<mutex> guard(transaction_lock);
	auto transaction = make_uniq<ClickhouseTransaction>(*this, context, next_transaction_id++);
	auto &result = *transaction;
	transactions[result] = std::move(transaction);
	return result;
}

void ClickhouseTransactionManager::RetireEntries(vector<shared_ptr<CatalogEntry>> entries) {
	if (entries.empty()) {
		return;
	}
	lock_guard<mutex> guard(transaction_lock);
	// every transaction that may still reference `entries` has already been given an id: see the class comment
	retired.push_back(RetiredBatch {next_transaction_id, std::move(entries)});
}

void ClickhouseTransactionManager::EndTransaction(Transaction &transaction) {
	// both destroyed after the lock is released, when this function returns: dropping the last reference to a schema
	// entry frees its whole table set
	unique_ptr<ClickhouseTransaction> ended;
	vector<RetiredBatch> released;
	lock_guard<mutex> guard(transaction_lock);
	auto entry = transactions.find(transaction);
	if (entry != transactions.end()) {
		ended = std::move(entry->second);
		transactions.erase(entry);
	}
	// with no transaction active, every id handed out so far belongs to a transaction that has ended, and no stamp
	// exceeds next_transaction_id
	auto oldest_active = next_transaction_id;
	for (auto &active : transactions) {
		oldest_active = MinValue(oldest_active, active.second->GetTransactionId());
	}
	// stamps never decrease, so the releasable batches are a prefix
	idx_t release_count = 0;
	while (release_count < retired.size() && retired[release_count].stamp <= oldest_active) {
		release_count++;
	}
	if (release_count == 0) {
		return;
	}
	for (idx_t i = 0; i < release_count; i++) {
		released.push_back(std::move(retired[i]));
	}
	retired.erase(retired.begin(), retired.begin() + static_cast<std::ptrdiff_t>(release_count));
}

ErrorData ClickhouseTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	EndTransaction(transaction);
	return ErrorData();
}

void ClickhouseTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &clickhouse_transaction = transaction.Cast<ClickhouseTransaction>();
	if (clickhouse_transaction.HasWritten()) {
		auto context = clickhouse_transaction.context.lock();
		if (context) {
			DUCKDB_LOG_WARNING(*context, StringUtil::Format("ClickHouse writes made in this transaction were already "
			                                                "committed and cannot be rolled back (database \"%s\")",
			                                                db.GetName()));
		}
	}
	EndTransaction(transaction);
}

void ClickhouseTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// nothing to checkpoint: all data lives in ClickHouse
}

} // namespace duckdb
