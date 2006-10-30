/**********************************************************************
 * dbginfo_pgsql.c - PL/pgSQL-specific support functions for PL 
 *					 debugger client applications
 *
 *	NOTE: This file is #included by dbginfo.c - it is not intended as
 *		  a stand-alone compile unit and will not compile by itself.
 *		  We isolate language-specific code into two different C files
 *		  and include the appropriate code depending on our compile 
 *		  environment. 
 *
 *  Copyright 2006 - EnterpriseDB, Inc.  All Rights Reserved
 *
 **********************************************************************/

/*******************************************************************************
 * parseQualifiedName()
 *
 *	This function splits a name list (info->names) into schema, package, and 
 *	function (or procedure) name.  It also retrieves the OID of the schema and
 *	package (if appropriate).  
 *
 *	NOTE: we split this code out into a separate function because the PL/pgSQL
 *		  implementation differs from the EDB SPL implementation.
 */

static void parseQualifiedName( targetInfo * info )
{
	info->packageOid = InvalidOid;

	DeconstructQualifiedName( info->names, &info->schemaName, &info->funcName );

}

/*******************************************************************************
 * getPackageName()
 *
 *	This helper function, given the OID of a package, returns the name of that
 *	package.
 *
 *	NOTE: PL/pgSQL does not support the notion of a package, but EDB's SPL does.
 *		  Please do not remove this function (and please leave any calls to this
 *		  function in place).
 */

static char * getPackageName( Oid packageOid )
{
	return( NULL );
}

/*******************************************************************************
 * getPackageTargetFromOid()
 *
 *	Given a package OID and a target OID, this function completes the targetInfo
 *	structure for the target function/procedure.  If the package OID and target 
 *	OID are identical, we assume that the caller wants us to report information 
 *	about the package initializer.
 *
 *	NOTE: PL/pgSQL does not support the notion of a package, but EDB's SPL does.
 *		  Please do not remove this function (and please leave any calls to this
 *		  function in place).
 */	

static bool getPackageTargetFromOid( targetInfo * info )
{
	return( FALSE );
}
	 
/*******************************************************************************
 * getPkgTarget()
 *
 *	This helper function is called (indirectly) by pldbg_get_target_info() to 
 *	retrieve information about a function or procedure that defined within a 
 *  package.
 *
 *  The caller already parsed out the raw name and decided that the user did
 *  include a package name.
 *
 *	This function is much less complex than getGlobalTarget() because we have 
 *  already identified the schema that contains the requested package.
 *
 *	If we find a match, we fill in the targetInfo structure with information
 *  about the target that we've identified.
 *
 *	NOTE: PL/pgSQL does not support the notion of a package, but EDB's SPL does.
 *		  Please do not remove this function (and please leave any calls to this
 *		  function in place).
 */

static bool getPkgTarget( targetInfo * info )
{
	return( FALSE );
}

/*******************************************************************************
 * completeProcTarget()
 *
 *	Given a pg_proc tuple, this function copies the target definition into
 *	the targetInfo structure.
 *
 *	NOTE: we split this code out into a separate function because the PL/pgSQL
 *		  implementation differs from the EDB SPL implementation.
 */

static void completeProcTarget( targetInfo * info, HeapTuple proctup )
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT( proctup );
	bool		 modesNull;
	bool		 namesNull;
	bool		 allTypesNull;
	Datum		 argModes;
	Datum		 argNames;
	Datum		 argTypes;

	/* 
	 * Grab argModes, argNames, and isFunc from the tuple
	 *
	 *	NOTE: we *must* use SysCacheGetAttr() for these attributes instead of 
	 *	peeking directly into the Form_pg_proc: these attributes follow the 
	 *	first variable-length attribute in the tuple and the Form_pg_proc 
	 *	typedef is meaningless beyond the first variable-length value.
	 */
	
	argModes = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proargmodes,	   &modesNull );
	argNames = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proargnames,	   &namesNull );
	argTypes = SysCacheGetAttr( PROCNAMEARGSNSP, proctup, Anum_pg_proc_proallargtypes, &allTypesNull );

	info->targetOid  = HeapTupleGetOid( proctup );
	info->schemaOid  = procform->pronamespace;
	info->targetName = pstrdup( NameStr( procform->proname ));
	info->argModes   = modesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argModes );
	info->argNames   = namesNull ? PointerGetDatum( NULL ) : (Datum)DatumGetArrayTypePCopy( argNames );
	info->langOid    = procform->prolang;
	info->isFunc     = TRUE;		/* Required for EDB SPL, please do not remove */
	info->fqName	 = makeFullName( info->schemaOid, InvalidOid, info->targetName );
	info->returnsSet = procform->proretset;
	info->returnType = procform->prorettype;

	if( allTypesNull )
	{
		info->nargs = procform->pronargs;
	}
	else
	{
		ArrayType * argModesArray = DatumGetArrayTypeP( argModes );

		info->nargs = ArrayGetNItems(ARR_NDIM(argModesArray), ARR_DIMS(argModesArray));	
	}

	if( allTypesNull )
	{
		int arg;

		for( arg = 0; arg < info->nargs; ++arg )
			info->argTypes[arg] = procform->proargtypes.values[arg];
	}
	else
	{
		ArrayType * allTypes = DatumGetArrayTypeP( argTypes );
		Oid		  * typeOids = (Oid *)ARR_DATA_PTR( allTypes );
		int			arg;

		for( arg = 0; arg < info->nargs; ++arg )
			info->argTypes[arg] = typeOids[arg];
	}
}

/*******************************************************************************
 * getProcCandidates()
 *
 *	Given a target name (not a complete signature), this function will return a
 *	list of matching pg_proc tuples.
 *
 *	NOTE: we split this code out into a separate function because the PL/pgSQL
 *		  implementation differs from the EDB SPL implementation.
 */

static CatCList * getProcCandidates( targetInfo * info )
{
	return( SearchSysCacheList( PROCNAMEARGSNSP, 1, CStringGetDatum( info->funcName ), 0, 0, 0 ));
}
