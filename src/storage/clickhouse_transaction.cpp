#include "storage/clickhouse_transaction.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

ClickhouseTransaction::ClickhouseTransaction(TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context) {
}

ClickhouseTransaction::~ClickhouseTransaction() = default;

ClickhouseTransaction &ClickhouseTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<ClickhouseTransaction>();
}

ClickhouseTransactionManager::ClickhouseTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
}

Transaction &ClickhouseTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<ClickhouseTransaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData ClickhouseTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
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
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

void ClickhouseTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// nothing to checkpoint: all data lives in ClickHouse
}

} // namespace duckdb
