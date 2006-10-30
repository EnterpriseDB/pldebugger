/**********************************************************************
 * dbginfo_pgsql.h - PL/pgSQL-specific support definitions for PL debugger 
 *				     client applications
 *
 *  Copyright 2006 - EnterpriseDB, Inc.  All Rights Reserved
 *
 **********************************************************************/

/*******************************************************************************
 * Structure targetInfo
 *
 *	When you call the pldbg_get_target_info() function, we build a structure of
 *  type targetInfo and pass that structure to a bunch of helper functions.  We
 *  do that to keep the argument list manageable.
 *
 *	There are three parts to this structure: before you call get_target_def()
 *	you must fill in rawName and targetType.  The middle section is where the 
 *	results accumulate.  The last section (labeled 'working context') is space
 *	that we use for the process of gathering the results.
 */

typedef struct
{
	/* Input values */
	char      * rawName;				/* Raw target name given by the user 									*/
	char	    targetType;				/* f(unction), p(rocedure), o(id), or t(rigger)							*/

	/* Results */
	bool	   isFunc;					/* Does rawName correspond to a function (true) or procedure (false)	*/
	int		   nargs;					/* Number of arguments expected by the target function/procedure		*/
	Oid	       argTypes[FUNC_MAX_ARGS]; /* Argument types expected by target									*/
	Oid		   targetOid;				/* OID of the target (may point to a pg_proc or edb_pkgelements tuple)	*/
	Oid		   packageOid;				/* OID of the package in which target is defined (or InvalidOid)		*/
	Oid		   schemaOid;				/* OID of the schema (pg_namespace tuple) in which target is defined	*/
	char     * targetName;				/* Target name															*/
	Datum      argModes;				/* Argument modes (in/out/both)											*/
	Datum	   argNames;				/* Argument names														*/
	Oid	  	   langOid;				    /* OID of pg_language in which target is written (edb-spl or pl/pgsql)	*/
	char     * fqName;					/* Fully-qualified name (schema.package.target or schema.target)		*/
	bool	   returnsSet;				/* TRUE -> target returns SETOF values									*/
	Oid		   returnType;				/* OID of pg_type which corresponds to target return type				*/
	int		   requiredArgs;			/* Number of required arguments	 (nargs - default-valued-arguments) 	*/

	/* Working context */
	List  	 * names;					/* Parsed-out name components (schema, package, target)					*/
	char  	 * schemaName;				/* Schema name															*/
	char  	 * funcName;				/* Function/procedure name												*/
	CatCList * catlist;					/* SysCacheList to release when finished								*/
} targetInfo;

