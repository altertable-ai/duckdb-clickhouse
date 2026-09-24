#include "storage/clickhouse_transaction.hpp"

#include "duckdb/common/error_data.hpp"

namespace duckdb {

ClickhouseTransaction::ClickhouseTransaction(TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context) {
}

ClickhouseTransaction::~ClickhouseTransaction() = default;

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
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

void ClickhouseTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// nothing to checkpoint: all data lives in ClickHouse
}

} // namespace duckdb
