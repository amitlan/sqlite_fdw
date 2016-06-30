/*-------------------------------------------------------------------------
 *
 * sqlite Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2013 Guillaume Lelarge
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Guillaume Lelarge <guillaume@lelarge.info>
 *
 * IDENTIFICATION
 *        sqlite_fdw/src/sqlite_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"

#include "funcapi.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"

#include <sqlite3.h>
#include <sys/stat.h>

PG_MODULE_MAGIC;

/*
 * Default values
 */
// This value is taken from sqlite
// (without stats, sqlite defaults to 1 million tuples for a table)
#define DEFAULT_ESTIMATED_LINES 1000000

/*
 * SQL functions
 */
extern Datum sqlite_fdw_handler(PG_FUNCTION_ARGS);
extern Datum sqlite_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(sqlite_fdw_handler);
PG_FUNCTION_INFO_V1(sqlite_fdw_validator);


/* callback functions */
#if (PG_VERSION_NUM >= 90200)
static void sqliteGetForeignRelSize(PlannerInfo *root,
						   RelOptInfo *baserel,
						   Oid foreigntableid);

static void sqliteGetForeignPaths(PlannerInfo *root,
						 RelOptInfo *baserel,
						 Oid foreigntableid);

static ForeignScan *sqliteGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
#if (PG_VERSION_NUM >= 90500)
						List *scan_clauses,
						Plan *outer_plan);
#else
						List *scan_clauses);
#endif

#else
static FdwPlan *sqlitePlanForeignScan(Oid foreigntableid, PlannerInfo *root, RelOptInfo *baserel);
#endif

static void sqliteBeginForeignScan(ForeignScanState *node,
						  int eflags);

static TupleTableSlot *sqliteIterateForeignScan(ForeignScanState *node);

static void sqliteReScanForeignScan(ForeignScanState *node);

static void sqliteEndForeignScan(ForeignScanState *node);

#if (PG_VERSION_NUM >= 90300)
static void sqliteAddForeignUpdateTargets(Query *parsetree,
								 RangeTblEntry *target_rte,
								 Relation target_relation);

static List *sqlitePlanForeignModify(PlannerInfo *root,
						   ModifyTable *plan,
						   Index resultRelation,
						   int subplan_index);

static void sqliteBeginForeignModify(ModifyTableState *mtstate,
							ResultRelInfo *resultRelInfo,
							List *fdw_private,
							int subplan_index,
							int eflags);

static TupleTableSlot *sqliteExecForeignInsert(EState *estate,
						   ResultRelInfo *resultRelInfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot);

static TupleTableSlot *sqliteExecForeignUpdate(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot);

static TupleTableSlot *sqliteExecForeignDelete(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot);

static void sqliteEndForeignModify(EState *estate,
						  ResultRelInfo *resultRelInfo);
#endif

static void sqliteExplainForeignScan(ForeignScanState *node,
							struct ExplainState *es);

#if (PG_VERSION_NUM >= 90300)
static void sqliteExplainForeignModify(ModifyTableState *mtstate,
							  ResultRelInfo *rinfo,
							  List *fdw_private,
							  int subplan_index,
							  struct ExplainState *es);
#endif

#if (PG_VERSION_NUM >= 90200)
static bool sqliteAnalyzeForeignTable(Relation relation,
							 AcquireSampleRowsFunc *func,
							 BlockNumber *totalpages);
#endif

/*
 * Helper functions
 */
static bool sqliteIsValidOption(const char *option, Oid context);
static void sqliteGetOptions(Oid foreigntableid, char **database, char **table);
static int GetEstimatedRows(Oid foreigntableid);
static bool file_exists(const char *name);


/* 
 * structures used by the FDW 
 *
 * These next two are not actually used by sqlite, but something like this
 * will be needed by anything more complicated that does actual work.
 *
 */

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct SQLiteFdwOption
{
	const char	*optname;
	Oid		optcontext;	/* Oid of catalog in which option may appear */
};

/*
 * Describes the valid options for objects that use this wrapper.
 */
static struct SQLiteFdwOption valid_options[] =
{

	/* Connection options */
	{ "database",  ForeignServerRelationId },

	/* Table options */
	{ "table",     ForeignTableRelationId },

	/* Sentinel */
	{ NULL,			InvalidOid }
};

/*
 * This is what will be set and stashed away in fdw_private and fetched 
 * for subsequent routines.
 */
typedef struct
{
	char	   *foo;
	int			bar;
}	sqliteFdwPlanState;

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */

typedef struct SQLiteFdwExecutionState
{
	sqlite3       *conn;
	sqlite3_stmt  *result;
	char          *query;
} SQLiteFdwExecutionState;

/*
 * Execution state of a foreign insert operation.
 */
typedef struct SqliteFdwModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */

	/* for remote query execution */
	sqlite3	   *conn;			/* connection for the scan */

	/* prepared statement */
	sqlite3_stmt *stmt;

	/* extracted fdw_private data */
	char	   *query;			/* text of INSERT/UPDATE/DELETE command */
	List	   *target_attrs;	/* list of target attribute numbers */

	/* info about parameters for prepared statement */
	int			p_nums;			/* number of parameters to transmit */
	FmgrInfo   *p_flinfo;		/* output conversion functions for them */

	/* working memory context */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} SqliteFdwModifyState;

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a sqlite_fdw foreign table.  We store:
 *
 * 1) INSERT statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers
 */
enum FdwModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwModifyPrivateUpdateSql,
	/* Integer list of target attribute numbers for INSERT/UPDATE */
	FdwModifyPrivateTargetAttnums,
};

/*
 * Deparse functions
 */

#define REL_ALIAS_PREFIX	"r"
/* Handy macro to add relation name qualification */
#define ADD_REL_QUALIFIER(buf, varno)	\
		appendStringInfo((buf), "%s%d.", REL_ALIAS_PREFIX, (varno))

#if (PG_VERSION_NUM >= 90300)
static void deparseInsertSql(StringInfo buf, PlannerInfo *root,
				 Index rtindex, Relation rel, List *targetAttrs);
static void deparseRelation(StringInfo buf, Relation rel);
static void deparseColumnRef(StringInfo buf, int varno, int varattno, PlannerInfo *root,
				 bool qualify_col);
static void deparseTargetList(StringInfo buf,
				  PlannerInfo *root,
				  Index rtindex,
				  Relation rel,
				  bool is_returning,
				  Bitmapset *attrs_used,
				  bool qualify_col,
				  List **retrieved_attrs);
static const char **convert_prep_stmt_params(struct SqliteFdwModifyState *fmstate,
						 TupleTableSlot *slot);
#endif

static sqlite3 *GetConnection(Oid ftableId);
static void ReleaseConnection(sqlite3* db);

Datum
sqlite_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	elog(DEBUG1,"entering function %s",__func__);

	/* assign the handlers for the FDW */

	/* these are required */
#if (PG_VERSION_NUM >= 90200)
	fdwroutine->GetForeignRelSize = sqliteGetForeignRelSize;
	fdwroutine->GetForeignPaths = sqliteGetForeignPaths;
	fdwroutine->GetForeignPlan = sqliteGetForeignPlan;
#else
	fdwroutine->PlanForeignScan = sqlitePlanForeignScan;
#endif
	fdwroutine->BeginForeignScan = sqliteBeginForeignScan;
	fdwroutine->IterateForeignScan = sqliteIterateForeignScan;
	fdwroutine->ReScanForeignScan = sqliteReScanForeignScan;
	fdwroutine->EndForeignScan = sqliteEndForeignScan;

	/* remainder are optional - use NULL if not required */
	/* support for insert / update / delete */
#if (PG_VERSION_NUM >= 90300)
	fdwroutine->AddForeignUpdateTargets = sqliteAddForeignUpdateTargets;
	fdwroutine->PlanForeignModify = sqlitePlanForeignModify;
	fdwroutine->BeginForeignModify = sqliteBeginForeignModify;
	fdwroutine->ExecForeignInsert = sqliteExecForeignInsert;
	fdwroutine->ExecForeignUpdate = sqliteExecForeignUpdate;
	fdwroutine->ExecForeignDelete = sqliteExecForeignDelete;
	fdwroutine->EndForeignModify = sqliteEndForeignModify;
#endif

	/* support for EXPLAIN */
	fdwroutine->ExplainForeignScan = sqliteExplainForeignScan;
#if (PG_VERSION_NUM >= 90300)
	fdwroutine->ExplainForeignModify = sqliteExplainForeignModify;
#endif

#if (PG_VERSION_NUM >= 90200)
	/* support for ANALYSE */
	fdwroutine->AnalyzeForeignTable = sqliteAnalyzeForeignTable;
#endif

	PG_RETURN_POINTER(fdwroutine);
}

Datum
sqlite_fdw_validator(PG_FUNCTION_ARGS)
{
	List      *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid       catalog = PG_GETARG_OID(1);
	char      *database = NULL;
	char      *table = NULL;
	ListCell  *cell;

	elog(DEBUG1,"entering function %s",__func__);

	/*
	 * Check that only options supported by sqlite_fdw,
	 * and allowed for the current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem	   *def = (DefElem *) lfirst(cell);

		if (!sqliteIsValidOption(def->defname, catalog))
		{
			struct SQLiteFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
							 opt->optname);
			}

			ereport(ERROR, 
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME), 
				errmsg("invalid option \"%s\"", def->defname), 
				errhint("Valid options in this context are: %s", buf.len ? buf.data : "<none>")
				));
		}

		if (strcmp(def->defname, "database") == 0)
		{
			if (database)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("redundant options: database (%s)", defGetString(def))
					));
			if (!file_exists(defGetString(def)))
				ereport(ERROR,
					(errcode_for_file_access(),
					errmsg("could not access file \"%s\"", defGetString(def))
					));

			database = defGetString(def);
		}
		else if (strcmp(def->defname, "table") == 0)
		{
			if (table)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("redundant options: table (%s)", defGetString(def))
					));

			table = defGetString(def);
		}
	}

	/* Check we have the options we need to proceed */
	if (catalog == ForeignServerRelationId && !database)
		ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("The database name must be specified")
			));

	PG_RETURN_VOID();
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
sqliteIsValidOption(const char *option, Oid context)
{
	struct SQLiteFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Fetch the options for a mysql_fdw foreign table.
 */
static void
sqliteGetOptions(Oid foreigntableid, char **database, char **table)
{
	ForeignTable   *f_table;
	ForeignServer  *f_server;
	List           *options;
	ListCell       *lc;

	/*
	 * Extract options from FDW objects.
	 */
	f_table = GetForeignTable(foreigntableid);
	f_server = GetForeignServer(f_table->serverid);

	options = NIL;
	options = list_concat(options, f_table->options);
	options = list_concat(options, f_server->options);

	/* Loop through the options */
	foreach(lc, options)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "database") == 0)
		{
			*database = defGetString(def);
		}

		if (strcmp(def->defname, "table") == 0)
		{
			*table = defGetString(def);
		}
	}

	if (!*table)
	{
		*table = get_rel_name(foreigntableid);
	}

	/* Check we have the options we need to proceed */
	if (!*database || !*table)
		ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("a database and a table must be specified")
			));
}


#if (PG_VERSION_NUM >= 90200)
static void
sqliteGetForeignRelSize(PlannerInfo *root,
						   RelOptInfo *baserel,
						   Oid foreigntableid)
{
	/*
	 * Obtain relation size estimates for a foreign table. This is called at
	 * the beginning of planning for a query that scans a foreign table. root
	 * is the planner's global information about the query; baserel is the
	 * planner's information about this table; and foreigntableid is the
	 * pg_class OID of the foreign table. (foreigntableid could be obtained
	 * from the planner data structures, but it's passed explicitly to save
	 * effort.)
	 *
	 * This function should update baserel->rows to be the expected number of
	 * rows returned by the table scan, after accounting for the filtering
	 * done by the restriction quals. The initial value of baserel->rows is
	 * just a constant default estimate, which should be replaced if at all
	 * possible. The function may also choose to update baserel->width if it
	 * can compute a better estimate of the average result row width.
	 */
	sqliteFdwPlanState *fdw_private;

	elog(DEBUG1,"entering function %s",__func__);

	baserel->rows = 0;

	fdw_private = palloc0(sizeof(sqliteFdwPlanState));
	baserel->fdw_private = (void *) fdw_private;

	/* initialize required state in fdw_private */
	baserel->rows = GetEstimatedRows(foreigntableid);
}

static void
sqliteGetForeignPaths(PlannerInfo *root,
						 RelOptInfo *baserel,
						 Oid foreigntableid)
{
	/*
	 * Create possible access paths for a scan on a foreign table. This is
	 * called during query planning. The parameters are the same as for
	 * GetForeignRelSize, which has already been called.
	 *
	 * This function must generate at least one access path (ForeignPath node)
	 * for a scan on the foreign table and must call add_path to add each such
	 * path to baserel->pathlist. It's recommended to use
	 * create_foreignscan_path to build the ForeignPath nodes. The function
	 * can generate multiple access paths, e.g., a path which has valid
	 * pathkeys to represent a pre-sorted result. Each access path must
	 * contain cost estimates, and can contain any FDW-private information
	 * that is needed to identify the specific scan method intended.
	 */

	/*
	 * sqliteFdwPlanState *fdw_private = baserel->fdw_private;
	 */

	Cost		startup_cost,
				total_cost;

	elog(DEBUG1,"entering function %s",__func__);

	startup_cost = 0;
	total_cost = startup_cost + baserel->rows;

	/* Create a ForeignPath node and add it as only possible path */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
#if (PG_VERSION_NUM >= 90600)
									 NULL,		/* default pathtarget */
#endif
									 baserel->rows,
									 startup_cost,
									 total_cost,
									 NIL,		/* no pathkeys */
									 NULL,		/* no outer rel either */
# if (PG_VERSION_NUM >= 90500)
									 NULL,		/* no extra plan */
#endif
									 NIL));		/* no fdw_private data */
}



static ForeignScan *
sqliteGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
#if (PG_VERSION_NUM >= 90500)
						List *scan_clauses,
						Plan *outer_plan)
#else
						List *scan_clauses)
#endif
{
	/*
	 * Create a ForeignScan plan node from the selected foreign access path.
	 * This is called at the end of query planning. The parameters are as for
	 * GetForeignRelSize, plus the selected ForeignPath (previously produced
	 * by GetForeignPaths), the target list to be emitted by the plan node,
	 * and the restriction clauses to be enforced by the plan node.
	 *
	 * This function must create and return a ForeignScan plan node; it's
	 * recommended to use make_foreignscan to build the ForeignScan node.
	 *
	 */

	Index		scan_relid = baserel->relid;

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check. So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */

	elog(DEBUG1,"entering function %s",__func__);

	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Create the ForeignScan node */
	return make_foreignscan(tlist,
							scan_clauses,	/* restriction clauses all local */
							scan_relid,
							NIL,	/* no expressions to evaluate */
#if (PG_VERSION_NUM >= 90500)
							NIL,		/* no private state either */
							NIL,		/* no scan target list*/
							NIL,		/* no remote clauses */
							NULL);		/* no outer plan */
#else
							NIL);		/* no private state either */
#endif
}
#else
static FdwPlan *
sqlitePlanForeignScan(Oid foreigntableid, PlannerInfo *root, RelOptInfo *baserel)
{
	FdwPlan		*fdwplan;

	/* Construct FdwPlan with cost estimates. */
	fdwplan = makeNode(FdwPlan);


	baserel->rows = GetEstimatedRows(foreigntableid);
	// TODO: find a way to estimate the average row size
	//baserel->width = ?;
	baserel->tuples = baserel->rows;

	fdwplan->startup_cost = 10;
	fdwplan->total_cost = baserel->rows + fdwplan->startup_cost;
	fdwplan->fdw_private = NIL;	/* not used */

	return fdwplan;
}
#endif


static void
sqliteBeginForeignScan(ForeignScanState *node,
						  int eflags)
{
	/*
	 * Begin executing a foreign scan. This is called during executor startup.
	 * It should perform any initialization needed before the scan can start,
	 * but not start executing the actual scan (that should be done upon the
	 * first call to IterateForeignScan). The ForeignScanState node has
	 * already been created, but its fdw_state field is still NULL.
	 * Information about the table to scan is accessible through the
	 * ForeignScanState node (in particular, from the underlying ForeignScan
	 * plan node, which contains any FDW-private information provided by
	 * GetForeignPlan). eflags contains flag bits describing the executor's
	 * operating mode for this plan node.
	 *
	 * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
	 * should not perform any externally-visible actions; it should only do
	 * the minimum required to make the node state valid for
	 * ExplainForeignScan and EndForeignScan.
	 *
	 */
	SQLiteFdwExecutionState  *festate;
	char					 *database;
	char					 *table;
	char                     *query;
    size_t                   len;

	elog(DEBUG1,"entering function %s",__func__);

	sqliteGetOptions(RelationGetRelid(node->ss.ss_currentRelation), &database, &table);

	/* Build the query */
    len = strlen(table) + 15;
    query = (char *) palloc0(len);
    snprintf(query, len, "SELECT * FROM %s", table);

	/* Stash away the state info we have already */
	festate = (SQLiteFdwExecutionState *) palloc(sizeof(SQLiteFdwExecutionState));
	node->fdw_state = (void *) festate;
	festate->conn = GetConnection(RelationGetRelid(node->ss.ss_currentRelation));
	festate->result = NULL;
	festate->query = query;
}


static TupleTableSlot *
sqliteIterateForeignScan(ForeignScanState *node)
{
	/*
	 * Fetch one row from the foreign source, returning it in a tuple table
	 * slot (the node's ScanTupleSlot should be used for this purpose). Return
	 * NULL if no more rows are available. The tuple table slot infrastructure
	 * allows either a physical or virtual tuple to be returned; in most cases
	 * the latter choice is preferable from a performance standpoint. Note
	 * that this is called in a short-lived memory context that will be reset
	 * between invocations. Create a memory context in BeginForeignScan if you
	 * need longer-lived storage, or use the es_query_cxt of the node's
	 * EState.
	 *
	 * The rows returned must match the column signature of the foreign table
	 * being scanned. If you choose to optimize away fetching columns that are
	 * not needed, you should insert nulls in those column positions.
	 *
	 * Note that PostgreSQL's executor doesn't care whether the rows returned
	 * violate any NOT NULL constraints that were defined on the foreign table
	 * columns — but the planner does care, and may optimize queries
	 * incorrectly if NULL values are present in a column declared not to
	 * contain them. If a NULL value is encountered when the user has declared
	 * that none should be present, it may be appropriate to raise an error
	 * (just as you would need to do in the case of a data type mismatch).
	 */

	char        **values;
	HeapTuple   tuple;
	int         x;
    const char  *pzTail;
    int         rc;

	SQLiteFdwExecutionState *festate = (SQLiteFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	/* Execute the query, if required */
	if (!festate->result)
	{
		rc = sqlite3_prepare(festate->conn, festate->query, -1, &festate->result, &pzTail);
		if (rc!=SQLITE_OK) {
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				errmsg("SQL error during prepare: %s", sqlite3_errmsg(festate->conn))
				));
		}
	}

	elog(DEBUG1,"entering function %s",__func__);

	ExecClearTuple(slot);

	/* get the next record, if any, and fill in the slot */
	if (sqlite3_step(festate->result) == SQLITE_ROW)
	{
		/* Build the tuple */
		values = (char **) palloc(sizeof(char *) * sqlite3_column_count(festate->result));

		for (x = 0; x < sqlite3_column_count(festate->result); x++)
		{
			values[x] = (char *) sqlite3_column_text(festate->result, x);
		}

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att), values);
		ExecStoreTuple(tuple, slot, InvalidBuffer, false);
	}

	/* then return the slot */
	return slot;
}


static void
sqliteReScanForeignScan(ForeignScanState *node)
{
	/*
	 * Restart the scan from the beginning. Note that any parameters the scan
	 * depends on may have changed value, so the new scan does not necessarily
	 * return exactly the same rows.
	 */

	elog(DEBUG1,"entering function %s",__func__);

}


static void
sqliteEndForeignScan(ForeignScanState *node)
{
	/*
	 * End the scan and release resources. It is normally not important to
	 * release palloc'd memory, but for example open files and connections to
	 * remote servers should be cleaned up.
	 */

	SQLiteFdwExecutionState *festate = (SQLiteFdwExecutionState *) node->fdw_state;

	elog(DEBUG1,"entering function %s",__func__);

	if (festate->result)
	{
		sqlite3_finalize(festate->result);
		festate->result = NULL;
	}

	if (festate->conn)
	{
		ReleaseConnection(festate->conn);
		festate->conn = NULL;
	}

	if (festate->query)
	{
		pfree(festate->query);
		festate->query = 0;
	}

}


#if (PG_VERSION_NUM >= 90300)
static void
sqliteAddForeignUpdateTargets(Query *parsetree,
								 RangeTblEntry *target_rte,
								 Relation target_relation)
{
	/*
	 * UPDATE and DELETE operations are performed against rows previously
	 * fetched by the table-scanning functions. The FDW may need extra
	 * information, such as a row ID or the values of primary-key columns, to
	 * ensure that it can identify the exact row to update or delete. To
	 * support that, this function can add extra hidden, or "junk", target
	 * columns to the list of columns that are to be retrieved from the
	 * foreign table during an UPDATE or DELETE.
	 *
	 * To do that, add TargetEntry items to parsetree->targetList, containing
	 * expressions for the extra values to be fetched. Each such entry must be
	 * marked resjunk = true, and must have a distinct resname that will
	 * identify it at execution time. Avoid using names matching ctidN or
	 * wholerowN, as the core system can generate junk columns of these names.
	 *
	 * This function is called in the rewriter, not the planner, so the
	 * information available is a bit different from that available to the
	 * planning routines. parsetree is the parse tree for the UPDATE or DELETE
	 * command, while target_rte and target_relation describe the target
	 * foreign table.
	 *
	 * If the AddForeignUpdateTargets pointer is set to NULL, no extra target
	 * expressions are added. (This will make it impossible to implement
	 * DELETE operations, though UPDATE may still be feasible if the FDW
	 * relies on an unchanging primary key to identify rows.)
	 */

	elog(DEBUG1,"entering function %s",__func__);

}


static List *
sqlitePlanForeignModify(PlannerInfo *root,
						   ModifyTable *plan,
						   Index resultRelation,
						   int subplan_index)
{
	/*
	 * Perform any additional planning actions needed for an insert, update,
	 * or delete on a foreign table. This function generates the FDW-private
	 * information that will be attached to the ModifyTable plan node that
	 * performs the update action. This private information must have the form
	 * of a List, and will be delivered to BeginForeignModify during the
	 * execution stage.
	 *
	 * root is the planner's global information about the query. plan is the
	 * ModifyTable plan node, which is complete except for the fdwPrivLists
	 * field. resultRelation identifies the target foreign table by its
	 * rangetable index. subplan_index identifies which target of the
	 * ModifyTable plan node this is, counting from zero; use this if you want
	 * to index into plan->plans or other substructure of the plan node.
	 *
	 * If the PlanForeignModify pointer is set to NULL, no additional
	 * plan-time actions are taken, and the fdw_private list delivered to
	 * BeginForeignModify will be NIL.
	 */

	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	StringInfoData sql;
	List	   *targetAttrs = NIL;

	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = heap_open(rte->relid, NoLock);

	/*
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, we transmit only columns that were explicitly
	 * targets of the UPDATE, so as to avoid unnecessary data transmission.
	 * (We can't do that for INSERT since we would miss sending default values
	 * for columns not listed in the source statement.)
	 */
	if (operation == CMD_INSERT)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = tupdesc->attrs[attnum - 1];

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}

	/*
	 * No RETURNING list please.
	 */
	if (plan->returningLists)
		elog(ERROR, "RETURNING not supported");

#if (PG_VERSION_NUM >= 90500)
	/*
	 * No ON CONFLICT specification please.
	 */
	if (plan->onConflictAction != ONCONFLICT_NONE)
		elog(ERROR, "ON CONFLICT specification not supported");
#endif

	/*
	 * Construct the SQL command string.
	 */
	switch (operation)
	{
		case CMD_INSERT:
			deparseInsertSql(&sql, root, resultRelation, rel, targetAttrs);
			break;
		case CMD_UPDATE:
		case CMD_DELETE:
			elog(ERROR, "UPDATE and DELETE not supported");
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	heap_close(rel, NoLock);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwModifyPrivateIndex, above.
	 */
	return list_make2(makeString(sql.data), targetAttrs);
}


static void
sqliteBeginForeignModify(ModifyTableState *mtstate,
							ResultRelInfo *resultRelInfo,
							List *fdw_private,
							int subplan_index,
							int eflags)
{
	/*
	 * Begin executing a foreign table modification operation. This routine is
	 * called during executor startup. It should perform any initialization
	 * needed prior to the actual table modifications. Subsequently,
	 * ExecForeignInsert, ExecForeignUpdate or ExecForeignDelete will be
	 * called for each tuple to be inserted, updated, or deleted.
	 *
	 * mtstate is the overall state of the ModifyTable plan node being
	 * executed; global data about the plan and execution state is available
	 * via this structure. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. (The ri_FdwState field of ResultRelInfo is
	 * available for the FDW to store any private state it needs for this
	 * operation.) fdw_private contains the private data generated by
	 * PlanForeignModify, if any. subplan_index identifies which target of the
	 * ModifyTable plan node this is. eflags contains flag bits describing the
	 * executor's operating mode for this plan node.
	 *
	 * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
	 * should not perform any externally-visible actions; it should only do
	 * the minimum required to make the node state valid for
	 * ExplainForeignModify and EndForeignModify.
	 *
	 * If the BeginForeignModify pointer is set to NULL, no action is taken
	 * during executor startup.
	 */

	SqliteFdwModifyState *fmstate;
	EState	   *estate = mtstate->ps.state;
	CmdType		operation = mtstate->operation;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	AttrNumber	n_params;
	Oid			typefnoid;
	bool		isvarlena;
	ListCell   *lc;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Begin constructing SqliteFdwModifyState. */
	fmstate = (SqliteFdwModifyState *) palloc0(sizeof(SqliteFdwModifyState));
	fmstate->rel = rel;

	/* Our connection to database. */
	fmstate->conn = GetConnection(RelationGetRelid(rel));

	/* Deconstruct fdw_private data. */
	fmstate->query = strVal(list_nth(fdw_private,
									 FdwModifyPrivateUpdateSql));
	fmstate->target_attrs = (List *) list_nth(fdw_private,
											  FdwModifyPrivateTargetAttnums);

	/* Create context for per-tuple temp workspace. */
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "sqlite_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);

	/* Prepare for output conversion of parameters used when binding. */
	n_params = list_length(fmstate->target_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;

	if (operation == CMD_INSERT)
	{
		/* Set up for remaining transmittable parameters */
		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = RelationGetDescr(rel)->attrs[attnum - 1];

			Assert(!attr->attisdropped);

			getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
			fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
			fmstate->p_nums++;
		}
	}

	Assert(fmstate->p_nums <= n_params);

	resultRelInfo->ri_FdwState = fmstate;

}


static TupleTableSlot *
sqliteExecForeignInsert(EState *estate,
						   ResultRelInfo *resultRelInfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot)
{
	/*
	 * Insert one tuple into the foreign table. estate is global execution
	 * state for the query. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. slot contains the tuple to be inserted; it will
	 * match the rowtype definition of the foreign table. planSlot contains
	 * the tuple that was generated by the ModifyTable plan node's subplan; it
	 * differs from slot in possibly containing additional "junk" columns.
	 * (The planSlot is typically of little interest for INSERT cases, but is
	 * provided for completeness.)
	 *
	 * The return value is either a slot containing the data that was actually
	 * inserted (this might differ from the data supplied, for example as a
	 * result of trigger actions), or NULL if no row was actually inserted
	 * (again, typically as a result of triggers). The passed-in slot can be
	 * re-used for this purpose.
	 *
	 * The data in the returned slot is used only if the INSERT query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignInsert pointer is set to NULL, attempts to insert
	 * into the foreign table will fail with an error message.
	 *
	 */

	SqliteFdwModifyState *fmstate = (SqliteFdwModifyState *) resultRelInfo->ri_FdwState;
	const char **p_values;
    const char  *pzTail;
	int		i, rc;

	/* Convert parameters needed by prepared statement to text form */
	p_values = convert_prep_stmt_params(fmstate, slot);

	/* Prepare if not done yet. */
	if (fmstate->stmt == NULL)
	{
		rc = sqlite3_prepare(fmstate->conn, fmstate->query, -1, &fmstate->stmt, &pzTail);
		if (rc!=SQLITE_OK)
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				errmsg("SQL error during prepare: %s", sqlite3_errmsg(fmstate->conn))));
	}

	/* Bind using values of current input tuple */
	for (i = 0; i < fmstate->p_nums; i++)
	{
		if (p_values[i])
		{
			int		len = strlen(p_values[i]);

			rc = sqlite3_bind_text(fmstate->stmt, i+1, p_values[i], len, SQLITE_STATIC);
			if (rc!=SQLITE_OK)
				ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					errmsg("SQL error during bind: %s", sqlite3_errmsg(fmstate->conn))));
		}
		else
		{
			rc = sqlite3_bind_null(fmstate->stmt, i+1);
			if (rc!=SQLITE_OK)
				ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					errmsg("SQL error during bind: %s", sqlite3_errmsg(fmstate->conn))));
		}
	}

	/* Get the result, and check for success. */
	if (sqlite3_step(fmstate->stmt) != SQLITE_DONE)
		ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					errmsg("SQL error during step: %s", sqlite3_errmsg(fmstate->conn))));

	if (sqlite3_clear_bindings(fmstate->stmt) != SQLITE_OK)
		ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					errmsg("SQL error during clear bindings: %s", sqlite3_errmsg(fmstate->conn))));

	if (sqlite3_reset(fmstate->stmt) != SQLITE_OK)
		ereport(ERROR,
					(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
					errmsg("SQL error during reset: %s", sqlite3_errmsg(fmstate->conn))));

	MemoryContextReset(fmstate->temp_cxt);

	return slot;
}


static TupleTableSlot *
sqliteExecForeignUpdate(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot)
{
	/*
	 * Update one tuple in the foreign table. estate is global execution state
	 * for the query. rinfo is the ResultRelInfo struct describing the target
	 * foreign table. slot contains the new data for the tuple; it will match
	 * the rowtype definition of the foreign table. planSlot contains the
	 * tuple that was generated by the ModifyTable plan node's subplan; it
	 * differs from slot in possibly containing additional "junk" columns. In
	 * particular, any junk columns that were requested by
	 * AddForeignUpdateTargets will be available from this slot.
	 *
	 * The return value is either a slot containing the row as it was actually
	 * updated (this might differ from the data supplied, for example as a
	 * result of trigger actions), or NULL if no row was actually updated
	 * (again, typically as a result of triggers). The passed-in slot can be
	 * re-used for this purpose.
	 *
	 * The data in the returned slot is used only if the UPDATE query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignUpdate pointer is set to NULL, attempts to update the
	 * foreign table will fail with an error message.
	 *
	 */

	elog(DEBUG1,"entering function %s",__func__);

	return slot;
}


static TupleTableSlot *
sqliteExecForeignDelete(EState *estate,
						   ResultRelInfo *rinfo,
						   TupleTableSlot *slot,
						   TupleTableSlot *planSlot)
{
	/*
	 * Delete one tuple from the foreign table. estate is global execution
	 * state for the query. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. slot contains nothing useful upon call, but can
	 * be used to hold the returned tuple. planSlot contains the tuple that
	 * was generated by the ModifyTable plan node's subplan; in particular, it
	 * will carry any junk columns that were requested by
	 * AddForeignUpdateTargets. The junk column(s) must be used to identify
	 * the tuple to be deleted.
	 *
	 * The return value is either a slot containing the row that was deleted,
	 * or NULL if no row was deleted (typically as a result of triggers). The
	 * passed-in slot can be used to hold the tuple to be returned.
	 *
	 * The data in the returned slot is used only if the DELETE query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignDelete pointer is set to NULL, attempts to delete
	 * from the foreign table will fail with an error message.
	 */

	elog(DEBUG1,"entering function %s",__func__);

	return slot;
}


static void
sqliteEndForeignModify(EState *estate,
						  ResultRelInfo *resultRelInfo)
{
	/*
	 * End the table update and release resources. It is normally not
	 * important to release palloc'd memory, but for example open files and
	 * connections to remote servers should be cleaned up.
	 *
	 * If the EndForeignModify pointer is set to NULL, no action is taken
	 * during executor shutdown.
	 */
	SqliteFdwModifyState *fmstate = (SqliteFdwModifyState *) resultRelInfo->ri_FdwState;
	int		rc;

	/* If fmstate is NULL, we are in EXPLAIN; nothing to do */
	if (fmstate == NULL)
		return;

	if (fmstate->stmt)
	{
		rc = sqlite3_finalize(fmstate->stmt);
		if (rc != SQLITE_OK)
			elog(ERROR, "could not finalize statement");
		fmstate->stmt = NULL;
	}

	/* Release remote connection */
	ReleaseConnection(fmstate->conn);
	fmstate->conn = NULL;
}

static void
sqliteExplainForeignModify(ModifyTableState *mtstate,
							  ResultRelInfo *rinfo,
							  List *fdw_private,
							  int subplan_index,
							  struct ExplainState *es)
{
	/*
	 * Print additional EXPLAIN output for a foreign table update. This
	 * function can call ExplainPropertyText and related functions to add
	 * fields to the EXPLAIN output. The flag fields in es can be used to
	 * determine what to print, and the state of the ModifyTableState node can
	 * be inspected to provide run-time statistics in the EXPLAIN ANALYZE
	 * case. The first four arguments are the same as for BeginForeignModify.
	 *
	 * If the ExplainForeignModify pointer is set to NULL, no additional
	 * information is printed during EXPLAIN.
	 */

	elog(DEBUG1,"entering function %s",__func__);

}

/*
 * deparse remote INSERT statement
 *
 * The statement text is appended to buf, and we also create an integer List
 * of the columns being retrieved by RETURNING (if any), which is returned
 * to *retrieved_attrs.
 */
static void
deparseInsertSql(StringInfo buf, PlannerInfo *root,
				 Index rtindex, Relation rel, List *targetAttrs)
{
	AttrNumber	pindex;
	bool		first;
	ListCell   *lc;

	appendStringInfoString(buf, "INSERT INTO ");
	deparseRelation(buf, rel);

	if (targetAttrs)
	{
		appendStringInfoChar(buf, '(');

		first = true;
		foreach(lc, targetAttrs)
		{
			int			attnum = lfirst_int(lc);

			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;

			deparseColumnRef(buf, rtindex, attnum, root, false);
		}

		appendStringInfoString(buf, ") VALUES (");

		pindex = 1;
		first = true;
		foreach(lc, targetAttrs)
		{
			if (!first)
				appendStringInfoString(buf, ", ");
			first = false;

			appendStringInfo(buf, "$%d", pindex);
			pindex++;
		}

		appendStringInfoChar(buf, ')');
	}
	else
		appendStringInfoString(buf, " DEFAULT VALUES");
}

/*
 * Append remote name of specified foreign table to buf.
 * Use value of table_name FDW option (if any) instead of relation's name.
 * Similarly, schema_name FDW option overrides schema name.
 */
static void
deparseRelation(StringInfo buf, Relation rel)
{
	ForeignTable *table;
	const char *relname = NULL;
	ListCell   *lc;

	/* obtain additional catalog information. */
	table = GetForeignTable(RelationGetRelid(rel));

	/*
	 * Use value of FDW options if any, instead of the name of object itself.
	 */
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "table") == 0)
			relname = defGetString(def);
	}

	/*
	 * Note: sqlite has no schemas as far as I know.
	 */
	if (relname == NULL)
		relname = RelationGetRelationName(rel);

	appendStringInfo(buf, "%s", quote_identifier(relname));
}

/*
 * Construct name to use for given column, and emit it into buf.
 * If it has a column_name FDW option, use that instead of attribute name.
 *
 * If qualify_col is true, qualify column name with the alias of relation.
 */
static void
deparseColumnRef(StringInfo buf, int varno, int varattno, PlannerInfo *root,
				 bool qualify_col)
{
	RangeTblEntry *rte;

	if (varattno == SelfItemPointerAttributeNumber)
	{
		/* We support fetching the remote side's CTID. */
		if (qualify_col)
			ADD_REL_QUALIFIER(buf, varno);
		appendStringInfoString(buf, "ctid");
	}
	else if (varattno < 0)
	{
		/*
		 * All other system attributes are fetched as 0, except for table OID,
		 * which is fetched as the local table OID.  However, we must be
		 * careful; the table could be beneath an outer join, in which case it
		 * must go to NULL whenever the rest of the row does.
		 */
		Oid			fetchval = 0;

		if (varattno == TableOidAttributeNumber)
		{
			rte = planner_rt_fetch(varno, root);
			fetchval = rte->relid;
		}

		if (qualify_col)
		{
			appendStringInfoString(buf, "CASE WHEN (");
			ADD_REL_QUALIFIER(buf, varno);
			appendStringInfo(buf, "*)::pg_catalog.text IS NOT NULL THEN %u END", fetchval);
		}
		else
			appendStringInfo(buf, "%u", fetchval);
	}
	else if (varattno == 0)
	{
		/* Whole row reference */
		Relation	rel;
		Bitmapset  *attrs_used;

		/* Required only to be passed down to deparseTargetList(). */
		List	   *retrieved_attrs;

		/* Get RangeTblEntry from array in PlannerInfo. */
		rte = planner_rt_fetch(varno, root);

		/*
		 * The lock on the relation will be held by upper callers, so it's
		 * fine to open it with no lock here.
		 */
		rel = heap_open(rte->relid, NoLock);

		/*
		 * The local name of the foreign table can not be recognized by the
		 * foreign server and the table it references on foreign server might
		 * have different column ordering or different columns than those
		 * declared locally. Hence we have to deparse whole-row reference as
		 * ROW(columns referenced locally). Construct this by deparsing a
		 * "whole row" attribute.
		 */
		attrs_used = bms_add_member(NULL,
									0 - FirstLowInvalidHeapAttributeNumber);

		/*
		 * In case the whole-row reference is under an outer join then it has
		 * to go NULL whenver the rest of the row goes NULL. Deparsing a join
		 * query would always involve multiple relations, thus qualify_col
		 * would be true.
		 */
		if (qualify_col)
		{
			appendStringInfoString(buf, "CASE WHEN (");
			ADD_REL_QUALIFIER(buf, varno);
			appendStringInfo(buf, "*)::pg_catalog.text IS NOT NULL THEN ");
		}

		appendStringInfoString(buf, "ROW(");
		deparseTargetList(buf, root, varno, rel, false, attrs_used, qualify_col,
						  &retrieved_attrs);
		appendStringInfoString(buf, ")");

		/* Complete the CASE WHEN statement started above. */
		if (qualify_col)
			appendStringInfo(buf, " END");

		heap_close(rel, NoLock);
		bms_free(attrs_used);
	}
	else
	{
		char	   *colname = NULL;
		List	   *options;
		ListCell   *lc;

		/* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR. */
		Assert(!IS_SPECIAL_VARNO(varno));

		/* Get RangeTblEntry from array in PlannerInfo. */
		rte = planner_rt_fetch(varno, root);

		/*
		 * If it's a column of a foreign table, and it has the column_name FDW
		 * option, use that value.
		 */
		options = GetForeignColumnOptions(rte->relid, varattno);
		foreach(lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "column_name") == 0)
			{
				colname = defGetString(def);
				break;
			}
		}

		/*
		 * If it's a column of a regular table or it doesn't have column_name
		 * FDW option, use attribute name.
		 */
		if (colname == NULL)
			colname = get_relid_attribute_name(rte->relid, varattno);

		if (qualify_col)
			ADD_REL_QUALIFIER(buf, varno);

		appendStringInfoString(buf, quote_identifier(colname));
	}
}

/*
 * Emit a target list that retrieves the columns specified in attrs_used.
 * This is used for both SELECT and RETURNING targetlists; the is_returning
 * parameter is true only for a RETURNING targetlist.
 *
 * The tlist text is appended to buf, and we also create an integer List
 * of the columns being retrieved, which is returned to *retrieved_attrs.
 *
 * If qualify_col is true, add relation alias before the column name.
 */
static void
deparseTargetList(StringInfo buf,
				  PlannerInfo *root,
				  Index rtindex,
				  Relation rel,
				  bool is_returning,
				  Bitmapset *attrs_used,
				  bool qualify_col,
				  List **retrieved_attrs)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	bool		have_wholerow;
	bool		first;
	int			i;

	*retrieved_attrs = NIL;

	/* If there's a whole-row reference, we'll need all the columns. */
	have_wholerow = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
								  attrs_used);

	first = true;
	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i - 1];

		/* Ignore dropped attributes. */
		if (attr->attisdropped)
			continue;

		if (have_wholerow ||
			bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
		{
			if (!first)
				appendStringInfoString(buf, ", ");
			else if (is_returning)
				appendStringInfoString(buf, " RETURNING ");
			first = false;

			deparseColumnRef(buf, rtindex, i, root, qualify_col);

			*retrieved_attrs = lappend_int(*retrieved_attrs, i);
		}
	}

	/*
	 * Add ctid if needed.  We currently don't support retrieving any other
	 * system columns.
	 */
	if (bms_is_member(SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber,
					  attrs_used))
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		else if (is_returning)
			appendStringInfoString(buf, " RETURNING ");
		first = false;

		if (qualify_col)
			ADD_REL_QUALIFIER(buf, rtindex);
		appendStringInfoString(buf, "ctid");

		*retrieved_attrs = lappend_int(*retrieved_attrs,
									   SelfItemPointerAttributeNumber);
	}

	/* Don't generate bad syntax if no undropped columns */
	if (first && !is_returning)
		appendStringInfoString(buf, "NULL");
}

/*
 * convert_prep_stmt_params
 *		Create array of text strings representing parameter values
 *
 * tupleid is ctid to send, or NULL if none
 * slot is slot to get remaining parameters from, or NULL if none
 *
 * Data is constructed in temp_cxt; caller should reset that after use.
 */
static const char **
convert_prep_stmt_params(SqliteFdwModifyState *fmstate,
						 TupleTableSlot *slot)
{
	const char **p_values;
	int			pindex = 0;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

	p_values = (const char **) palloc(sizeof(char *) * fmstate->p_nums);

	/* get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		ListCell   *lc;

		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Datum		value;
			bool		isnull;

			value = slot_getattr(slot, attnum, &isnull);
			if (isnull)
				p_values[pindex] = NULL;
			else
				p_values[pindex] = OutputFunctionCall(&fmstate->p_flinfo[pindex],
													  value);
			pindex++;
		}
	}

	Assert(pindex == fmstate->p_nums);

	MemoryContextSwitchTo(oldcontext);

	return p_values;
}
#endif


static void
sqliteExplainForeignScan(ForeignScanState *node,
							struct ExplainState *es)
{
	/*
	 * Print additional EXPLAIN output for a foreign table scan. This function
	 * can call ExplainPropertyText and related functions to add fields to the
	 * EXPLAIN output. The flag fields in es can be used to determine what to
	 * print, and the state of the ForeignScanState node can be inspected to
	 * provide run-time statistics in the EXPLAIN ANALYZE case.
	 *
	 * If the ExplainForeignScan pointer is set to NULL, no additional
	 * information is printed during EXPLAIN.
	 */
	sqlite3                  *db;
	sqlite3_stmt             *result;
	char                     *query;
    size_t                   len;
    int                      rc;
    const char  *pzTail;
	SQLiteFdwExecutionState *festate = (SQLiteFdwExecutionState *) node->fdw_state;

	elog(DEBUG1,"entering function %s",__func__);

	/* Show the query (only if VERBOSE) */
	if (es->verbose)
	{
		/* show query */
		ExplainPropertyText("sqlite query", festate->query, es);
	}

	/* Connect to the server */
	db = GetConnection(RelationGetRelid(node->ss.ss_currentRelation));

	/* Build the query */
    len = strlen(festate->query) + 20;
    query = (char *)palloc(len);
    snprintf(query, len, "EXPLAIN QUERY PLAN %s", festate->query);

    /* Execute the query */
	rc = sqlite3_prepare(db, query, -1, &result, &pzTail);
	if (rc!=SQLITE_OK)
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			errmsg("SQL error during prepare: %s", sqlite3_errmsg(db))));

	/* get the next record, if any, and fill in the slot */
	while (sqlite3_step(result) == SQLITE_ROW)
	{
		/* I don't keep the three first columns;
		   it could be a good idea to add that later */
		//for (x = 0; x < sqlite3_column_count(festate->result); x++)
		//{
			ExplainPropertyText("sqlite plan", (char *) sqlite3_column_text(result, 3), es);
		//}
	}

	// Free the query results
	sqlite3_finalize(result);

	// Close temporary connection
	ReleaseConnection(db);

}

#if (PG_VERSION_NUM >= 90200)
static bool
sqliteAnalyzeForeignTable(Relation relation,
							 AcquireSampleRowsFunc *func,
							 BlockNumber *totalpages)
{
	/* ----
	 * This function is called when ANALYZE is executed on a foreign table. If
	 * the FDW can collect statistics for this foreign table, it should return
	 * true, and provide a pointer to a function that will collect sample rows
	 * from the table in func, plus the estimated size of the table in pages
	 * in totalpages. Otherwise, return false.
	 *
	 * If the FDW does not support collecting statistics for any tables, the
	 * AnalyzeForeignTable pointer can be set to NULL.
	 *
	 * If provided, the sample collection function must have the signature:
	 *
	 *	  int
	 *	  AcquireSampleRowsFunc (Relation relation, int elevel,
	 *							 HeapTuple *rows, int targrows,
	 *							 double *totalrows,
	 *							 double *totaldeadrows);
	 *
	 * A random sample of up to targrows rows should be collected from the
	 * table and stored into the caller-provided rows array. The actual number
	 * of rows collected must be returned. In addition, store estimates of the
	 * total numbers of live and dead rows in the table into the output
	 * parameters totalrows and totaldeadrows. (Set totaldeadrows to zero if
	 * the FDW does not have any concept of dead rows.)
	 * ----
	 */

	elog(DEBUG1,"entering function %s",__func__);

	return false;
}
#endif

static int
GetEstimatedRows(Oid foreigntableid)
{
	sqlite3		   *db;
	sqlite3_stmt   *result;
	char		   *database;
	char		   *table;
	char		   *query;
    size_t			len;
    int				rc;
    const char	   *pzTail;

	int rows = 0;

	sqliteGetOptions(foreigntableid, &database, &table);

	/* Connect to the server */
	db = GetConnection(foreigntableid);

	/* Check that the sqlite_stat1 table exists */
	rc = sqlite3_prepare(db, "SELECT 1 FROM sqlite_master WHERE name='sqlite_stat1'", -1, &result, &pzTail);
	if (rc!=SQLITE_OK)
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			errmsg("SQL error during prepare: %s", sqlite3_errmsg(db))
			));

	if (sqlite3_step(result) != SQLITE_ROW)
	{
		ereport(WARNING,
			(errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
			errmsg("The sqlite3 database has not been analyzed."),
			errhint("Run ANALYZE on table \"%s\", database \"%s\".", table, database)
			));
		rows = 10;
	}

	// Free the query results
	sqlite3_finalize(result);

	if (rows == 0)
	{
		/* Build the query */
	    len = strlen(table) + 60;
	    query = (char *)palloc(len);
	    snprintf(query, len, "SELECT stat FROM sqlite_stat1 WHERE tbl='%s' AND idx IS NULL", table);
	    elog(LOG, "query:%s", query);

	    /* Execute the query */
		rc = sqlite3_prepare(db, query, -1, &result, &pzTail);
		if (rc!=SQLITE_OK)
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				errmsg("SQL error during prepare: %s", sqlite3_errmsg(db))
				));

		/* get the next record, if any, and fill in the slot */
		if (sqlite3_step(result) == SQLITE_ROW)
		{
			rows = sqlite3_column_int(result, 0);
		}

		// Free the query results
		sqlite3_finalize(result);
	}
	else
	{
		/*
		 * The sqlite database doesn't have any statistics.
		 * There's not much we can do, except using a hardcoded one.
		 * Using a foreign table option might be a better solution.
		 */
		rows = DEFAULT_ESTIMATED_LINES;
	}

	// Close temporary connection
	ReleaseConnection(db);

	return rows;
}

static bool
file_exists(const char *name)
{
    struct stat st;

    AssertArg(name != NULL);

    if (stat(name, &st) == 0)
        return S_ISDIR(st.st_mode) ? false : true;
    else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not access file \"%s\": %m", name)));

    return false;
}

/*
 * Connection cache (inited on the first use)
 */
static HTAB *ConnectionHash = NULL;
static bool xact_got_connection = false;

typedef Oid ConnCacheKey;
typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key, must be first */
	sqlite3	   *conn;
	int			xact_depth;		/* 0 = no xact open, 1 = main xact open, 2 =
								 * one level of subxact open, etc. */
	bool		have_error;		/* have any subxacts aborted in this xact? */
} ConnCacheEntry;

static void
do_sql_command(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *stmt;
    const char  *pzTail;
	int		rc;

	rc = sqlite3_prepare(db, sql, -1, &stmt, &pzTail);
	if (rc!=SQLITE_OK)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				errmsg("SQL error during prepare: %s", sqlite3_errmsg(db))));

	/* Get the result, and check for success. */
	if (sqlite3_step(stmt) != SQLITE_DONE)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				errmsg("SQL error during step: %s", sqlite3_errmsg(db))));
}

/*
 * sqlitefdw_xact_callback --- cleanup at main-transaction end.
 */
static void
sqlitefdw_xact_callback(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	/*
	 * Scan all connection cache entries to find open remote transactions, and
	 * close them.
	 */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore cache entry if no open connection right now */
		if (entry->conn == NULL)
			continue;

		/* If it has an open remote transaction, try to close it */
		if (entry->xact_depth > 0)
		{
			elog(DEBUG3, "closing remote transaction on connection %p",
				 entry->conn);

			switch (event)
			{
#if (PG_VERSION_NUM >= 90500)
				case XACT_EVENT_PARALLEL_PRE_COMMIT:
#endif

#if (PG_VERSION_NUM >= 90300)
				case XACT_EVENT_PRE_COMMIT:
					/* Commit all remote transactions during pre-commit */
					do_sql_command(entry->conn, "COMMIT TRANSACTION");

					entry->have_error = false;
					break;
				case XACT_EVENT_PRE_PREPARE:

					/*
					 * We disallow remote transactions that modified anything,
					 * since it's not very reasonable to hold them open until
					 * the prepared transaction is committed.  For the moment,
					 * throw error unconditionally; later we might allow
					 * read-only cases.  Note that the error will cause us to
					 * come right back here with event == XACT_EVENT_ABORT, so
					 * we'll clean up the connection state at that point.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot prepare a transaction that modified remote tables")));
					break;
#endif

#if (PG_VERSION_NUM >= 90500)
				case XACT_EVENT_PARALLEL_COMMIT:
#endif
				case XACT_EVENT_COMMIT:
				case XACT_EVENT_PREPARE:
					/* Pre-commit should have closed the open transaction */
					elog(ERROR, "missed cleaning up connection during pre-commit");
					break;
#if (PG_VERSION_NUM >= 90500)
				case XACT_EVENT_PARALLEL_ABORT:
#endif
				case XACT_EVENT_ABORT:
					/* Assume we might have lost track of prepared statements */
					entry->have_error = true;

					/* If we're aborting, abort all remote transactions too */
					do_sql_command(entry->conn, "ROLLBACK TRANSACTION");
					break;
			}
		}

		/* Reset state to show we're out of a transaction */
		entry->xact_depth = 0;
	}

	/*
	 * Regardless of the event type, we can now mark ourselves as out of the
	 * transaction.  (Note: if we are here during PRE_COMMIT or PRE_PREPARE,
	 * this saves a useless scan of the hashtable during COMMIT or PREPARE.)
	 */
	xact_got_connection = false;
}

/*
 * sqlitefdw_subxact_callback --- cleanup at subtransaction end.
 */
static void
sqlitefdw_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;
	int			curlevel;

#if (PG_VERSION_NUM >= 90300)
	/* Nothing to do at subxact start, nor after commit. */
	if (!(event == SUBXACT_EVENT_PRE_COMMIT_SUB ||
		  event == SUBXACT_EVENT_ABORT_SUB))
		return;
#else
	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;
#endif

	/* Quick exit if no connections were touched in this transaction. */
	if (!xact_got_connection)
		return;

	/*
	 * Scan all connection cache entries to find open remote subtransactions
	 * of the current level, and close them.
	 */
	curlevel = GetCurrentTransactionNestLevel();
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		char		sql[100];

		/*
		 * We only care about connections with open remote subtransactions of
		 * the current level.
		 */
		if (entry->conn == NULL || entry->xact_depth < curlevel)
			continue;

		if (entry->xact_depth > curlevel)
			elog(ERROR, "missed cleaning up remote subtransaction at level %d",
				 entry->xact_depth);

#if (PG_VERSION_NUM >= 90300)
		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		{
			/* Commit all remote subtransactions during pre-commit */
			snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT s%d", curlevel);
			do_sql_command(entry->conn, sql);
		}
		else
#endif
		{
			/* Assume we might have lost track of prepared statements */
			entry->have_error = true;

			/* Rollback all remote subtransactions during abort */
			snprintf(sql, sizeof(sql),
					 "ROLLBACK TO SAVEPOINT s%d; RELEASE SAVEPOINT s%d",
					 curlevel, curlevel);
			do_sql_command(entry->conn, sql);
		}

		/* OK, we're outta that level of subtransaction */
		entry->xact_depth--;
	}
}

/*
 * Start remote transaction or subtransaction, if needed.
 */
static void
begin_remote_xact(ConnCacheEntry *entry)
{
	int			curlevel = GetCurrentTransactionNestLevel();

	/* Start main transaction if we haven't yet */
	if (entry->xact_depth <= 0)
	{
		const char *sql;

		sql = "BEGIN TRANSACTION";
		do_sql_command(entry->conn, sql);
		entry->xact_depth = 1;
	}

	/*
	 * If we're in a subtransaction, stack up savepoints to match our level.
	 * This ensures we can rollback just the desired effects when a
	 * subtransaction aborts.
	 */
	while (entry->xact_depth < curlevel)
	{
		char		sql[64];

		snprintf(sql, sizeof(sql), "SAVEPOINT s%d", entry->xact_depth + 1);
		do_sql_command(entry->conn, sql);
		entry->xact_depth++;
	}
}

/*
 * Get a sqlite3 connection object which can be used to execute queries.
 * server with the user's authorization.  A new connection is established
 * if we don't already have a suitable one, and a transaction is opened at
 * the right subtransaction nesting depth if we didn't do that already.
 */
static sqlite3 *
GetConnection(Oid ftableId)
{
	bool	found;
	ForeignTable   *f_table;
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
		ConnectionHash = hash_create("sqlite_fdw connections", 4,
									 &ctl,
#if (PG_VERSION_NUM >= 90500)
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
#else
									 HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION);
#endif

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		RegisterXactCallback(sqlitefdw_xact_callback, NULL);
		RegisterSubXactCallback(sqlitefdw_subxact_callback, NULL);
	}


	/* Set flag that we did GetConnection during the current transaction */
	xact_got_connection = true;

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	f_table = GetForeignTable(ftableId);
	key = f_table->serverid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* initialize new hashtable entry (key is already filled in) */
		entry->conn = NULL;
		entry->xact_depth = 0;
		entry->have_error = false;
	}

	/*
	 * We don't check the health of cached connection here, because it would
	 * require some overhead.  Broken connection will be detected when the
	 * connection is actually used.
	 */

	/*
	 * If cache entry doesn't have a connection, we have to establish a new
	 * connection.  (If connect_pg_server throws an error, the cache entry
	 * will be left in a valid empty state.)
	 */

	/* Connect to the database */
	if (entry->conn == NULL)
	{
		sqlite3 *db;
		char   *database = NULL;
		char   *table = NULL;

		sqliteGetOptions(ftableId, &database, &table);
		if (sqlite3_open(database, &db))
			ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				errmsg("Can't open sqlite database %s: %s", database, sqlite3_errmsg(db))));

		entry->conn = db;
		entry->xact_depth = 0;	/* just to be sure */
		entry->have_error = false;
	}

	/*
	 * Start a new transaction or subtransaction if needed.
	 */
	begin_remote_xact(entry);

	return entry->conn;
}

static void
ReleaseConnection(sqlite3* db)
{
	/*
	 * Currently, we don't actually track connection references because all
	 * cleanup is managed on a transaction or subtransaction basis instead. So
	 * there's nothing to do here.
	 */
}
