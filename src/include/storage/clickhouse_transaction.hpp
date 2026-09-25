#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {
class CatalogEntry;

//! ClickHouse has no multi-statement transactions: a DuckDB transaction on an attached ClickHouse database holds no
//! remote state, every scan is an independent ClickHouse query and every write is committed as soon as it is sent.
//! The transaction only remembers whether it wrote, so a ROLLBACK can warn that those writes were not undone, and
//! carries the id that decides when retired catalog entries may be freed (see ClickhouseTransactionManager).
class ClickhouseTransaction : public Transaction {
public:
	ClickhouseTransaction(TransactionManager &manager, ClientContext &context, idx_t transaction_id);
	~ClickhouseTransaction() override;

	static ClickhouseTransaction &Get(ClientContext &context, Catalog &catalog);

	void MarkWritten() {
		written = true;
	}
	bool HasWritten() const {
		return written;
	}
	//! Assigned by ClickhouseTransactionManager::StartTransaction(), strictly increasing
	idx_t GetTransactionId() const {
		return transaction_id;
	}

private:
	atomic<bool> written {false};
	const idx_t transaction_id;
};

//! Starts and ends the (stateless, see ClickhouseTransaction) transactions of one attached ClickHouse database, and
//! owns the catalog entries the database's cache has retired.
//!
//! Retire-don't-free. ClickhouseCatalog::ClearCache() (every DDL statement, CTAS, clickhouse_execute(),
//! clickhouse_clear_cache(), from any connection) drops the cached schema entries -- and with them their table sets
//! and table entries -- while other connections, or the clearing statement itself, may still hold raw pointers or
//! references into them: a table entry being bound, LogicalCreateTable::schema / PhysicalCreateTable::schema, the
//! `this` of the ClickhouseSchemaEntry method running the DDL, the entry ClickhouseDdl::CreateTable() returns to the
//! CTAS sink. So the entries are not freed but handed to RetireEntries(), which keeps them alive until every DuckDB
//! transaction that was active on this database when they were retired has ended:
//!
//! 1. Every transaction gets an id from `next_transaction_id`, strictly increasing, under `transaction_lock`.
//! 2. A retired batch is stamped, under the same lock, with the id the next transaction would get. Every transaction
//!    that can hold a pointer into the batch has an id below the stamp: it started (id assigned) before it looked
//!    the entry up (ClickhouseCatalogSet makes sure every lookup runs inside this database's transaction), the
//!    lookup happened before ClearEntries() removed the entry from the cache, and the removal happened before the
//!    stamp was taken. A transaction that starts after the stamp was taken can only find the fresh entries.
//! 3. Whenever a transaction ends (commit or rollback), under the same lock again, every batch whose stamp is at most
//!    the id of the oldest transaction still active -- every batch when none is -- is released: no transaction with
//!    an id below its stamp is left. The last reference is dropped after the lock is released.
//!
//! One mutex covers ids, active transactions and retired batches, so the three steps cannot interleave in a way that
//! frees a batch too early. The cost: a batch lives until the oldest transaction active at its retirement ends, so a
//! long-lived transaction (a BEGIN left open on a connection that touched this database) keeps every batch retired
//! meanwhile alive; they are released as soon as it ends, or when the database is detached.
//!
//! What this does not cover is a bound plan that outlives its transaction -- a prepared statement. DuckDB rebinds a
//! prepared statement that reads or writes this database before every EXECUTE (ClickhouseCatalog has no catalog
//! version, so PreparedStatementData::RequireRebind() always asks for a rebind), and the scan's bind data owns its
//! schema entry through ClickhouseScanBindData::lifetime (ClickhouseCatalog::GetSchemaEntryOwner()) regardless.
class ClickhouseTransactionManager : public TransactionManager {
public:
	explicit ClickhouseTransactionManager(AttachedDatabase &db);
	~ClickhouseTransactionManager() override;

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

	//! Keeps `entries` alive until every transaction active on this database right now has ended (see above)
	void RetireEntries(vector<shared_ptr<CatalogEntry>> entries);

private:
	//! Forgets `transaction` and releases the retired batches no active transaction can still reference
	void EndTransaction(Transaction &transaction);

	struct RetiredBatch {
		//! next_transaction_id when the batch was retired: transactions with a lower id may still reference it
		idx_t stamp;
		vector<shared_ptr<CatalogEntry>> entries;
	};

	//! Guards everything below
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<ClickhouseTransaction>> transactions;
	idx_t next_transaction_id = 1;
	//! In retirement order, so the stamps never decrease
	vector<RetiredBatch> retired;
};

} // namespace duckdb
