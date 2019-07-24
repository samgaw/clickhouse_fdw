/*-------------------------------------------------------------------------
 *
 * clickhousedb_connection.c
 *		  Connection management functions for clickhousedb_fdw
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/clickhousedb_fdw/clickhousedb_connection.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_user_mapping.h"
#include "access/xact.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


#include "clickhousedb_fdw.h"

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/* for assigning cursor numbers and prepared statement numbers */
static unsigned int cursor_number = 0;
static unsigned int prep_stmt_number = 0;

/* tracks whether any work is needed in callback functions */
static bool xact_got_connection = false;

/* prototypes of private functions */
static void check_conn_params(const char *password, UserMapping *user);
static void begin_remote_xact(ConnCacheEntry *entry);
static void pgfdw_xact_callback(XactEvent event, void *arg);
static void pgfdw_subxact_callback(SubXactEvent event,
                                   SubTransactionId mySubid,
                                   SubTransactionId parentSubid,
                                   void *arg);
static void pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static void pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry);
static bool pgfdw_cancel_query(ch_connection conn);
static bool pgfdw_exec_cleanup_query(ch_connection conn, const char *query,
                                     bool ignore_errors);

static ch_connection
clickhouse_connect(ForeignServer *server, UserMapping *user)
{
	char	   *connstring;
	char	   *driver = "http";

	ch_connection_details	details = {"127.0.0.1", 8123, NULL, NULL, "default"};

	ExtractConnectionOptions(server->options, &driver, &details.host,
		&details.port, &details.dbname, &details.username, &details.password);
	ExtractConnectionOptions(user->options, &driver, &details.host,
		&details.port, &details.dbname, &details.username, &details.password);

	if (strcmp(driver, "http") == 0)
	{
		ch_connection conn;
		char *connstring;

		if (details.username && details.password)
			connstring = psprintf("http://%s:%s@%s:%d/", details.username,
				details.password, details.host, details.port);
		else if (details.username)
			connstring = psprintf("http://%s@%s:%d/", details.username,
				details.host, details.port);
		else
			connstring = psprintf("http://%s:%d/", details.host, details.port);

		conn =  http_connect(connstring);
		pfree(connstring);
		return conn;
	}
	else if (strcmp(driver, "binary") == 0)
		return binary_connect(&details);
	else
		elog(ERROR, "invalid ClickHouse connection driver");
}

ch_connection
GetConnection(UserMapping *user, bool will_prep_stmt, bool read)
{
	bool		found;
	ConnCacheEntry *entry;
	ConnCacheKey key;

	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash = hash_create("clickhousedb_fdw connections", 8,
		                             &ctl,
		                             HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		RegisterXactCallback(pgfdw_xact_callback, NULL);
		RegisterSubXactCallback(pgfdw_subxact_callback, NULL);
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
		                              pgfdw_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
		                              pgfdw_inval_callback, (Datum) 0);
	}

	/* Set flag that we did GetConnection during the current transaction */
	xact_got_connection = true;

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key.userid = user->umid;
	key.read = read;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We need only clear "conn" here; remaining fields will be filled
		 * later when "conn" is set.
		 */
		entry->gate.conn = NULL;
	}

	/* Reject further use of connections which failed abort cleanup. */
	pgfdw_reject_incomplete_xact_state_change(entry);

	/*
	 * If the connection needs to be remade due to invalidation, disconnect as
	 * soon as we're out of all transactions.
	 */
	if (entry->gate.conn != NULL && entry->invalidated && entry->xact_depth == 0)
	{
		elog(LOG, "closing connection to ClickHouse due to invalidation");
		entry->gate.methods->disconnect(entry->gate.conn);
		entry->gate.conn = NULL;
	}

	/*
	 * We don't check the health of cached connection here, because it would
	 * require some overhead.  Broken connection will be detected when the
	 * connection is actually used.
	 */

	/*
	 * If cache entry doesn't have a connection, we have to establish a new
	 * connection.  (If clickhouse_connect throws an error, the cache entry
	 * will remain in a valid empty state, ie conn == NULL.)
	 */
	if (entry->gate.conn == NULL)
	{
		ForeignServer *server = GetForeignServer(user->serverid);

		/* Reset all transient state fields, to be sure all are clean */
		entry->xact_depth = 0;
		entry->have_error = false;
		entry->changing_xact_state = false;
		entry->invalidated = false;
		entry->read = read;
		entry->server_hashvalue =
		    GetSysCacheHashValue1(FOREIGNSERVEROID,
		                          ObjectIdGetDatum(server->serverid));
		entry->mapping_hashvalue =
		    GetSysCacheHashValue1(USERMAPPINGOID,
		                          ObjectIdGetDatum(user->umid));

		/* Now try to make the connection */
		entry->gate = clickhouse_connect(server, user);

		elog(DEBUG3,
		     "new clickhousedb_fdw connection %p for server \"%s\" (user mapping oid %u, userid %u)",
		     entry->gate.conn, server->servername, user->umid, user->userid);
	}

	return entry->gate;
}

/*
 * pgfdw_xact_callback --- cleanup at main-transaction end.
 */
static void
pgfdw_xact_callback(XactEvent event, void *arg)
{
}

/*
 * pgfdw_subxact_callback --- cleanup at subtransaction end.
 */
static void
pgfdw_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
                       SubTransactionId parentSubid, void *arg)
{
}

/*
 * Connection invalidation callback function
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * mark connections depending on that entry as needing to be remade.
 * We can't immediately destroy them, since they might be in the midst of
 * a transaction, but we'll remake them at the next opportunity.
 *
 * Although most cache invalidation callbacks blow away all the related stuff
 * regardless of the given hashvalue, connections are expensive enough that
 * it's worth trying to avoid that.
 *
 * NB: We could avoid unnecessary disconnection more strictly by examining
 * individual option values, but it seems too much effort for the gain.
 */
static void
pgfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore empty entries */
		if (entry->gate.conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state */
		if (hashvalue == 0 ||
		        (cacheid == FOREIGNSERVEROID &&
		         entry->server_hashvalue == hashvalue) ||
		        (cacheid == USERMAPPINGOID &&
		         entry->mapping_hashvalue == hashvalue))
		{
			entry->invalidated = true;
		}
	}
}

/*
 * Raise an error if the given connection cache entry is marked as being
 * in the middle of an xact state change.  This should be called at which no
 * such change is expected to be in progress; if one is found to be in
 * progress, it means that we aborted in the middle of a previous state change
 * and now don't know what the remote transaction state actually is.
 * Such connections can't safely be further used.  Re-establishing the
 * connection would change the snapshot and roll back any writes already
 * performed, so that's not an option, either. Thus, we must abort.
 */
static void
pgfdw_reject_incomplete_xact_state_change(ConnCacheEntry *entry)
{
	HeapTuple	tup;
	Form_pg_user_mapping umform;
	ForeignServer *server;

	/* nothing to do for inactive entries and entries of sane state */
	if (entry->gate.conn == NULL || !entry->changing_xact_state)
	{
		return;
	}

	/* make sure this entry is inactive */
	entry->gate.methods->disconnect(entry->gate.conn);
	entry->gate.conn = NULL;

	/* find server name to be shown in the message below */
	tup = SearchSysCache1(USERMAPPINGOID,
	                      ObjectIdGetDatum(entry->key.userid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for user mapping %u", entry->key.userid);

	umform = (Form_pg_user_mapping) GETSTRUCT(tup);
	server = GetForeignServer(umform->umserver);
	ReleaseSysCache(tup);

	ereport(ERROR,
	        (errcode(ERRCODE_CONNECTION_EXCEPTION),
	         errmsg("connection to server \"%s\" was lost",
	                server->servername)));
}
