
/*-------------------------------------------------------------------------
 *
 * plugin_profiler.c
 *
 *	  Profiling plugin for PL/pgSQL instrumentation
 *
 *		This plugin provides a profiler for PL/pgSQL functions.  For every
 *		line of PL/pgSQL code, we aggregate the following performance
 *		counters:
 *		  execution count (number of times each statement is executed)
 *		  total execution time (how long did we spend executing each statement?)
 *		  longest execution time (how long did the slowest iteration take?)
 *		  number of scans (total number of sequential and indexed scans)
 *		  blocks fetched
 *		  blocks hit (blocks found in buffer pool)
 *		  tuples returned
 *		  tuples fetched
 *		  tuples inserted
 *		  tuples updated
 *		  tuples deleted
 *
 *	Copyright 2006,2007 - EnterpriseDB, Inc.
 *
 * IDENTIFICATION
 *	  $EnterpriseDB: edb-postgres/contrib/debugger/plugin_tracer.c,v 1.0 2005/12/15 02:49:32 xrad Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>
#include <sys/time.h>
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "catalog/namespace.h"
#include "plpgsql.h"
#include "pgstat.h"
#include "plugin_helpers.h"

PG_MODULE_MAGIC;	/* Tell the server about our compile environment */

/**********************************************************************
 * Type and structure definitions
 **********************************************************************/

#if PG_VERSION_NUM >= 80300
typedef	PgStat_TableCounts ioStatsType;
#else
typedef PgStat_TableEntry  ioStatsType;
#define SET_VARSIZE(varlena, len)	(VARATT_SIZEP((varlena)) = (len))
#endif

/* -------------------------------------------------------------------
 * perStmtStats
 *
 *	We allocate one perStmtStats structure for each statement in a 
 *  given function. (FIXME: we actuallty execute one perStmtStats 
 *	for each line, regardless of how many statements exist on each
 *  line).
 */
typedef struct
{
	ioStatsType		 		ioStats;		/* Tuple- and block-level performance counters 		*/
	struct timeval			timeLongest;	/* Slowest iteration of this stmt		   			*/
	struct timeval			timeTotal;		/* Total amount of time spent executing this stmt	*/
	PgStat_Counter			execCount;		/* Number of times we've executed this stmt			*/

	ioStatsType				begStats;		/* Starting I/O stats for this statement			*/
	struct timeval		    begTime;		/* Start time for this statement 					*/
} perStmtStats;

/* -------------------------------------------------------------------
 * profilerCtx
 *
 *	We allocate one profilerCtx for each stack frame (that is, each
 *  invocation of a PL/pgSQL function).
 *
 *	We use this structure to keep track of the source code for the
 *	function and to keep track of an array of perStmtStats structures
 *  (one for each line of code)
 */

typedef struct
{
	int						lineCount;		/* Number of lines-of-code in this function				 	*/
	const char           **	sourceLines;	/* Pointers to null-terminated source code for each line 	*/
	perStmtStats          * stmtStats; 		/* Pointers to performance counters for each line		 	*/
	bool					suppressZeroes;	/* If TRUE, omit zero-valued performance counters from XML	*/
} profilerCtx;


static char * xmlFileName    = NULL;
static char * statsTableName = NULL;

/**********************************************************************
 * Exported function prototypes
 **********************************************************************/

void load_plugin( PLpgSQL_plugin * hooks );

/**********************************************************************
 * Helper function prototypes
 **********************************************************************/
static void 		  profiler_init( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		  profiler_func_beg( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		  profiler_func_end( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		  profiler_stmt_beg( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );
static void 		  profiler_stmt_end( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );

static perStmtStats *getStatsForStmt( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );
static void          dumpStats( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		 dumpStatsTable( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		 dumpStatsXML( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static bool 		 statsAlreadyExist( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		 updateStats( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		 insertStats( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void 		 initArgTypesInsert( Oid argTypes[14] );
static void 		 initArgTypesUpdate( Oid argTypes[13] );
static const char 	*createTable( const char * qualifiedName );
static bool 		 tableExists( const char * qualifiedName );

	
/**********************************************************************
 * Exported Function definitions
 **********************************************************************/

/* -------------------------------------------------------------------
 * load_plugin()
 *
 *	This function must be called when this plugin is loaded - it 
 *  fills in the function pointers in the given PLpgSQL_plugin struct
 * -------------------------------------------------------------------
 */

static PLpgSQL_plugin plugin_funcs = { profiler_init, profiler_func_beg, profiler_func_end, profiler_stmt_beg, profiler_stmt_end };

void _PG_init( void )
{
	PLpgSQL_plugin ** var_ptr = (PLpgSQL_plugin **) find_rendezvous_variable( "PLpgSQL_plugin" );

	*var_ptr = &plugin_funcs;

	/*
	 * NOTE: we have to wrap this code in a PG_TRY() block because there (currently) is no way to find out if a given 
	 *		 GUC variable already exists - you can call GetConfigOptionByName(), but that will throw an error if the
	 *		 variable doesn't exist.  So, we just try to define our custom variables and intercept the error if the
	 *		 variable is already known.
	 *
	 *		 We can enounter an existing variable if the user calls a PL/pgSQL function (thus loading this plugin),
	 *		 and then LOAD's the plugin manually (throwing out the old incarnation of the plugin and loading a new
	 *		 instance), and finally invokes a PL/pgSQL function again. 
	 */

	PG_TRY();
	{

#if 0
	NOTE: our XML support is pretty flawed at the moment so we will just disable
		  it.

		DefineCustomStringVariable( "plpgsql.profiler_filename",
									"Pathname of PL/pgSQL profile file",
									NULL,
									&xmlFileName,
									PGC_USERSET,
									NULL, 
									NULL );
#endif

		DefineCustomStringVariable( "plpgsql.profiler_tablename",
									"Name of PL/pgSQL profile table",
									NULL,
									&statsTableName,
									PGC_USERSET,
									NULL,
									NULL );

	}
	PG_END_TRY();
}
        
void load_plugin( PLpgSQL_plugin * hooks )
{
	hooks->func_setup = profiler_init;
	hooks->func_beg   = profiler_func_beg;
	hooks->func_end   = profiler_func_end;
	hooks->stmt_beg   = profiler_stmt_beg;
	hooks->stmt_end   = profiler_stmt_end;
}

/**********************************************************************
 * Static Function definitions
 **********************************************************************/

/**********************************************************************
 * Hook functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * profiler_init()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a 
 *  new function is about to start.  Specifically, this instrumentation
 *  hook is called after the stack frame has been created, but before
 *  values are assigned to the local variables.
 *
 *	'estate' points to the stack frame for this function, 'func' 
 *	points to the definition of the function
 *
 *  We use this hook to load the source code for the function that's
 *  being invoked and to set up our context structures
 * -------------------------------------------------------------------
 */

static void profiler_init( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	HeapTuple	  procTuple;
	char        * funcName;
	profilerCtx * profilerInfo;
	char		* procSrc;

	/* If we don't have a value for plpgsql.profiler_filename, just go home */
	if(( xmlFileName == NULL ) && (( statsTableName == NULL ) || ( statsTableName[0] == '\0' )))
	{
		estate->plugin_info = NULL;
		return;
	}
	/*
	 * The PL/pgSQL interpreter provides a void pointer (in each stack frame) that's reserved
	 * for plugins.  We allocate a profilerCtx structure and record it's address in that
	 * pointer so we can keep some per-invocation information.
	 */
	profilerInfo = (profilerCtx *) palloc( sizeof( profilerCtx ));

	estate->plugin_info = profilerInfo;

	/* Allocate enough space to hold a pointer to each line of source code */
	procSrc = findSource( funcGetOid( func ), funcGetPkgOid( func ), &procTuple, &funcName );

	profilerInfo->lineCount   = scanSource( NULL, procSrc );
	profilerInfo->stmtStats   = palloc0( profilerInfo->lineCount * sizeof( perStmtStats ));
	profilerInfo->sourceLines = palloc( profilerInfo->lineCount * sizeof( const char * ));
	
	/* FIXME: provide a way for the user to suppress/report zero-valued performance counters */

	profilerInfo->suppressZeroes = false;

	/* Now scan through the source code for this function so we know where each line begins */

	scanSource( profilerInfo->sourceLines, procSrc );

	ReleaseSysCache( procTuple );
}

/* -------------------------------------------------------------------
 * profiler_func_beg()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a 
 *  new function is starting.  Specifically, this instrumentation
 *  hook is called after values have been assigned to all local 
 *	variables (and all function parameters).
 *
 *	'estate' points to the stack frame for this function, 'func' 
 *	points to the definition of the function
 * -------------------------------------------------------------------
 */

static void profiler_func_beg( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
}

/* -------------------------------------------------------------------
 * profiler_func_end()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a 
 *  function runs to completion.
 * -------------------------------------------------------------------
 */

static void profiler_func_end( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	/* estate->plugin_info will be NULL if we don't have a file/table to write to */
	if( estate->plugin_info == NULL )
		return;

	/* 
	 * We have accumulated performance statistics for this invocation, now dump 
	 * those statistics to an XML file
	 */
	dumpStats( estate, func );
}

/* -------------------------------------------------------------------
 * profiler_stmt_beg()
 *
 *	This hook function is called by the PL/pgSQL interpreter just before
 *	executing a statement (stmt).
 *
 *  Prior to executing each statement, we record the current time and 
 *  the current values of all of the performance counters.
 * -------------------------------------------------------------------
 */

static void profiler_stmt_beg( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
	perStmtStats    * stats;
	struct timezone   tz;

	/* estate->plugin_info will be NULL if we don't have a file/table to write to */
	if( estate->plugin_info == NULL )
		return;

	/* Record the performance counters so we can 'delta' them later */
	stats = getStatsForStmt( estate, stmt );

#ifdef HAVE_PGSTATGLOBALS
	stats->begStats = pgStatGlobals;
#endif

	gettimeofday( &stats->begTime, &tz );
}

/* -------------------------------------------------------------------
 * profiler_stmt_end()
 *
 *	This hook function is called by the PL/pgSQL interpreter just after
 *	it executes a statement (stmt).
 *
 *	We use this hook to 'delta' the before and after performance counters
 *  and record the differences in the perStmtStats structure associated 
 *  with this statement.
 * -------------------------------------------------------------------
 */

static void profiler_stmt_end( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
	perStmtStats    * stats;
	struct timeval    endTime;
	struct timezone   tz;

	/* estate->plugin_info will be NULL if we don't have a file/table to write to */
	if( estate->plugin_info == NULL )
		return;

	/* Grab the current time */

	gettimeofday( &endTime, &tz );

	/* Now increment the performance counters associated with this statement */
	stats = getStatsForStmt( estate, stmt );

#ifdef HAVE_PGSTATGLOBALS
	stats->ioStats.t_numscans        += ( pgStatGlobals.t_numscans        - stats->begStats.t_numscans );
	stats->ioStats.t_tuples_returned += ( pgStatGlobals.t_tuples_returned - stats->begStats.t_tuples_returned );
	stats->ioStats.t_tuples_fetched  += ( pgStatGlobals.t_tuples_fetched  - stats->begStats.t_tuples_fetched );
	stats->ioStats.t_tuples_inserted += ( pgStatGlobals.t_tuples_inserted - stats->begStats.t_tuples_inserted );
	stats->ioStats.t_tuples_updated  += ( pgStatGlobals.t_tuples_updated  - stats->begStats.t_tuples_updated );
	stats->ioStats.t_tuples_deleted  += ( pgStatGlobals.t_tuples_deleted  - stats->begStats.t_tuples_deleted );
	stats->ioStats.t_blocks_fetched  += ( pgStatGlobals.t_blocks_fetched  - stats->begStats.t_blocks_fetched );
	stats->ioStats.t_blocks_hit      += ( pgStatGlobals.t_blocks_hit      - stats->begStats.t_blocks_hit );
#endif
    /* Normalize the elapsed time for this statement */

	if( stats->begTime.tv_usec > endTime.tv_usec )
	{
	    endTime.tv_usec += 1000000;
	    endTime.tv_sec  -= 1;
	}

	endTime.tv_sec  -= stats->begTime.tv_sec;
	endTime.tv_usec -= stats->begTime.tv_usec;

    /*
     *  Update the 'longest' value if this iteration has taken longer than all other
	 *  iterations of this statement.
	 */

	if( endTime.tv_sec > stats->timeLongest.tv_sec )
		stats->timeLongest = endTime;
	else if(( endTime.tv_sec == stats->timeLongest.tv_sec ) && ( endTime.tv_usec > stats->timeLongest.tv_usec ))
		stats->timeLongest = endTime;

    /* Now add to the total time for this statement */

	if(( stats->timeTotal.tv_usec + endTime.tv_usec ) > 1000000 )
	{
	    stats->timeTotal.tv_sec  += endTime.tv_sec + 1;
	    stats->timeTotal.tv_usec += endTime.tv_usec;
	    stats->timeTotal.tv_usec -= 1000000;
	}
	else
	{
	    stats->timeTotal.tv_sec  += endTime.tv_sec;
	    stats->timeTotal.tv_usec += endTime.tv_usec;
	}
	
    /* And update the execution count */

	stats->execCount++;
}

/**********************************************************************
 * Helper functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * getStatsForStmt()
 *
 *	This helper function returns a pointer to the perStmtStats 
 *  structure associated with the given statement.  Nothing really
 *  magical in this function, just factored out from the caller(s)
 *  in case we ever change the way we store the performance counters
 * -------------------------------------------------------------------
 */

static perStmtStats * getStatsForStmt( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
	profilerCtx * profilerInfo = (profilerCtx *) estate->plugin_info;

	return( profilerInfo->stmtStats + stmt->lineno );
}

/* -------------------------------------------------------------------
 * dumpStats()
 *
 *	dumpStats() is called at the end of each PL/pgSQL function - it 
 *	writes execution statistics to the profile table (if statsTableName
 *	is not null) and to an XML log file (if xmlFileName is not NULL).
 */

static void dumpStats( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	/* If the user gave us an XML file name, write stats to that file */
	if( xmlFileName )
		dumpStatsXML( estate, func );

	/* If the user gave us a table name, insert/update stats in that table */
	if( statsTableName )
		dumpStatsTable( estate, func );
}

/* -------------------------------------------------------------------
 * dumpStatsTable()
 *
 *	This function decides whether to update existing stats in the 
 *	user-defined table or to insert new stats.  We update if there 
 *	are any stats already in place (for the given function), otherwise,
 *  we insert a new set of stats.
 *
 *	You can choose the table name, but we expect the column names and
 *  data types to match the CREATE TABLE statement shown below (if you 
 *  want to add more columns, add them to the end or fix the INSERT 
 *  statement in insertStats()).
 */
 
/*
  CREATE TABLE profiler_stats( 
      sourceCode TEXT, 
	  func_oid OID, 
	  line_number INT, 
	  exec_count INT8, 
	  tuples_returned INT8, 
	  time_total FLOAT8, 
	  time_longest FLOAT8, 
	  num_scans INT8, 
	  tuples_fetched INT8, 
	  tuples_inserted INT8, 
	  tuples_updated INT8, 
	  tuples_deleted INT8, 
	  blocks_fetched INT8, 
	  blocks_hit INT8
    ); 

   CREATE UNIQUE INDEX profiler_stats_pkey ON profiler_stats( func_oid, line_number );
*/

static void dumpStatsTable( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	/* Connect to the SPI so our helper functions don't have to worry about that */
	SPI_push();

	if( SPI_connect() < 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("plugin_profiler: could not connect to SPI")));
	}

	if( statsAlreadyExist( estate, func ))
		updateStats( estate, func );
	else
		insertStats( estate, func );

	SPI_finish();
	SPI_pop();

}

/* -------------------------------------------------------------------
 * updateStats()
 *
 *	updateStats() will update existing statistics in the user-defined
 *  profile table.  Since we do a line-by-line UPDATE, you must ensure
 *  that the function definition remains consistent - if you change 
 *  the function, be sure to delete the execution stats for that 
 *  function (if you don't, the line numbers will get confused).
 *
 *	If you change a function (whose OID, for example, is 65322), then
 *  be sure to delete any statistics for that function like this:
 *	  DELETE FROM profiler_stats WHER func_oid = 65322;
 */

static void updateStats( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	profilerCtx     * profilerInfo = (profilerCtx *) estate->plugin_info;
	int				  lineNo;
	void			* updatePlan;
	Datum			  values[13];
	Oid				  argTypes[13];
	char 			* updateStmt;
	const char * rawStmt = "UPDATE %s SET "
		" exec_count      = exec_count + $3,"
		" tuples_returned = tuples_returned + $4,"
		" time_total      = time_total + $5,"
		" time_longest    = time_longest + $6,"
		" num_scans       = num_scans + $7,"
		" tuples_fetched  = tuples_fetched + $8,"
		" tuples_inserted = tuples_inserted + $9,"
		" tuples_updated  = tuples_updated + $10,"
		" tuples_deleted  = tuples_deleted + $11,"
		" blocks_fetched  = blocks_fetched + $12,"
		" blocks_hit      = blocks_hit + $13"
		" WHERE func_oid  = $1 AND line_number = $2";

	initArgTypesUpdate( argTypes );

	/* Plug the user-chosen table name into the UPDATE statment */
	updateStmt = palloc( strlen( rawStmt ) + strlen( statsTableName ));
	sprintf( updateStmt, rawStmt, statsTableName );

	/* Prepare the UPDATE statment */
	updatePlan = SPI_prepare( updateStmt, 13, argTypes );

	/* Loop through each line of source code and update the stats */
	for( lineNo = 0; lineNo < profilerInfo->lineCount; ++lineNo )
	{
		perStmtStats * stats = profilerInfo->stmtStats + lineNo;

		values[0]  = ObjectIdGetDatum( func->fn_oid );
		values[1]  = Int32GetDatum((int32) lineNo );
		values[2]  = Int64GetDatum( stats->execCount );
		values[3]  = Int64GetDatum( stats->ioStats.t_tuples_returned );
		values[4]  = Float8GetDatum( stats->timeTotal.tv_sec + ( stats->timeTotal.tv_usec / 1000000.0 ));
		values[5]  = Float8GetDatum( stats->timeLongest.tv_sec + ( stats->timeLongest.tv_usec / 1000000.0 ));
		values[6]  = Int64GetDatum( stats->ioStats.t_numscans );
		values[7]  = Int64GetDatum( stats->ioStats.t_tuples_fetched );
		values[8]  = Int64GetDatum( stats->ioStats.t_tuples_inserted );
		values[9]  = Int64GetDatum( stats->ioStats.t_tuples_updated );
		values[10] = Int64GetDatum( stats->ioStats.t_tuples_deleted );
		values[11] = Int64GetDatum( stats->ioStats.t_blocks_fetched );
		values[12] = Int64GetDatum( stats->ioStats.t_blocks_hit );

		if( SPI_execp( updatePlan, values, NULL, 1 ) != SPI_OK_UPDATE )
		{
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("plugin_profiler: error updating profiler data")));
		}
	}

	/* Clean up */
	pfree( updateStmt );
	SPI_pfree( updatePlan );
}

/* -------------------------------------------------------------------
 * insertStats()
 *
 *	insertStats() will write execution statistics into the table 
 *  chosen by the user.
 */

static void insertStats( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	profilerCtx     * profilerInfo = (profilerCtx *) estate->plugin_info;
	int				  lineNo;
	void			* insertPlan;
	Datum			  values[14];
	Oid				  argTypes[14];
	char 			* insertStmt;
	const char      * rawStmt = "INSERT INTO %s VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14)";

	initArgTypesInsert( argTypes );

	/* Plug the user-chosen table name into the INSERT statment */
	insertStmt = palloc( strlen( rawStmt ) + strlen( statsTableName ));
	sprintf( insertStmt, rawStmt, statsTableName );

	insertPlan = SPI_prepare( insertStmt, 14, argTypes );

	/* Loop through each line of source code and INSERT the stats */
	for( lineNo = 0; lineNo < profilerInfo->lineCount; ++lineNo )
	{
		perStmtStats * stats = profilerInfo->stmtStats + lineNo;
		size_t sourceCodeLen = strlen( profilerInfo->sourceLines[lineNo] );
		char * sourceCode 	 = palloc( VARHDRSZ + sourceCodeLen );

		memcpy( VARDATA( sourceCode ), profilerInfo->sourceLines[lineNo], sourceCodeLen );
		SET_VARSIZE( sourceCode , VARHDRSZ + sourceCodeLen );

		values[0]  = PointerGetDatum( sourceCode );
		values[1]  = ObjectIdGetDatum( func->fn_oid );
		values[2]  = Int32GetDatum((int32) lineNo );
		values[3]  = Int64GetDatum( stats->execCount );
		values[4]  = Int64GetDatum( stats->ioStats.t_tuples_returned );
		values[5]  = Float8GetDatum( stats->timeTotal.tv_sec + ( stats->timeTotal.tv_usec / 1000000.0 ));
		values[6]  = Float8GetDatum( stats->timeLongest.tv_sec + ( stats->timeLongest.tv_usec / 1000000.0 ));
		values[7]  = Int64GetDatum( stats->ioStats.t_numscans );
		values[8]  = Int64GetDatum( stats->ioStats.t_tuples_fetched );
		values[9]  = Int64GetDatum( stats->ioStats.t_tuples_inserted );
		values[10] = Int64GetDatum( stats->ioStats.t_tuples_updated );
		values[11] = Int64GetDatum( stats->ioStats.t_tuples_deleted );
		values[12] = Int64GetDatum( stats->ioStats.t_blocks_fetched );
		values[13] = Int64GetDatum( stats->ioStats.t_blocks_hit );

		if( SPI_execp( insertPlan, values, NULL, 1 ) != SPI_OK_INSERT )
		{
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("plugin_profiler: error inserting profiler data")));
		}

		pfree( sourceCode );
	}

	pfree( insertStmt );
	SPI_pfree( insertPlan );
}

/* -------------------------------------------------------------------
 * statsAlreadyExist()
 *
 * 	This function determines whether statistics already exist (in the 
 *  user-chosen table) for the given function.  If we find any rows
 *  that match the OID of the function that we just completed, we 
 *  return TRUE and our caller UPDATE's those existing statistics.  
 *  If we can't find any rows for the just-completed function, we 
 *  return FALSE and our caller INSERT's a new set of statistics.
 */

static bool statsAlreadyExist( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	const char * rawStmt     = "SELECT exec_count FROM %s WHERE func_oid = $1 LIMIT 1";
	Oid			 argTypes[1] = { OIDOID };
	Datum		 values[1]   = { ObjectIdGetDatum( func->fn_oid ) };
	char	   * selectStmt;
	void	   * selectPlan;	
	bool		 result;

	if( !tableExists( statsTableName ))
		createTable( statsTableName );

	selectStmt = palloc( strlen( rawStmt ) + strlen( statsTableName ));
	sprintf( selectStmt, rawStmt, statsTableName );

	selectPlan = SPI_prepare( selectStmt, 1, argTypes );

	if( SPI_execp( selectPlan, values, NULL, 1 ) != SPI_OK_SELECT )
	{
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("plugin_profiler: error querying profiler data")));
	}

	if( SPI_processed == 0 )
		result = FALSE;
	else
		result = TRUE;

	pfree( selectStmt );
	SPI_pfree( selectPlan );

	return( result );
}

/* -------------------------------------------------------------------
 * tableExists()
 *
 * 	This function determines whether the given table name (which may
 *  be schema qualified) exists.
 */

static bool tableExists( const char * qualifiedName )
{
	RangeVar   *relVar;

#if PG_VERSION_NUM >= 80300
	relVar = makeRangeVarFromNameList(stringToQualifiedNameList(qualifiedName));
#else
	relVar = makeRangeVarFromNameList(stringToQualifiedNameList(qualifiedName, "profiler"));
#endif

	if( OidIsValid( RangeVarGetRelid( relVar, true )))
		return true;
	else
		return false;
}

/* -------------------------------------------------------------------
 * createTable()
 *
 * 	This function creates a profiler statistics table of the proper 
 *  shape.  The table will be named <qualifiedName>, which may be
 *  schema-qualified.
 */

static const char * createTable( const char * qualifiedName )
{
	StringInfoData	buf;
	const char     *createTableString = 
		" CREATE TABLE %s ( "
		"  sourceCode TEXT, "
		"  func_oid OID, "
		"  line_number INT, "
		"  exec_count INT8, "
		"  tuples_returned INT8, "
		"  time_total FLOAT8, "
		"  time_longest FLOAT8, "
		"  num_scans INT8, "
		"  tuples_fetched INT8, "
		"  tuples_inserted INT8, "
		"  tuples_updated INT8, "
		"  tuples_deleted INT8, "
		"  blocks_fetched INT8, "
		"  blocks_hit INT8"
		" );";
	const char  *createIndexString = 
		" CREATE UNIQUE INDEX %s_pkey ON %s( func_oid, line_number );";

	initStringInfo( &buf );

	appendStringInfo( &buf, createTableString, qualifiedName );

	SPI_exec( buf.data, 0 );

	initStringInfo( &buf );

	appendStringInfo( &buf, createIndexString, qualifiedName, qualifiedName );
	
	SPI_exec( buf.data, 0 );
	
	return qualifiedName;
}


/* -------------------------------------------------------------------
 * initArgTypesInsert()
 *
 *	This is a helper function that builds an array of argument-type
 *  OIDs.  The argTypes array constructed by this function must match
 *  the layout of the profile table and the layout of the INSERT
 *  statement in insertStats()
 */

static void initArgTypesInsert( Oid argTypes[14] )
{
	argTypes[0]  = TEXTOID;		/* Source Code Text */
	argTypes[1]  = OIDOID;		/* Function OID		*/
	argTypes[2]  = INT4OID;		/* Line number  	*/
	argTypes[3]  = INT8OID;		/* Execution Count	*/
	argTypes[4]  = INT8OID;		/* Tuples Returned	*/
	argTypes[5]  = FLOAT8OID;	/* Time Total		*/
	argTypes[6]  = FLOAT8OID;	/* Time Longest		*/
	argTypes[7]  = INT8OID;		/* Numscans			*/
	argTypes[8]  = INT8OID;		/* Tuples Fetched	*/
	argTypes[9]  = INT8OID;		/* Tuples Inserted	*/
	argTypes[10] = INT8OID;		/* Tuples Updated	*/
	argTypes[11] = INT8OID;		/* Tuples Deleted	*/
	argTypes[12] = INT8OID;		/* Blocks Fetched	*/
	argTypes[13] = INT8OID;		/* Blocks Hit		*/
}

/* -------------------------------------------------------------------
 * initArgTypesUpdate()
 *
 *	This is a helper function that builds an array of argument-type
 *  OIDs.  The argTypes array constructed by this function must match
 *  the layout of the profile table and the layout of the UPDATE 
 *  statement in updateStats()
 */

static void initArgTypesUpdate( Oid argTypes[13] )
{
	argTypes[0]  = OIDOID;		/* Function OID		*/
	argTypes[1]  = INT4OID;		/* Line number  	*/
	argTypes[2]  = INT8OID;		/* Execution Count	*/
	argTypes[3]  = INT8OID;		/* Tuples Returned	*/
	argTypes[4]  = FLOAT8OID;	/* Time Total		*/
	argTypes[5]  = FLOAT8OID;	/* Time Longest		*/
	argTypes[6]  = INT8OID;		/* Numscans			*/
	argTypes[7]  = INT8OID;		/* Tuples Fetched	*/
	argTypes[8]  = INT8OID;		/* Tuples Inserted	*/
	argTypes[9]  = INT8OID;		/* Tuples Updated	*/
	argTypes[10] = INT8OID;		/* Tuples Deleted	*/
	argTypes[11] = INT8OID;		/* Blocks Fetched	*/
	argTypes[12] = INT8OID;		/* Blocks Hit		*/
}


/* -------------------------------------------------------------------
 * dumpStatsXML()
 *
 *	This function writes an XML-formatted report that details the 
 *  per-statement performance counters for the given invocation.
 *
 *  For each source-code line in the function, we write the text
 *  of the source code, the execution count, the total (and longest)
 *  time spent executing that statement, and I/O statistics for 
 *  that line of code.
 * -------------------------------------------------------------------
 */

static void dumpStatsXML( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	profilerCtx     * profilerInfo = (profilerCtx *) estate->plugin_info;
	bool			  dumpZeroes   = !profilerInfo->suppressZeroes;
	FILE			* dst          = fopen( xmlFileName, "w" );
	int				  lineNo;

	fprintf( dst, "<?xml version='1.0'?>\n" );
	fprintf( dst, "<profile>\n" );

	/* Loop through each line of source code */

	for( lineNo = 0; lineNo < profilerInfo->lineCount; ++lineNo )
	{
		perStmtStats * stats = profilerInfo->stmtStats + lineNo;

		fprintf( dst, "  <line lineNo='%d'>\n", lineNo );
		fprintf( dst, "    <src>" ); xmlEncode( dst, profilerInfo->sourceLines[lineNo], strlen( profilerInfo->sourceLines[lineNo] )); fprintf( dst, "</src>\n" );
		fprintf( dst, "    <stats>\n" );

		if( dumpZeroes || stats->execCount )
		    fprintf( dst, "      <executions      value='%lld'/>\n", stats->execCount );

		if( dumpZeroes || stats->ioStats.t_tuples_returned )
			fprintf( dst, "      <tuples_returned value='%lld'/>\n", stats->ioStats.t_tuples_returned );

		if( dumpZeroes || stats->timeTotal.tv_sec + stats->timeTotal.tv_usec )
			fprintf( dst, "      <totalTime       value='%ld.%07ld'/>\n", stats->timeTotal.tv_sec, stats->timeTotal.tv_usec );

		if( dumpZeroes || stats->timeLongest.tv_sec + stats->timeLongest.tv_usec )
			fprintf( dst, "      <longestTime     value='%ld.%07ld'/>\n", stats->timeLongest.tv_sec, stats->timeLongest.tv_usec );

		if( dumpZeroes || stats->ioStats.t_numscans )
			fprintf( dst, "      <numscans        value='%lld'/>\n", stats->ioStats.t_numscans );

		if( dumpZeroes || stats->ioStats.t_tuples_fetched )
			fprintf( dst, "      <tuples_other    value='%lld'/>\n", stats->ioStats.t_tuples_fetched );

		if( dumpZeroes || stats->ioStats.t_tuples_inserted )
			fprintf( dst, "      <tuples_inserted value='%lld'/>\n", stats->ioStats.t_tuples_inserted );

		if( dumpZeroes || stats->ioStats.t_tuples_updated )
			fprintf( dst, "      <tuples_updated  value='%lld'/>\n", stats->ioStats.t_tuples_updated );

		if( dumpZeroes || stats->ioStats.t_tuples_deleted )
			fprintf( dst, "      <tuples_deleted  value='%lld'/>\n", stats->ioStats.t_tuples_deleted );

		if( dumpZeroes || stats->ioStats.t_blocks_fetched )
			fprintf( dst, "      <blocks_fetched  value='%lld'/>\n", stats->ioStats.t_blocks_fetched );

		if( dumpZeroes || stats->ioStats.t_blocks_hit )
			fprintf( dst, "      <blocks_hit      value='%lld'/>\n", stats->ioStats.t_blocks_hit );

		fprintf( dst, "    </stats>\n" );
		fprintf( dst, "  </line>\n" );

	}

	fprintf( dst, "</profile>\n" );

	fclose( dst );
}

#include "plugin_helpers.c"
