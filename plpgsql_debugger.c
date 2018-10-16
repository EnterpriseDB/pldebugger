/**********************************************************************
 * plpgsql_debugger.c	- Debugger for the PL/pgSQL procedural language
 *
 * Copyright (c) 2004-2018 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License v2.0, see
 *		https://opensource.org/licenses/artistic-license-2.0
 * for full details
 *
 **********************************************************************/

#include "postgres.h"

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>

#include "lib/stringinfo.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "globalbp.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "miscadmin.h"

#if INCLUDE_PACKAGE_SUPPORT
#include "spl.h"
#include "catalog/edb_variable.h"
#else
#include "plpgsql.h"
#endif

#include "pldebugger.h"

/* Include header for GETSTRUCT */
#if (PG_VERSION_NUM >= 90300)
#include "access/htup_details.h"
#endif

/*
 * We use a var_value structure to record  a little extra information about
 * each variable.
 */

typedef struct
{
	bool	    isnull;			/* TRUE -> this variable IS NULL */
	bool		visible;		/* hidden or visible? see is_visible_datum() */
	bool		duplicate_name;	/* Is this one of many vars with same name? */
} var_value;

/*
 * When the debugger decides that it needs to step through (or into) a
 * particular function invocation, it allocates a dbg_ctx and records the
 * address of that structure in the executor's context structure
 * (estate->plugin_info).
 *
 * The dbg_ctx keeps track of all of the information we need to step through
 * code and display variable values
 */

typedef struct
{
	PLpgSQL_function *	func;		/* Function definition */
	bool				stepping;	/* If TRUE, stop at next statement */
	var_value	     *  symbols;	/* Extra debugger-private info about variables */
	char			 ** argNames;	/* Argument names */
	int					argNameCount; /* Number of names pointed to by argNames */
	void 			 (* error_callback)(void *arg);
	void 			 (* assign_expr)( PLpgSQL_execstate *estate, PLpgSQL_datum *target, PLpgSQL_expr *expr );
#if INCLUDE_PACKAGE_SUPPORT
	PLpgSQL_package   * package;
#endif
} dbg_ctx;

static void 		 dbg_startup( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		 dbg_newstmt( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );
static void 		 initialize_plugin_info( PLpgSQL_execstate * estate, PLpgSQL_function * func );

static char       ** fetchArgNames( PLpgSQL_function * func, int * nameCount );
static PLpgSQL_var * find_var_by_name( const PLpgSQL_execstate * estate, const char * var_name, int lineno, int * index );

static bool 		 is_datum_visible( PLpgSQL_datum * datum );
static bool			 is_var_visible( PLpgSQL_execstate * frame, int var_no );
static bool			 datumIsNull(PLpgSQL_datum *datum);
static bool          varIsArgument(const PLpgSQL_execstate *estate, PLpgSQL_function *func, int varNo, char **p_argname);
static char		   * get_text_val( PLpgSQL_var * var, char ** name, char ** type );

#if INCLUDE_PACKAGE_SUPPORT
static const char * plugin_name  = "spl_plugin";
#else
static const char * plugin_name  = "PLpgSQL_plugin";
#endif

static PLpgSQL_plugin plugin_funcs = { dbg_startup, NULL, NULL, dbg_newstmt, NULL };

/*
 * pldebugger_language_t interface.
 */
static void plpgsql_debugger_init(void);
static bool plpgsql_frame_belongs_to_me(ErrorContextCallback *frame);
static void plpgsql_send_stack_frame(ErrorContextCallback *frame);
static void plpgsql_send_vars(ErrorContextCallback *frame);
static void plpgsql_select_frame(ErrorContextCallback *frame);
static void plpgsql_print_var(ErrorContextCallback *frame, const char *var_name, int lineno);
static bool plpgsql_do_deposit(ErrorContextCallback *frame, const char *var_name, int line_number, const char *value);
static Oid plpgsql_get_func_oid(ErrorContextCallback *frame);
static void plpgsql_send_cur_line(ErrorContextCallback *frame);

#if INCLUDE_PACKAGE_SUPPORT
debugger_language_t spl_debugger_lang =
#else
debugger_language_t plpgsql_debugger_lang =
#endif
{
	plpgsql_debugger_init,
	plpgsql_frame_belongs_to_me,
	plpgsql_send_stack_frame,
	plpgsql_send_vars,
	plpgsql_select_frame,
	plpgsql_print_var,
	plpgsql_do_deposit,
	plpgsql_get_func_oid,
	plpgsql_send_cur_line
};

/* Install this module as an PL/pgSQL instrumentation plugin */
static void
plpgsql_debugger_init(void)
{
	PLpgSQL_plugin ** var_ptr = (PLpgSQL_plugin **) find_rendezvous_variable( plugin_name );

	*var_ptr = &plugin_funcs;
}


/**********************************************************************
 * Functions implemeting the pldebugger_language_t interface
 **********************************************************************/

static bool
plpgsql_frame_belongs_to_me(ErrorContextCallback *frame)
{
	return (frame->callback == plugin_funcs.error_callback);
}

/*
 * plpgsql_send_stack_frame()
 *
 * This function sends information about a single stack frame to the debugger
 * client.  This function is called by send_stack() whenever send_stack()
 * finds a PL/pgSQL call in the stack (remember, the call stack may contain
 * stack frames for functions written in other languages like PL/Tcl).
 */
static void
plpgsql_send_stack_frame(ErrorContextCallback *frame)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) frame->arg;
#if (PG_VERSION_NUM >= 80500)
	PLpgSQL_function  * func     = estate->func;
#else
	PLpgSQL_function  * func     = estate->err_func;
#endif
	PLpgSQL_stmt	  * stmt 	 = estate->err_stmt;
	int					argNameCount;
	char             ** argNames = fetchArgNames( func, &argNameCount );
	StringInfo		    result   = makeStringInfo();
	char              * delimiter = "";
	int				    arg;

	/*
	 * Send the name, function OID, and line number for this frame
	 */

	appendStringInfo( result, "%s:%d:%d:",
#if (PG_VERSION_NUM >= 90200)
					  func->fn_signature,
#else
					  func->fn_name,
#endif
					  func->fn_oid,
					  stmt->lineno );

	/*
	 * Now assemble a string that shows the argument names and value for this frame
	 */

	for( arg = 0; arg < func->fn_nargs; ++arg )
	{
		int					index   = func->fn_argvarnos[arg];
		PLpgSQL_datum		*argDatum = (PLpgSQL_datum *)estate->datums[index];
		char 				*value;

		/* value should be an empty string if argDatum is null*/
		if( datumIsNull( argDatum ))
			value = pstrdup( "" );
		else
			value = get_text_val((PLpgSQL_var*)argDatum, NULL, NULL );

		if( argNames && argNames[arg] && argNames[arg][0] )
			appendStringInfo( result,  "%s%s=%s", delimiter, argNames[arg], value );
		else
			appendStringInfo( result,  "%s$%d=%s", delimiter, arg+1, value );

		pfree( value );

		delimiter = ", ";
	}

	dbg_send( "%s", result->data );
}

/*
 * varIsArgument() :
 *
 * Returns true if it's an argument of the function. In case the function is an
 * EDB-SPL nested function, it returns true only if it is an argument of the
 * current nested function; in all other cases it returns false, even if it's
 * an argument of any of the outer nested functions in the current nested
 * function call stack.
 *
 * If the variable is a *named* argument, the argument name is passed
 * through 'p_argname'. In case of nested functions, p_argname is set if the
 * variable name is a named argument of any of the nested functions in the
 * current nested function call stack.
 *
 * varNo is the estate->datums index.
 */
static bool
varIsArgument(const PLpgSQL_execstate *estate, PLpgSQL_function *func,
		   int varNo, char **p_argname)
{
#if INCLUDE_PACKAGE_SUPPORT && (PG_VERSION_NUM >= 90600)

	/*
	 * If this variable is actually an argument of this function, return the
	 * argument name. Such argument variables have '$n' refname, so we need to
	 * use the function argument name if it's a named argument.
	 */
	if (func->fn_nallargs > 0 &&
		varNo >= func->fn_first_argno &&
		varNo < func->fn_first_argno + func->fn_nallargs)
	{
		char *argname = func->fn_argnames[varNo - func->fn_first_argno];

		if (argname && argname[0])
			*p_argname = argname;

		return true;
	}

	/*
	 * Now check if it is an argument of any of the outer functions. If yes,
	 * we need to get the argument name in case it is a named argument.
	 */
	if (func->parent)
		(void) varIsArgument(estate, func->parent, varNo, p_argname);

	/* It is not an argument of the current function. */
	return false;

#else

	dbg_ctx	   *dbg_info = (dbg_ctx *) estate->plugin_info;
	bool		isArg = false;

	if (varNo < dbg_info->func->fn_nargs)
		isArg = true;

	/* Get it's name if it's a named argument. */
	if (varNo < dbg_info->argNameCount)
	{
		isArg = true;

		if (dbg_info->argNames && dbg_info->argNames[varNo] &&
			dbg_info->argNames[varNo][0])
		{
			*p_argname  = dbg_info->argNames[varNo];
		}
	}

	return isArg;

#endif
}

/*
 * plpgsql_send_vars()
 *
 * This function sends a list of variables (names, types, values...) to
 * the proxy process.  We send information about the variables defined in
 * the given frame (local variables) and parameter values.
 */
static void
plpgsql_send_vars(ErrorContextCallback *frame)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) frame->arg;
	dbg_ctx * dbg_info = (dbg_ctx *) estate->plugin_info;
	int       i;

	for( i = 0; i < estate->ndatums; i++ )
	{
		if( is_var_visible( estate, i ))
		{
			switch( estate->datums[i]->dtype )
			{
#if (PG_VERSION_NUM >= 110000)
				case PLPGSQL_DTYPE_PROMISE:
#endif
				case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var * var = (PLpgSQL_var *) estate->datums[i];
					char        * val;
					char		* name = var->refname;
					bool		  isArg;

					isArg = varIsArgument(estate, dbg_info->func, i, &name);

					if( datumIsNull((PLpgSQL_datum *)var ))
						val = "NULL";
					else
						val = get_text_val( var, NULL, NULL );

					dbg_send( "%s:%c:%d:%c:%c:%c:%d:%s",
							  name,
							  isArg ? 'A' : 'L',
							  var->lineno,
							  dbg_info->symbols[i].duplicate_name ? 'f' : 't',
							  var->isconst ? 't':'f',
							  var->notnull ? 't':'f',
							  var->datatype ? var->datatype->typoid : InvalidOid,
							  val );

					break;
				}
#if 0
			FIXME: implement other types

				case PLPGSQL_DTYPE_REC:
				{
					PLpgSQL_rec * rec = (PLpgSQL_rec *) estate->datums[i];
					int		      att;
					char        * typeName;

					if (rec->tupdesc != NULL)
					{
						for( att = 0; att < rec->tupdesc->natts; ++att )
						{
							typeName = SPI_gettype( rec->tupdesc, att + 1 );

							dbg_send( "o:%s.%s:%d:%d:%d:%d:%s\n",
									  rec->refname, NameStr( rec->tupdesc->attrs[att]->attname ),
									  0, rec->lineno, 0, rec->tupdesc->attrs[att]->attnotnull, typeName ? typeName : "" );

							if( typeName )
								pfree( typeName );
						}
					}
					break;
				}
#endif
				case PLPGSQL_DTYPE_ROW:
				case PLPGSQL_DTYPE_REC:
				case PLPGSQL_DTYPE_RECFIELD:
				case PLPGSQL_DTYPE_ARRAYELEM:
#if (PG_VERSION_NUM < 110000)
				case PLPGSQL_DTYPE_EXPR:
#endif
				{
					/* FIXME: implement other types */
					break;
				}
			}
		}
	}

#if INCLUDE_PACKAGE_SUPPORT
	/* If this frame represents a package function/procedure, send the package variables too */
	if( dbg_info->package != NULL )
	{
		PLpgSQL_package * package = dbg_info->package;
		int				  varIndex;

		for( varIndex = 0; varIndex < package->ndatums; ++varIndex )
		{
			PLpgSQL_datum * datum = package->datums[varIndex];

			switch( datum->dtype )
			{
				case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var * var = (PLpgSQL_var *) datum;
					char        * val;
					char		* name = var->refname;

					if( datumIsNull((PLpgSQL_datum *)var ))
						val = "NULL";
					else
						val = get_text_val( var, NULL, NULL );

					dbg_send( "%s:%c:%d:%c:%c:%c:%d:%s",
							  name,
							  'P',				/* variable class - P means package var */
							  var->lineno,
							  'f',				/* duplicate name?						*/
							  var->isconst ? 't':'f',
							  var->notnull ? 't':'f',
							  var->datatype ? var->datatype->typoid : InvalidOid,
							  val );

					break;
				}
			}
		}
	}
#endif

	dbg_send( "%s", "" );	/* empty string indicates end of list */
}

static void
plpgsql_select_frame(ErrorContextCallback *frame)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) frame->arg;

	/*
	 * When a frame is selected, ensure that we've initialized its
	 * plugin_info.
	 */
	if (estate->plugin_info == NULL)
	{
#if (PG_VERSION_NUM >= 80500)
		initialize_plugin_info(estate, estate->func);
#else
		initialize_plugin_info(estate, estate->err_func);
#endif
	}
}

/*
 * ---------------------------------------------------------------------
 * find_var_by_name()
 *
 *	This function returns the PLpgSQL_var pointer that corresponds to
 *	named variable (var_name).  If the named variable can't be found,
 *  find_var_by_name() returns NULL.
 *
 *  If the index is non-NULL, this function will set *index to the
 *  named variables index withing estate->datums[]
 */

static PLpgSQL_var *
find_var_by_name(const PLpgSQL_execstate * estate, const char * var_name, int lineno, int * index)
{
	dbg_ctx          * dbg_info = (dbg_ctx *)estate->plugin_info;
	PLpgSQL_function * func     = dbg_info->func;
	int                i;

	for( i = 0; i < func->ndatums; i++ )
	{
		PLpgSQL_var * var = (PLpgSQL_var *) estate->datums[i];
		size_t 		  len = strlen(var->refname);

		if(len != strlen(var_name))
			continue;

		if( strncmp( var->refname, var_name, len) == 0 )
		{
		 	if(( lineno == -1 ) || ( var->lineno == lineno ))
			{
				/* Found the named variable - return the index if the caller wants it */

				if( index )
					*index = i;
			}

			return( var );
		}
	}

	/* We can't find the variable named by the caller - return NULL */

	return( NULL );

}

static PLpgSQL_datum *
find_datum_by_name(const PLpgSQL_execstate *frame, const char *var_name,
				   int lineNo, int *index)
{
	dbg_ctx * dbg_info = (dbg_ctx *)frame->plugin_info;
	int		  i;

#if INCLUDE_PACKAGE_SUPPORT

	if( var_name[0] == '@' )
	{
		/* This is a package variable (it's name starts with a '@') */
		int		  varIndex;

		if( dbg_info == NULL )
			return( NULL );

		if( dbg_info->package == NULL )
			return( NULL );

		for( varIndex = 0; varIndex < dbg_info->package->ndatums; ++varIndex )
		{
			PLpgSQL_datum * datum = dbg_info->package->datums[varIndex];

			switch( datum->dtype )
			{
#if (PG_VERSION_NUM >= 110000)
				case PLPGSQL_DTYPE_PROMISE:
#endif
				case PLPGSQL_DTYPE_VAR:
				{
					PLpgSQL_var * var = (PLpgSQL_var *) datum;

					if( strcmp( var->refname, var_name+1 ) == 0 )
						return( datum );
					break;
				}
			}
		}

		return( NULL );
	}
#endif

	for( i = 0; i < frame->ndatums; ++i )
	{
		char	*	datumName = NULL;
		int			datumLineno = -1;

		switch( frame->datums[i]->dtype )
		{
#if (PG_VERSION_NUM >= 110000)
			case PLPGSQL_DTYPE_PROMISE:
#endif
			case PLPGSQL_DTYPE_VAR:
			case PLPGSQL_DTYPE_ROW:
			case PLPGSQL_DTYPE_REC:
			{
				PLpgSQL_variable * var = (PLpgSQL_variable *)frame->datums[i];

				datumName   = var->refname;
				datumLineno = var->lineno;

				(void) varIsArgument(frame, dbg_info->func, i, &datumName);

				break;
			}

			case PLPGSQL_DTYPE_RECFIELD:
			case PLPGSQL_DTYPE_ARRAYELEM:
#if (PG_VERSION_NUM < 110000)
			case PLPGSQL_DTYPE_EXPR:
#endif
#if (PG_VERSION_NUM <= 80400)
			case PLPGSQL_DTYPE_TRIGARG:
#endif
			{
				break;
			}
		}

		if( datumName == NULL )
			continue;

		if( strcmp( var_name, datumName ) == 0 )
		{
			if( lineNo == -1 || lineNo == datumLineno )
			{
				if( index )
					*index = i;

				return( frame->datums[i] );
			}
		}
	}

	return( NULL );
}

/*
 * ---------------------------------------------------------------------
 * print_var()
 *
 *	This function will print (that is, send to the debugger client) the
 *  type and value of the given variable.
 */
static void
print_var(const PLpgSQL_execstate *frame, const char *var_name, int lineno,
		  const PLpgSQL_var *tgt)
{
	char	     	 * extval;
	HeapTuple	       typeTup;
	Form_pg_type       typeStruct;
	FmgrInfo	       finfo_output;
	dbg_ctx 		 * dbg_info = (dbg_ctx *)frame->plugin_info;

	if( tgt->isnull )
	{
		if( dbg_info->symbols[tgt->dno].duplicate_name )
			dbg_send( "v:%s(%d):NULL\n", var_name, lineno );
		else
			dbg_send( "v:%s:NULL\n", var_name );
		return;
	}

	/* Find the output function for this data type */

	typeTup = SearchSysCache( TYPEOID, ObjectIdGetDatum( tgt->datatype->typoid ), 0, 0, 0 );

	if( !HeapTupleIsValid( typeTup ))
	{
		dbg_send( "v:%s(%d):***can't find type\n", var_name, lineno );
		return;
	}

	typeStruct = (Form_pg_type)GETSTRUCT( typeTup );

	/* Now invoke the output function to convert the variable into a null-terminated string */

	fmgr_info( typeStruct->typoutput, &finfo_output );

	extval = DatumGetCString( FunctionCall3( &finfo_output, tgt->value, ObjectIdGetDatum(typeStruct->typelem), Int32GetDatum(-1)));

	/* Send the name:value to the debugger client */

	if( dbg_info->symbols[tgt->dno].duplicate_name )
		dbg_send( "v:%s(%d):%s\n", var_name, lineno, extval );
	else
		dbg_send( "v:%s:%s\n", var_name, extval );

	pfree( extval );
	ReleaseSysCache( typeTup );
}

static void
print_row(const PLpgSQL_execstate *frame, const char *var_name, int lineno,
		  const PLpgSQL_row * tgt)
{
	/* XXX.  Shouldn't there be some code here? */
}

static void
print_rec(const PLpgSQL_execstate *frame, const char *var_name, int lineno,
		  const PLpgSQL_rec *tgt)
{
	int		attNo;

	TupleDesc	rec_tupdesc;
	HeapTuple	tuple;

#if (PG_VERSION_NUM >= 110000) && !defined(INCLUDE_PACKAGE_SUPPORT)
	if (tgt->erh == NULL ||
		ExpandedRecordIsEmpty(tgt->erh))
		return;

	rec_tupdesc = expanded_record_get_tupdesc(tgt->erh);
	tuple = expanded_record_get_tuple(tgt->erh);
#else
	if (tgt->tupdesc == NULL)
		return;

	rec_tupdesc = tgt->tupdesc;
	tuple = tgt->tup;
#endif

	for( attNo = 0; attNo < rec_tupdesc->natts; ++attNo )
	{
		char * extval = SPI_getvalue( tuple, rec_tupdesc, attNo + 1 );

#if (PG_VERSION_NUM >= 110000)
		dbg_send( "v:%s.%s:%s\n", var_name, NameStr( rec_tupdesc->attrs[attNo].attname ), extval ? extval : "NULL" );
#else
		dbg_send( "v:%s.%s:%s\n", var_name, NameStr( rec_tupdesc->attrs[attNo]->attname ), extval ? extval : "NULL" );
#endif

		if( extval )
			pfree( extval );
	}
}

static void
print_recfield(const PLpgSQL_execstate *frame, const char *var_name,
			   int lineno, const PLpgSQL_recfield *tgt)
{
	/* XXX.  Shouldn't there be some code here? */
}

static void
plpgsql_print_var(ErrorContextCallback *frame, const char *var_name,
				  int lineno)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) frame->arg;
	PLpgSQL_variable * generic = NULL;

	/* Try to find the given variable */

	if(( generic = (PLpgSQL_variable*) find_var_by_name( estate, var_name, lineno, NULL )) == NULL )
	{
		dbg_send( "v:%s(%d):Unknown variable (or not in scope)\n", var_name, lineno );
		return;
	}

	switch( generic->dtype )
	{
#if (PG_VERSION_NUM >= 110000)
		case PLPGSQL_DTYPE_PROMISE:
#endif
		case PLPGSQL_DTYPE_VAR:
			print_var( estate, var_name, lineno, (PLpgSQL_var *) generic );
			break;

		case PLPGSQL_DTYPE_ROW:
			print_row( estate, var_name, lineno, (PLpgSQL_row *) generic );
			break;

		case PLPGSQL_DTYPE_REC:
			print_rec( estate, var_name, lineno, (PLpgSQL_rec *) generic );
			break;

		case PLPGSQL_DTYPE_RECFIELD:
			print_recfield( estate, var_name, lineno, (PLpgSQL_recfield *) generic );
			break;

		case PLPGSQL_DTYPE_ARRAYELEM:
#if (PG_VERSION_NUM < 110000)
		case PLPGSQL_DTYPE_EXPR:
#endif
			/**
			 * FIXME::
			 * Hmm..  Shall we print the values for expression/array element?
			 **/
			break;
	}
}

/*
 * ---------------------------------------------------------------------
 * mark_duplicate_names()
 *
 *	In a PL/pgSQL function/procedure you can declare many variables with
 *  the same name as long as the name is unique within a scope.  The PL
 *	compiler co-mingles all variables into a single symbol table without
 *  indicating (at run-time) when a variable comes into scope.
 *
 *  When we display a variable to the user, we want to show an undecorated
 *  name unless the given variable has duplicate declarations (in nested
 *  scopes).  If we detect that a variable has duplicate declarations, we
 *	decorate the name with the line number at which each instance is
 *  declared.  This function detects duplicate names and marks duplicates
 *  in our private symbol table.
 */
static void
mark_duplicate_names(const PLpgSQL_execstate *estate, int var_no)
{
	dbg_ctx * dbg_info = (dbg_ctx *)estate->plugin_info;

	if( dbg_info->symbols[var_no].duplicate_name )
	{
		/* already detected as a duplicate name - just go home */
		return;
	}

	/*
	 * FIXME: Handle other dtypes here too - for now, we just assume
	 *		  that all other types have duplicate names
	 */

	if( estate->datums[var_no]->dtype != PLPGSQL_DTYPE_VAR )
	{
		dbg_info->symbols[var_no].duplicate_name = TRUE;
		return;
	}
	else
	{
		PLpgSQL_var * var	   = (PLpgSQL_var *)estate->datums[var_no];
		char        * var_name = var->refname;
		int			  i;

		for( i = 0; i < estate->ndatums; ++i )
		{
			if( i != var_no )
			{
				if( estate->datums[i]->dtype != PLPGSQL_DTYPE_VAR )
					continue;

				var = (PLpgSQL_var *)estate->datums[i];

				if( strcmp( var_name, var->refname ) == 0 )
				{
					dbg_info->symbols[var_no].duplicate_name  = TRUE;
					dbg_info->symbols[i].duplicate_name  = TRUE;
				}
			}
		}
	}
}

/*
 * ---------------------------------------------------------------------
 * completeFrame()
 *
 *	This function ensures that the given execution frame contains
 *	all of the information we need in order to debug it.  In particular,
 *	we create an array that extends the frame->datums[] array.
 *	We need to know which variables should be visible to the
 *	debugger client (we hide some of them by convention) and
 *	we need to figure out which names are unique and which
 *	are duplicates.
 */
static void
completeFrame(PLpgSQL_execstate *frame)
{
	dbg_ctx 		 * dbg_info = (dbg_ctx *)frame->plugin_info;
	PLpgSQL_function * func     = dbg_info->func;
	int		           i;

	if( dbg_info->symbols == NULL )
	{
		dbg_info->symbols = (var_value *) palloc( sizeof( var_value ) * func->ndatums );

		for( i = 0; i < func->ndatums; ++i )
		{
			dbg_info->symbols[i].isnull = TRUE;

			/*
			 * Note: in SPL, we hide a few variables from the debugger since
			 *       they are internally generated (that is, not declared by
			 *		 the user).  Decide whether this particular variable should
			 *       be visible to the debugger client.
			 */

			dbg_info->symbols[i].visible 		= is_datum_visible( frame->datums[i] );
			dbg_info->symbols[i].duplicate_name = FALSE;
		}

		for( i = 0; i < func->ndatums; ++i )
			mark_duplicate_names( frame, i );

		dbg_info->argNames = fetchArgNames( func, &dbg_info->argNameCount );
	}
}


/* ------------------------------------------------------------------
 * fetchArgNames()
 *
 *   This function returns the name of each argument for the given
 *   function or procedure. If the function/procedure does not have
 *	 named arguments, this function returns NULL
 *
 *	 The argument names are returned as an array of string pointers
 */
static char **
fetchArgNames(PLpgSQL_function *func, int *nameCount)
{
#if INCLUDE_PACKAGE_SUPPORT && (PG_VERSION_NUM >= 90600)
	/*
	 * Argument names are now available in func, and we anyway can't fetch from
	 * pg_proc in case of a nested function.
	 */
	*nameCount = func->fn_nallargs;
	return func->fn_argnames;
#else
	HeapTuple	tup;
	Datum		argnamesDatum;
	bool		isNull;
	Datum	   *elems;
	bool	   *nulls;
	char	  **result;
	int			i;

	if( func->fn_nargs == 0 )
		return( NULL );

	tup = SearchSysCache( PROCOID, ObjectIdGetDatum( func->fn_oid ), 0, 0, 0 );

	if( !HeapTupleIsValid( tup ))
		elog( ERROR, "cache lookup for function %u failed", func->fn_oid );

	argnamesDatum = SysCacheGetAttr( PROCOID, tup, Anum_pg_proc_proargnames, &isNull );

	if( isNull )
	{
		ReleaseSysCache( tup );
		return( NULL );
	}

	deconstruct_array( DatumGetArrayTypeP( argnamesDatum ), TEXTOID, -1, false, 'i', &elems, &nulls, nameCount );

	result = (char **) palloc( sizeof(char *) * (*nameCount));

	for( i = 0; i < (*nameCount); i++ )
		result[i] = DatumGetCString( DirectFunctionCall1( textout, elems[i] ));

	ReleaseSysCache( tup );

	return( result );
#endif
}

static char *
get_text_val(PLpgSQL_var *var, char **name, char **type)
{
	HeapTuple	       typeTup;
	Form_pg_type       typeStruct;
	FmgrInfo	       finfo_output;
	char            *  text_value = NULL;

	/* Find the output function for this data type */
	typeTup = SearchSysCache( TYPEOID, ObjectIdGetDatum( var->datatype->typoid ), 0, 0, 0 );

	if( !HeapTupleIsValid( typeTup ))
		return( NULL );

	typeStruct = (Form_pg_type)GETSTRUCT( typeTup );

	/* Now invoke the output function to convert the variable into a null-terminated string */
	fmgr_info( typeStruct->typoutput, &finfo_output );

	text_value = DatumGetCString( FunctionCall3( &finfo_output, var->value, ObjectIdGetDatum(typeStruct->typelem), Int32GetDatum(-1)));

	ReleaseSysCache( typeTup );

	if( name )
		*name = var->refname;

	if( type )
		*type = var->datatype->typname;

	return( text_value );
}

static Oid
plpgsql_get_func_oid(ErrorContextCallback *frame)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) frame->arg;
	dbg_ctx 		* dbg_info = (dbg_ctx *) estate->plugin_info;

	return dbg_info->func->fn_oid;
}

static void
dbg_startup(PLpgSQL_execstate *estate, PLpgSQL_function *func)
{
	if( func == NULL )
	{
		/*
		 * In general, this should never happen, but it seems to in the
		 * case of package constructors
		 */
		estate->plugin_info = NULL;
		return;
	}

	if( !breakpointsForFunction( func->fn_oid ) && !per_session_ctx.step_into_next_func)
	{
		estate->plugin_info = NULL;
		return;
	}
	initialize_plugin_info(estate, func);
}

static void
initialize_plugin_info(PLpgSQL_execstate *estate, PLpgSQL_function *func)
{
	dbg_ctx * dbg_info;

	/* Allocate a context structure and record the address in the estate */
	estate->plugin_info = dbg_info = (dbg_ctx *) palloc( sizeof( dbg_ctx ));

	/*
	 * As soon as we hit the first statement, we'll allocate space for each
	 * local variable. For now, we set symbols to NULL so we know to report all
	 * variables the next time we stop...
	 */
	dbg_info->symbols  		 = NULL;
	dbg_info->stepping 		 = FALSE;
	dbg_info->func     		 = func;

	/*
	 * The PL interpreter filled in two member of our plugin_funcs
	 * structure for us - we compare error_callback to the callback
	 * in the error_context_stack to make sure that we only deal with
	 * PL/pgSQL (or SPL) stack frames (hokey, but it works).  We use
	 * assign_expr when we need to deposit a value in variable.
	 */
	dbg_info->error_callback = plugin_funcs.error_callback;
	dbg_info->assign_expr    = plugin_funcs.assign_expr;

#if INCLUDE_PACKAGE_SUPPORT
	/*
	 * Look up the package this function belongs to.
	 *
	 * Inline code blocks have invalid fn_oid. They never belong to packages.
	 */
	if (OidIsValid(dbg_info->func->fn_oid))
	{
		/*
		 * Find the namespace in which this function/procedure is defined
		 */
		HeapTuple	htup;
		Oid			namespaceOid;

		htup = SearchSysCache(PROCOID, ObjectIdGetDatum(dbg_info->func->fn_oid), 0, 0, 0);

		if (!HeapTupleIsValid(htup))
			elog(ERROR, "cache lookup failed for procedure %d", dbg_info->func->fn_oid);

		namespaceOid = ((Form_pg_proc)GETSTRUCT(htup))->pronamespace;

		ReleaseSysCache(htup);

		/*
		 * Now figure out if this namespace is a package or a schema
		 *
		 * NOTE: we could read the pg_namespace tuple and check pg_namespace.nspparent,
		 *		 but it's faster to just search for the namespaceOid in the global
		 *		 package array instead; we have to do that anyway to find the package
		 */

		dbg_info->package = plugin_funcs.get_package( namespaceOid );
	}
	else
		dbg_info->package = InvalidOid;
#endif
}


/*
 * ---------------------------------------------------------------------
 * plpgsql_do_deposit()
 *
 *  This function handles the 'deposit' feature - that is, this function
 *  sets a given PL variable to a new value, supplied by the client.
 *
 *	do_deposit() is called when you type a new value into a variable in
 *  the local-variables window.
 *
 *  NOTE: For the convenience of the user, we first assume that the
 *		  provided value is an expression.  If it doesn't evaluate,
 *		  we convert the value into a literal by surrounding it with
 *		  single quotes.  That may be surprising if you happen to make
 *		  a typo, but it will "do the right thing" in most cases.
 *
 * Returns true on success, false on failure.
 */
static bool
plpgsql_do_deposit(ErrorContextCallback *frame, const char *var_name,
				   int lineno, const char *value)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) frame->arg;
	dbg_ctx       *dbg_info   = estate->plugin_info;
	PLpgSQL_datum *target;
	char      	  *select;
	PLpgSQL_expr  *expr;
	MemoryContext  curContext = CurrentMemoryContext;
	ResourceOwner  curOwner   = CurrentResourceOwner;
	bool		retval = false;

	target = find_datum_by_name(estate, var_name, lineno, NULL);
	if (!target)
		return false;

	/*
	 * Now build a SELECT statement that returns the requested value
	 *
	 * NOTE: we allocate 2 extra bytes for quoting just in case we
	 *       need to later (see the retry logic below)
	 */

	select = palloc( strlen( "SELECT " ) + strlen( value ) + 2 + 1 );

	sprintf( select, "SELECT %s", value );

	/*
	 * Note: we must create a dynamically allocated PLpgSQL_expr here - we
	 *       can't create one on the stack because exec_assign_expr()
	 *       links this expression into a list (active_simple_exprs) and
	 *       this expression must survive until the end of the current
	 *	     transaction so we don't free it out from under spl_plpgsql_xact_cb()
	 */

	expr = (PLpgSQL_expr *) palloc0( sizeof( *expr ));

#if (PG_VERSION_NUM < 110000)
	expr->dtype			= PLPGSQL_DTYPE_EXPR;
	expr->dno			= -1;
#endif
	expr->query			= select;
	expr->plan			= NULL;
#if (PG_VERSION_NUM <= 80400)
	expr->plan_argtypes		= NULL;
	expr->nparams			= 0;
#endif
	expr->expr_simple_expr		= NULL;

	BeginInternalSubTransaction( NULL );

	MemoryContextSwitchTo( curContext );

	PG_TRY();
	{
		if( target )
			dbg_info->assign_expr( estate, target, expr );

		/* Commit the inner transaction, return to outer xact context */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo( curContext );
		CurrentResourceOwner = curOwner;

		SPI_restore_connection();

		/* That worked, don't try again */
		retval = true;
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo( curContext );

		FlushErrorState();

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo( curContext );
		CurrentResourceOwner = curOwner;

		SPI_restore_connection();

		/* That failed - try again as a literal */
		retval = false;
	}
	PG_END_TRY();

	/*
	 * If the given value is not a valid expression, try converting
	 * the value into a literal by sinqle-quoting it.
	 */

	if (!retval)
	{
		sprintf( select, "SELECT '%s'", value );

#if (PG_VERSION_NUM < 110000)
		expr->dtype		= PLPGSQL_DTYPE_EXPR;
		expr->dno		= -1;
#endif
		expr->query		= select;
		expr->plan		= NULL;
		expr->expr_simple_expr	= NULL;
#if (PG_VERSION_NUM <= 80400)
		expr->plan_argtypes	= NULL;
		expr->nparams		= 0;
#endif

		BeginInternalSubTransaction( NULL );

		MemoryContextSwitchTo( curContext );

		PG_TRY();
		{
			if( target )
				dbg_info->assign_expr( estate, target, expr );

			/* Commit the inner transaction, return to outer xact context */
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo( curContext );
			CurrentResourceOwner = curOwner;

			SPI_restore_connection();

			retval = true;
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo( curContext );

			FlushErrorState();

			/* Abort the inner transaction */
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo( curContext );
			CurrentResourceOwner = curOwner;

			SPI_restore_connection();

			retval = false;
		}
		PG_END_TRY();
	}

	pfree( select );

	return retval;
}

/*
 * ---------------------------------------------------------------------
 * is_datum_visible()
 *
 *	This function determines whether the given datum is 'visible' to the
 *  debugger client.  We want to hide a few undocumented/internally
 *  generated variables from the user - this is the function that hides
 *  them.  We set a flag in the symbols entry for this datum
 *  to indicate whether this variable is hidden or visible - that way,
 *  only have to do the expensive stuff once per invocation.
 */
static bool
is_datum_visible(PLpgSQL_datum *datum)
{
	static const char * hidden_variables[] =
	{
		"found",
		"rowcount",
		"sqlcode",
		"sqlerrm",
		"_found",
		"_rowcount",
	};

	/*
	 * All of the hidden variables are scalars at the moment so
	 * assume that anything else is visible regardless of name
	 */

	if( datum->dtype != PLPGSQL_DTYPE_VAR )
		return( TRUE );
	else
	{
		PLpgSQL_var * var = (PLpgSQL_var *)datum;
		int			  i;

		for( i = 0; i < sizeof( hidden_variables ) / sizeof( hidden_variables[0] ); ++i )
		{
			if( strcmp( var->refname, hidden_variables[i] ) == 0 )
			{
				/*
				 * We found this variable in our list of hidden names -
				 * this variable is *not* visible
				 */

				return( FALSE );
			}
		}

		/*
		 * The SPL pre-processor generates a few variable names for
		 * DMBS.PUTLINE statements - we want to hide those variables too.
		 * The generated variables are of the form 'txtnnn...' where
		 * 'nnn...' is a sequence of one or more digits.
		 */

		if( strncmp( var->refname, "txt", 3 ) == 0 )
		{
			int	   i;

			/*
			 * Starts with 'txt' - see if the rest of the string is composed
			 * entirely of digits
			 */

			for( i = 3; var->refname[i] != '\0'; ++i )
			{
				if( var->refname[i] < '0' || var->refname[i] > '9' )
					return( TRUE );
			}

			return( FALSE );
		}

		return( TRUE );
	}
}

/*
 * ---------------------------------------------------------------------
 * is_var_visible()
 *
 *	This function determines whether the given variable is 'visible' to the
 *  debugger client. We hide some variables from the user (see the
 *  is_datum_visible() function for more info).  This function is quick -
 *  we do the slow work in is_datum_visible() and simply check the results
 *  here.
 */
static bool
is_var_visible(PLpgSQL_execstate *frame, int var_no)
{
	dbg_ctx * dbg_info = (dbg_ctx *)frame->plugin_info;

	if (dbg_info->symbols == NULL)
		completeFrame(frame);

	return( dbg_info->symbols[var_no].visible );
}


/*
 * plpgsql_send_cur_line()
 *
 * This function sends the current position to the debugger client. We
 * send the function's OID, xmin, cmin, and the current line number
 * (we're telling the client which line of code we're about to execute).
 */
static void
plpgsql_send_cur_line(ErrorContextCallback *frame)
{
	PLpgSQL_execstate *estate = (PLpgSQL_execstate *) frame->arg;
	PLpgSQL_stmt *stmt = estate->err_stmt;
	dbg_ctx	   *dbg_info = (dbg_ctx *) estate->plugin_info;
	PLpgSQL_function *func = dbg_info->func;

	dbg_send( "%d:%d:%s",
			  func->fn_oid,
			  stmt->lineno+1,
#if (PG_VERSION_NUM >= 90200)
			  func->fn_signature
#else
			  func->fn_name
#endif
		);
}

/*
 * ---------------------------------------------------------------------
 * isFirstStmt()
 *
 *	Returns true if the given statement is the first statement in the
 *  given function.
 */
static bool
isFirstStmt(PLpgSQL_stmt *stmt, PLpgSQL_function *func)
{
	if( stmt == linitial( func->action->body ))
		return( TRUE );
	else
		return( FALSE );
}

/*
 * ---------------------------------------------------------------------
 * dbg_newstmt()
 *
 *	The PL/pgSQL executor calls plpgsql_dbg_newstmt() just before executing each
 *	statement.
 *
 *	This function is the heart of the debugger.  If you're single-stepping,
 *	or you hit a breakpoint, plpgsql_dbg_newstmt() sends a message to the debugger
 *  client indicating the current line and then waits for a command from
 *	the user.
 *
 *	NOTE: it is very important that this function should impose negligible
 *	      overhead when a debugger client is *not* attached.  In other words
 *		  if you're running PL/pgSQL code without a debugger, you notice no
 *		  performance penalty.
 */
static void
dbg_newstmt(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt)
{
	PLpgSQL_execstate * frame = estate;

	/*
	 * If there's no debugger attached, go home as quickly as possible.
	 */
	if( frame->plugin_info == NULL )
		return;
	else
	{
		dbg_ctx 		  * dbg_info = (dbg_ctx *)frame->plugin_info;
		Breakpoint		  * breakpoint = NULL;
		eBreakpointScope	breakpointScope = 0;

		/*
		 * The PL compiler marks certain statements as 'invisible' to the
		 * debugger. In particular, the compiler may generate statements
		 * that do not appear in the source code. Such a statement is
		 * marked with a line number of -1: if we're looking at an invisible
		 * statement, just return to the caller.
		 */

		if( stmt->lineno == -1 )
			return;

		/*
		 * Now set up an error handler context so we can intercept any
		 * networking errors (errors communicating with the proxy).
		 */

		if( sigsetjmp( client_lost.m_savepoint, 1 ) != 0 )
		{
			/*
			 *  The connection to the debugger client has slammed shut -
			 *	just pretend like there's no debugger attached and return
			 *
			 *	NOTE: we no longer have a connection to the debugger proxy -
			 *		  that means that we cannot interact with the proxy, we
			 *		  can't wait for another command, nothing.  We let the
			 *		  executor continue execution - anything else will hang
			 *		  this backend, waiting for a debugger command that will
			 *		  never arrive.
			 *
			 *		  If, however, we hit a breakpoint again, we'll stop and
			 *		  wait for another debugger proxy to connect to us.  If
			 *		  that's not the behavior you're looking for, you can
			 *		  drop the breakpoint, or call free_function_breakpoints()
			 *		  here to get rid of all breakpoints in this backend.
			 */
			per_session_ctx.client_w = 0; 		/* No client connection */
			dbg_info->stepping 		 = FALSE; 	/* No longer stepping   */
		}

		if(( dbg_info->stepping ) || breakAtThisLine( &breakpoint, &breakpointScope, dbg_info->func->fn_oid, isFirstStmt( stmt, dbg_info->func ) ? -1 : stmt->lineno ))
			dbg_info->stepping = TRUE;
		else
			return;

		per_session_ctx.step_into_next_func = FALSE;

		/* We found a breakpoint for this function (or we're stepping into) */
		/* Make contact with the debugger client */

		if( !attach_to_proxy( breakpoint ))
		{
			/*
			 * Can't attach to the proxy, maybe we found a stale breakpoint?
			 * That can happen if you set a global breakpoint on a function,
			 * invoke that function from a client application, debug the target
			 * kill the debugger client, and then re-invoke the function from
			 * the same client application - we will find the stale global
			 * breakpoint on the second invocation.
			 *
			 * We want to remove that breakpoint so that we don't keep trying
			 * to attach to a phantom proxy process.
			 */
			if( breakpoint )
				BreakpointDelete( breakpointScope, &(breakpoint->key));

			/*
			 * In any case, if we don't have a proxy to work with, we can't
			 * do any debugging so give up.
			 */
			pfree( frame->plugin_info );
			frame->plugin_info       = NULL; /* No debugger context  */
			per_session_ctx.client_w = 0; /* No client connection */

			return;
		}

		if( stmt->cmd_type == PLPGSQL_STMT_BLOCK )
			return;

		/*
		 * The PL/pgSQL compiler inserts an automatic RETURN statement at the
		 * end of each function (unless the last statement in the function is
		 * already a RETURN). If we run into that statement, we don't really
		 * want to wait for the user to STEP across it. Remember, the user won't
		 * see the RETURN statement in the source-code listing for his function.
		 *
		 * Fortunately, the automatic RETURN statement has a line-number of 0
		 * so it's easy to spot.
		 */
		if( stmt->lineno == 0 )
			return;

		/*
		 * If we're in step mode, tell the debugger client, read a command from the client and
		 * execute the command
		 */

		if( dbg_info->stepping )
		{
			/*
			 * Make sure that we have all of the debug info that we need in this stack frame
			 */
			completeFrame( frame );

			/*
			 * We're in single-step mode (or at a breakpoint)
			 * send the current line number to the debugger client and report any
			 * variable modifications
			 */

			if (!plugin_debugger_main_loop())
				dbg_info->stepping = FALSE;
		}
	}
}

/* ---------------------------------------------------------------------
 *	datumIsNull()
 *
 *	determine whether datum is NULL or not.
 *	TODO: consider datatypes other than PLPGSQL_DTYPE_VAR as well
 */
static bool
datumIsNull(PLpgSQL_datum *datum)
{
	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
		{
			PLpgSQL_var *var = (PLpgSQL_var *) datum;

			if (var->isnull)
				return true;
		}
		break;

		/* other data types are not currently handled, we just return true */
		case PLPGSQL_DTYPE_REC:
		case PLPGSQL_DTYPE_ROW:
			return true;

		default:
			return true;
	}

	return false;
}
