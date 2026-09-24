#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

//! ClickHouse has no multi-statement transactions: a DuckDB transaction on an attached ClickHouse database holds no
//! remote state, every scan is an independent ClickHouse query and every write is committed as soon as it is sent.
//! The transaction only remembers whether it wrote, so a ROLLBACK can warn that those writes were not undone.
class ClickhouseTransaction : public Transaction {
public:
	ClickhouseTransaction(TransactionManager &manager, ClientContext &context);
	~ClickhouseTransaction() override;

	static ClickhouseTransaction &Get(ClientContext &context, Catalog &catalog);

	void MarkWritten() {
		written = true;
	}
	bool HasWritten() const {
		return written;
	}

private:
	atomic<bool> written {false};
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
