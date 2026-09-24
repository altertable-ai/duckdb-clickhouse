#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

//! ClickHouse has no multi-statement transactions: a DuckDB transaction on an attached ClickHouse database holds no
//! remote state and every scan is an independent ClickHouse query
class ClickhouseTransaction : public Transaction {
public:
	ClickhouseTransaction(TransactionManager &manager, ClientContext &context);
	~ClickhouseTransaction() override;
};

class ClickhouseTransactionManager : public TransactionManager {
public:
	explicit ClickhouseTransactionManager(AttachedDatabase &db);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<ClickhouseTransaction>> transactions;
};

} // namespace duckdb
