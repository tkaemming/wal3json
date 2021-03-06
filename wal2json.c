/*-------------------------------------------------------------------------
 *
 * wal2json.c
 * 		JSON output plugin for changeset extraction
 *
 * Copyright (c) 2013-2018, Euler Taveira de Oliveira
 *
 * IDENTIFICATION
 *		contrib/wal2json/wal2json.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"

#include "replication/logical.h"

#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#define	WAL2JSON_FORMAT_VERSION			1
#define	WAL2JSON_FORMAT_MIN_VERSION		1

PG_MODULE_MAGIC;

extern void		_PG_init(void);
extern void	PGDLLEXPORT	_PG_output_plugin_init(OutputPluginCallbacks *cb);

typedef struct
{
	MemoryContext context;
	bool		include_types;		/* include data types */
	bool		include_type_oids;	/* include data type oids */
	bool		include_typmod;		/* include typmod in types */
	bool		include_not_null;	/* include not-null constraints */

	bool		pretty_print;		/* pretty-print JSON? */

	List		*filter_tables;		/* filter out tables */
	List		*add_tables;		/* add only these tables */

	int			format_version;		/* support different formats */
} JsonDecodingData;

typedef struct SelectTable
{
	char	*schemaname;
	char	*tablename;
	bool	allschemas;				/* true means any schema */
	bool	alltables;				/* true means any table */
} SelectTable;

/* These must be available to pg_dlsym() */
static void pg_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init);
static void pg_decode_shutdown(LogicalDecodingContext *ctx);
static void pg_decode_begin_txn(LogicalDecodingContext *ctx,
					ReorderBufferTXN *txn);
static void pg_decode_commit_txn(LogicalDecodingContext *ctx,
					 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pg_decode_change(LogicalDecodingContext *ctx,
				 ReorderBufferTXN *txn, Relation rel,
				 ReorderBufferChange *change);
#if	PG_VERSION_NUM >= 90600
static void pg_decode_message(LogicalDecodingContext *ctx,
					ReorderBufferTXN *txn, XLogRecPtr lsn,
					bool transactional, const char *prefix,
					Size content_size, const char *content);
#endif

static bool parse_table_identifier(List *qualified_tables, char separator, List **select_tables);
static bool string_to_SelectTable(char *rawstring, char separator, List **select_tables);

void
_PG_init(void)
{
}

/* Specify output plugin callbacks */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

	cb->startup_cb = pg_decode_startup;
	cb->begin_cb = pg_decode_begin_txn;
	cb->change_cb = pg_decode_change;
	cb->commit_cb = pg_decode_commit_txn;
	cb->shutdown_cb = pg_decode_shutdown;
#if	PG_VERSION_NUM >= 90600
	cb->message_cb = pg_decode_message;
#endif
}

/* Initialize this plugin */
static void
pg_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	ListCell	*option;
	JsonDecodingData *data;
	SelectTable	*t;

	data = palloc0(sizeof(JsonDecodingData));
	data->context = AllocSetContextCreate(TopMemoryContext,
										"wal2json output context",
#if PG_VERSION_NUM >= 90600
										ALLOCSET_DEFAULT_SIZES
#else
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE
#endif
                                        );
	data->include_types = true;
	data->include_type_oids = false;
	data->include_typmod = true;
	data->pretty_print = false;
	data->include_not_null = false;
	data->filter_tables = NIL;

	data->format_version = WAL2JSON_FORMAT_VERSION;

	/* add all tables in all schemas by default */
	t = palloc0(sizeof(SelectTable));
	t->allschemas = true;
	t->alltables = true;
	data->add_tables = lappend(data->add_tables, t);

	ctx->output_plugin_private = data;

	opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;

	foreach(option, ctx->output_plugin_options)
	{
		DefElem *elem = lfirst(option);

		Assert(elem->arg == NULL || IsA(elem->arg, String));

		if (strcmp(elem->defname, "include-types") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(DEBUG1, "include-types argument is null");
				data->include_types = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_types))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-type-oids") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(DEBUG1, "include-type-oids argument is null");
				data->include_type_oids = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_type_oids))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-typmod") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(DEBUG1, "include-typmod argument is null");
				data->include_typmod = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_typmod))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-not-null") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(DEBUG1, "include-not-null argument is null");
				data->include_not_null = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_not_null))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-unchanged-toast") == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("parameter \"%s\" was deprecated", elem->defname)));
		}
		else if (strcmp(elem->defname, "filter-tables") == 0)
		{
			char	*rawstr;

			if (elem->arg == NULL)
			{
				elog(DEBUG1, "filter-tables argument is null");
				data->filter_tables = NIL;
			}
			else
			{
				rawstr = pstrdup(strVal(elem->arg));
				if (!string_to_SelectTable(rawstr, ',', &data->filter_tables))
				{
					pfree(rawstr);
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_NAME),
							 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								 strVal(elem->arg), elem->defname)));
				}
				pfree(rawstr);
			}
		}
		else if (strcmp(elem->defname, "add-tables") == 0)
		{
			char	*rawstr;

			/*
			 * If this parameter is specified, remove 'all tables in all
			 * schemas' value from list.
			 */
			list_free_deep(data->add_tables);
			data->add_tables = NIL;

			if (elem->arg == NULL)
			{
				elog(DEBUG1, "add-tables argument is null");
				data->add_tables = NIL;
			}
			else
			{
				rawstr = pstrdup(strVal(elem->arg));
				if (!string_to_SelectTable(rawstr, ',', &data->add_tables))
				{
					pfree(rawstr);
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_NAME),
							 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								 strVal(elem->arg), elem->defname)));
				}
				pfree(rawstr);
			}
		}
		else if (strcmp(elem->defname, "format-version") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(DEBUG1, "format-version argument is null");
				data->format_version = WAL2JSON_FORMAT_VERSION;
			}
			else if (!parse_int(strVal(elem->arg), &data->format_version, 0, NULL))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));

			if (data->format_version > WAL2JSON_FORMAT_VERSION)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("client sent format_version=%d but we only support format %d or lower",
						 data->format_version, WAL2JSON_FORMAT_VERSION)));

			if (data->format_version < WAL2JSON_FORMAT_MIN_VERSION)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("client sent format_version=%d but we only support format %d or higher",
						 data->format_version, WAL2JSON_FORMAT_MIN_VERSION)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" = \"%s\" is unknown",
						elem->defname,
						elem->arg ? strVal(elem->arg) : "(null)")));
		}
	}

	elog(DEBUG2, "format version: %d", data->format_version);
}

/* cleanup this plugin's resources */
static void
pg_decode_shutdown(LogicalDecodingContext *ctx)
{
	JsonDecodingData *data = ctx->output_plugin_private;

	/* cleanup our own resources via memory context reset */
	MemoryContextDelete(data->context);
}

/* BEGIN callback */
static void
pg_decode_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	OutputPluginPrepareWrite(ctx, true);
	appendStringInfo(ctx->out, "{\"event\":\"begin\",\"xid\":%u,\"timestamp\":\"%s\"}", txn->xid, timestamptz_to_str(txn->commit_time));
	OutputPluginWrite(ctx, true);
}

/* COMMIT callback */
static void
pg_decode_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr commit_lsn)
{
	OutputPluginPrepareWrite(ctx, true);
	appendStringInfo(ctx->out, "{\"event\":\"commit\"}");
	OutputPluginWrite(ctx, true);
}


/*
 * Accumulate tuple information and stores it at the end
 *
 * replident: is this tuple a replica identity?
 * hasreplident: does this tuple has an associated replica identity?
 */
static void
tuple_to_stringinfo(LogicalDecodingContext *ctx, TupleDesc tupdesc, HeapTuple tuple, TupleDesc indexdesc, bool replident, bool hasreplident)
{
	JsonDecodingData	*data;
	int					natt;

	StringInfoData		colnames;
	StringInfoData		coltypes;
	StringInfoData		coltypeoids;
	StringInfoData		colnotnulls;
	StringInfoData		colvalues;
	char				comma[2] = "";

	data = ctx->output_plugin_private;

	initStringInfo(&colnames);
	initStringInfo(&coltypes);
	if (data->include_type_oids)
		initStringInfo(&coltypeoids);
	if (data->include_not_null)
		initStringInfo(&colnotnulls);
	initStringInfo(&colvalues);

	/*
	 * If replident is true, it will output info about replica identity. In this
	 * case, there are special JSON objects for it. Otherwise, it will print new
	 * tuple data.
	 */
	if (replident)
	{
		appendStringInfo(&colnames, "\"oldkeys\":{");
		appendStringInfo(&colnames, "\"keynames\":[");
		appendStringInfo(&coltypes, "\"keytypes\":[");
		if (data->include_type_oids)
			appendStringInfo(&coltypeoids, "\"keytypeoids\":[");
		appendStringInfo(&colvalues, "\"keyvalues\":[");
	}
	else
	{
		appendStringInfo(&colnames, "\"columnnames\":[");
		appendStringInfo(&coltypes, "\"columntypes\":[");
		if (data->include_type_oids)
			appendStringInfo(&coltypeoids, "\"columntypeoids\":[");
		if (data->include_not_null)
			appendStringInfo(&colnotnulls, "\"columnoptionals\":[");
		appendStringInfo(&colvalues, "\"columnvalues\":[");
	}

	/* Print column information (name, type, value) */
	for (natt = 0; natt < tupdesc->natts; natt++)
	{
		Form_pg_attribute	attr;		/* the attribute itself */
		Oid					typid;		/* type of current attribute */
		HeapTuple			type_tuple;	/* information about a type */
		Oid					typoutput;	/* output function */
		bool				typisvarlena;
		Datum				origval;	/* possibly toasted Datum */
		Datum				val;		/* definitely detoasted Datum */
		char				*outputstr = NULL;
		bool				isnull;		/* column is null? */

		/*
		 * Commit d34a74dd064af959acd9040446925d9d53dff15b introduced
		 * TupleDescAttr() in back branches. If the version supports
		 * this macro, use it. Version 10 and later already support it.
		 */
#if (PG_VERSION_NUM >= 90600 && PG_VERSION_NUM < 90605) || (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90509) || (PG_VERSION_NUM >= 90400 && PG_VERSION_NUM < 90414)
		attr = tupdesc->attrs[natt];
#else
		attr = TupleDescAttr(tupdesc, natt);
#endif

		elog(DEBUG1, "attribute \"%s\" (%d/%d)", NameStr(attr->attname), natt, tupdesc->natts);

		/* Do not print dropped or system columns */
		if (attr->attisdropped || attr->attnum < 0)
			continue;

		/* Search indexed columns in whole heap tuple */
		if (indexdesc != NULL)
		{
			int		j;
			bool	found_col = false;

			for (j = 0; j < indexdesc->natts; j++)
			{
				Form_pg_attribute	iattr;

				/* See explanation a few lines above. */
#if (PG_VERSION_NUM >= 90600 && PG_VERSION_NUM < 90605) || (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90509) || (PG_VERSION_NUM >= 90400 && PG_VERSION_NUM < 90414)
				iattr = indexdesc->attrs[j];
#else
				iattr = TupleDescAttr(indexdesc, j);
#endif

				if (strcmp(NameStr(attr->attname), NameStr(iattr->attname)) == 0)
					found_col = true;

			}

			/* Print only indexed columns */
			if (!found_col)
				continue;
		}

		/* Get Datum from tuple */
		origval = heap_getattr(tuple, natt + 1, tupdesc, &isnull);

		/* Skip nulls iif printing key/identity */
		if (isnull && replident)
			continue;

		typid = attr->atttypid;

		/* Figure out type name */
		type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(type_tuple))
			elog(ERROR, "cache lookup failed for type %u", typid);

		/* Get information needed for printing values of a type */
		getTypeOutputInfo(typid, &typoutput, &typisvarlena);

		/* XXX Unchanged TOAST Datum does not need to be output */
		if (!isnull && typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval))
		{
			elog(DEBUG1, "column \"%s\" has an unchanged TOAST", NameStr(attr->attname));
			continue;
		}

		/* Accumulate each column info */
		appendStringInfo(&colnames, "%s", comma);
		escape_json(&colnames, NameStr(attr->attname));

		if (data->include_types)
		{
			if (data->include_typmod)
			{
				char	*type_str;

				type_str = TextDatumGetCString(DirectFunctionCall2(format_type, attr->atttypid, attr->atttypmod));
				appendStringInfo(&coltypes, "%s", comma);
				escape_json(&coltypes, type_str);
				pfree(type_str);
			}
			else
			{
				Form_pg_type type_form = (Form_pg_type) GETSTRUCT(type_tuple);
				appendStringInfo(&coltypes, "%s", comma);
				escape_json(&coltypes, NameStr(type_form->typname));
			}

			/* oldkeys doesn't print not-null constraints */
			if (!replident && data->include_not_null)
			{
				if (attr->attnotnull)
					appendStringInfo(&colnotnulls, "%sfalse", comma);
				else
					appendStringInfo(&colnotnulls, "%strue", comma);
			}
		}

		if (data->include_type_oids)
			appendStringInfo(&coltypeoids, "%s%u", comma, typid);

		ReleaseSysCache(type_tuple);

		if (isnull)
		{
			appendStringInfo(&colvalues, "%snull", comma);
		}
		else
		{
			if (typisvarlena)
				val = PointerGetDatum(PG_DETOAST_DATUM(origval));
			else
				val = origval;

			/* Finally got the value */
			outputstr = OidOutputFunctionCall(typoutput, val);

			/*
			 * Data types are printed with quotes unless they are number, true,
			 * false, null, an array or an object.
			 *
			 * The NaN and Infinity are not valid JSON symbols. Hence,
			 * regardless of sign they are represented as the string null.
			 */
			switch (typid)
			{
				case INT2OID:
				case INT4OID:
				case INT8OID:
				case OIDOID:
				case FLOAT4OID:
				case FLOAT8OID:
				case NUMERICOID:
					if (pg_strncasecmp(outputstr, "NaN", 3) == 0 ||
							pg_strncasecmp(outputstr, "Infinity", 8) == 0 ||
							pg_strncasecmp(outputstr, "-Infinity", 9) == 0)
					{
						appendStringInfo(&colvalues, "%snull", comma);
						elog(DEBUG1, "attribute \"%s\" is special: %s", NameStr(attr->attname), outputstr);
					}
					else if (strspn(outputstr, "0123456789+-eE.") == strlen(outputstr))
						appendStringInfo(&colvalues, "%s%s", comma, outputstr);
					else
						elog(ERROR, "%s is not a number", outputstr);
					break;
				case BOOLOID:
					if (strcmp(outputstr, "t") == 0)
						appendStringInfo(&colvalues, "%strue", comma);
					else
						appendStringInfo(&colvalues, "%sfalse", comma);
					break;
				case BYTEAOID:
					appendStringInfo(&colvalues, "%s", comma);
					/* string is "\x54617069727573", start after "\x" */
					escape_json(&colvalues, (outputstr + 2));
					break;
				default:
					appendStringInfo(&colvalues, "%s", comma);
					escape_json(&colvalues, outputstr);
					break;
			}
		}

		/* The first column does not have comma */
		if (strcmp(comma, "") == 0)
			comma[0] = ',';
	}

	/* Column info ends */
	if (replident)
	{
		appendStringInfo(&colnames, "],");
		if (data->include_types)
			appendStringInfo(&coltypes, "],");
		if (data->include_type_oids)
			appendStringInfo(&coltypeoids, "],");
		appendStringInfo(&colvalues, "]");
		appendStringInfo(&colvalues, "}");
	}
	else
	{
		appendStringInfo(&colnames, "],");
		if (data->include_types)
			appendStringInfo(&coltypes, "],");
		if (data->include_type_oids)
			appendStringInfo(&coltypeoids, "],");
		if (data->include_not_null)
			appendStringInfo(&colnotnulls, "],");
		if (hasreplident)
			appendStringInfo(&colvalues, "],");
		else
			appendStringInfo(&colvalues, "]");
	}

	/* Print data */
	appendStringInfoString(ctx->out, colnames.data);
	if (data->include_types)
		appendStringInfoString(ctx->out, coltypes.data);
	if (data->include_type_oids)
		appendStringInfoString(ctx->out, coltypeoids.data);
	if (data->include_not_null)
		appendStringInfoString(ctx->out, colnotnulls.data);
	appendStringInfoString(ctx->out, colvalues.data);

	pfree(colnames.data);
	pfree(coltypes.data);
	if (data->include_type_oids)
		pfree(coltypeoids.data);
	if (data->include_not_null)
		pfree(colnotnulls.data);
	pfree(colvalues.data);
}

/* Print columns information */
static void
columns_to_stringinfo(LogicalDecodingContext *ctx, TupleDesc tupdesc, HeapTuple tuple, bool hasreplident)
{
	tuple_to_stringinfo(ctx, tupdesc, tuple, NULL, false, hasreplident);
}

/* Print replica identity information */
static void
identity_to_stringinfo(LogicalDecodingContext *ctx, TupleDesc tupdesc, HeapTuple tuple, TupleDesc indexdesc)
{
	/* Last parameter does not matter */
	tuple_to_stringinfo(ctx, tupdesc, tuple, indexdesc, true, false);
}

/* Callback for individual changed tuples */
static void
pg_decode_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 Relation relation, ReorderBufferChange *change)
{
	JsonDecodingData *data;
	Form_pg_class class_form;
	TupleDesc	tupdesc;
	MemoryContext old;

	Relation	indexrel;
	TupleDesc	indexdesc;

	char		*schemaname;
	char		*tablename;

	AssertVariableIsOfType(&pg_decode_change, LogicalDecodeChangeCB);

	data = ctx->output_plugin_private;
	class_form = RelationGetForm(relation);
	tupdesc = RelationGetDescr(relation);

	/* Avoid leaking memory by using and resetting our own context */
	old = MemoryContextSwitchTo(data->context);

	/* schema and table names are used for select tables */
	schemaname = get_namespace_name(class_form->relnamespace);
	tablename = NameStr(class_form->relname);

	OutputPluginPrepareWrite(ctx, true);

	/* Make sure rd_replidindex is set */
	RelationGetIndexList(relation);

	/* Filter tables, if available */
	if (list_length(data->filter_tables) > 0)
	{
		ListCell	*lc;

		foreach(lc, data->filter_tables)
		{
			SelectTable	*t = lfirst(lc);

			if (t->allschemas || strcmp(t->schemaname, schemaname) == 0)
			{
				if (t->alltables || strcmp(t->tablename, tablename) == 0)
				{
					elog(DEBUG2, "\"%s\".\"%s\" was filtered out",
								((t->allschemas) ? "*" : t->schemaname),
								((t->alltables) ? "*" : t->tablename));
					return;
				}
			}
		}
	}

	/* Add tables */
	if (list_length(data->add_tables) > 0)
	{
		ListCell	*lc;
		bool		skip = true;

		/* all tables in all schemas are added by default */
		foreach(lc, data->add_tables)
		{
			SelectTable	*t = lfirst(lc);

			if (t->allschemas || strcmp(t->schemaname, schemaname) == 0)
			{
				if (t->alltables || strcmp(t->tablename, tablename) == 0)
				{
					elog(DEBUG2, "\"%s\".\"%s\" was added",
								((t->allschemas) ? "*" : t->schemaname),
								((t->alltables) ? "*" : t->tablename));
					skip = false;
				}
			}
		}

		/* table was not found */
		if (skip)
			return;
	}

	/* Sanity checks */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			if (change->data.tp.newtuple == NULL)
			{
				elog(WARNING, "no tuple data for INSERT in table \"%s\"", NameStr(class_form->relname));
				MemoryContextSwitchTo(old);
				MemoryContextReset(data->context);
				return;
			}
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			/*
			 * Bail out iif:
			 * (i) doesn't have a pk and replica identity is not full;
			 * (ii) replica identity is nothing.
			 */
			if (!OidIsValid(relation->rd_replidindex) && relation->rd_rel->relreplident != REPLICA_IDENTITY_FULL)
			{
				/* FIXME this sentence is imprecise */
				elog(WARNING, "table \"%s\" without primary key or replica identity is nothing", NameStr(class_form->relname));
				MemoryContextSwitchTo(old);
				MemoryContextReset(data->context);
				return;
			}

			if (change->data.tp.newtuple == NULL)
			{
				elog(WARNING, "no tuple data for UPDATE in table \"%s\"", NameStr(class_form->relname));
				MemoryContextSwitchTo(old);
				MemoryContextReset(data->context);
				return;
			}
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			/*
			 * Bail out iif:
			 * (i) doesn't have a pk and replica identity is not full;
			 * (ii) replica identity is nothing.
			 */
			if (!OidIsValid(relation->rd_replidindex) && relation->rd_rel->relreplident != REPLICA_IDENTITY_FULL)
			{
				/* FIXME this sentence is imprecise */
				elog(WARNING, "table \"%s\" without primary key or replica identity is nothing", NameStr(class_form->relname));
				MemoryContextSwitchTo(old);
				MemoryContextReset(data->context);
				return;
			}

			if (change->data.tp.oldtuple == NULL)
			{
				elog(WARNING, "no tuple data for DELETE in table \"%s\"", NameStr(class_form->relname));
				MemoryContextSwitchTo(old);
				MemoryContextReset(data->context);
				return;
			}
			break;
		default:
			Assert(false);
	}

	appendStringInfo(ctx->out, "{\"event\":\"change\",");

	/* Print change kind */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			appendStringInfo(ctx->out, "\"action\":\"insert\",");
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			appendStringInfo(ctx->out, "\"action\":\"update\",");
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			appendStringInfo(ctx->out, "\"action\":\"delete\",");
			break;
		default:
			Assert(false);
	}

	/* Print qualified table name */
	appendStringInfo(ctx->out, "\"schema\":");
	escape_json(ctx->out, get_namespace_name(class_form->relnamespace));
	appendStringInfo(ctx->out, ",");

	appendStringInfo(ctx->out, "\"table\":");
	escape_json(ctx->out, NameStr(class_form->relname));
	appendStringInfo(ctx->out, ",");

	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			/* Print the new tuple */
			columns_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, false);
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			/* Print the new tuple */
			columns_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, true);

			/*
			 * The old tuple is available when:
			 * (i) pk changes;
			 * (ii) replica identity is full;
			 * (iii) replica identity is index and indexed column changes.
			 *
			 * FIXME if old tuple is not available we must get only the indexed
			 * columns (the whole tuple is printed).
			 */
			if (change->data.tp.oldtuple == NULL)
			{
				elog(DEBUG1, "old tuple is null");

				indexrel = RelationIdGetRelation(relation->rd_replidindex);
				if (indexrel != NULL)
				{
					indexdesc = RelationGetDescr(indexrel);
					identity_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, indexdesc);
					RelationClose(indexrel);
				}
				else
				{
					identity_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, NULL);
				}
			}
			else
			{
				elog(DEBUG1, "old tuple is not null");
				identity_to_stringinfo(ctx, tupdesc, &change->data.tp.oldtuple->tuple, NULL);
			}
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			/* Print the replica identity */
			indexrel = RelationIdGetRelation(relation->rd_replidindex);
			if (indexrel != NULL)
			{
				indexdesc = RelationGetDescr(indexrel);
				identity_to_stringinfo(ctx, tupdesc, &change->data.tp.oldtuple->tuple, indexdesc);
				RelationClose(indexrel);
			}
			else
			{
				identity_to_stringinfo(ctx, tupdesc, &change->data.tp.oldtuple->tuple, NULL);
			}

			if (change->data.tp.oldtuple == NULL)
				elog(DEBUG1, "old tuple is null");
			else
				elog(DEBUG1, "old tuple is not null");
			break;
		default:
			Assert(false);
	}

	appendStringInfo(ctx->out, "}");

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);

	OutputPluginWrite(ctx, true);
}

#if	PG_VERSION_NUM >= 90600
/* Callback for generic logical decoding messages */
static void
pg_decode_message(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
		XLogRecPtr lsn, bool transactional, const char *prefix, Size
		content_size, const char *content)
{
	JsonDecodingData *data;
	MemoryContext old;
	char *content_str;

	data = ctx->output_plugin_private;

	/* Avoid leaking memory by using and resetting our own context */
	old = MemoryContextSwitchTo(data->context);

	OutputPluginPrepareWrite(ctx, true);

	/* build a complete JSON object for non-transactional message */
	if (!transactional)
		appendStringInfo(ctx->out, "{\"change\":[");

	appendStringInfo(ctx->out, "{\"kind\":\"message\",");

	if (transactional)
		appendStringInfo(ctx->out, "\"transactional\":true,");
	else
		appendStringInfo(ctx->out, "\"transactional\":false,");

	appendStringInfo(ctx->out, "\"prefix\":");
	escape_json(ctx->out, prefix);
	appendStringInfo(ctx->out, ",\"content\":");

	content_str = (char *) palloc0((content_size + 1) * sizeof(char));
	strncpy(content_str, content, content_size);
	escape_json(ctx->out, content_str);
	pfree(content_str);

	appendStringInfo(ctx->out, "}");

	/* build a complete JSON object for non-transactional message */
	if (!transactional)
		appendStringInfo(ctx->out, "]}");

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);

	OutputPluginWrite(ctx, true);
}
#endif

static bool
parse_table_identifier(List *qualified_tables, char separator, List **select_tables)
{
	ListCell	*lc;

	foreach(lc, qualified_tables)
	{
		char		*str = lfirst(lc);
		char		*startp;
		char		*nextp;
		int			len;
		SelectTable	*t = palloc0(sizeof(SelectTable));

		/*
		 * Detect a special character that means all schemas. There could be a
		 * schema named "*" thus this test should be before we remove the
		 * escape character.
		 */
		if (str[0] == '*' || str[1] == '.')
			t->allschemas = true;
		else
			t->allschemas = false;

		startp = nextp = str;
		while (*nextp && *nextp != separator)
		{
			/* remove escape character */
			if (*nextp == '\\')
				memmove(nextp, nextp + 1, strlen(nextp));
			nextp++;
		}
		len = nextp - startp;

		/* if separator was not found, schema was not informed */
		if (*nextp == '\0')
		{
			pfree(t);
			return false;
		}
		else
		{
			/* schema name */
			t->schemaname = (char *) palloc0((len + 1) * sizeof(char));
			strncpy(t->schemaname, startp, len);

			nextp++;			/* jump separator */
			startp = nextp;		/* start new identifier (table name) */

			/*
			 * Detect a special character that means all tables. There could be
			 * a table named "*" thus this test should be before that we remove
			 * the escape character.
			 */
			if (startp[0] == '*' && startp[1] == '\0')
				t->alltables = true;
			else
				t->alltables = false;

			while (*nextp)
			{
				/* remove escape character */
				if (*nextp == '\\')
					memmove(nextp, nextp + 1, strlen(nextp));
				nextp++;
			}
			len = nextp - startp;

			/* table name */
			t->tablename = (char *) palloc0((len + 1) * sizeof(char));
			strncpy(t->tablename, startp, len);
		}

		*select_tables = lappend(*select_tables, t);
	}

	return true;
}

static bool
string_to_SelectTable(char *rawstring, char separator, List **select_tables)
{
	char	   *nextp;
	bool		done = false;
	List	   *qualified_tables = NIL;

	nextp = rawstring;

	while (isspace(*nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new identifier. */
	do
	{
		char	   *curname;
		char	   *endp;
		char	   *qname;

		curname = nextp;
		while (*nextp && *nextp != separator && !isspace(*nextp))
		{
			if (*nextp == '\\')
				nextp++;	/* ignore next character because of escape */
			nextp++;
		}
		endp = nextp;
		if (curname == nextp)
			return false;	/* empty unquoted name not allowed */

		while (isspace(*nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == separator)
		{
			nextp++;
			while (isspace(*nextp))
				nextp++;		/* skip leading whitespace for next */
			/* we expect another name, so done remains false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* invalid syntax */

		/* Now safe to overwrite separator with a null */
		*endp = '\0';

		/*
		 * Finished isolating current name --- add it to list
		 */
		qname = pstrdup(curname);
		qualified_tables = lappend(qualified_tables, qname);

		/* Loop back if we didn't reach end of string */
	} while (!done);

	if (!parse_table_identifier(qualified_tables, '.', select_tables))
		return false;

	list_free_deep(qualified_tables);

	return true;
}
