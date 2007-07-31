-- pldbg.sql
--  This script creates the data types and functions defined by the PL debugger API
--
-- Copyright (c) 2004-2007 EnterpriseDB Corporation. All Rights Reserved.
--
-- Licensed under the Artistic License, see 
--		http://www.opensource.org/licenses/artistic-license.php
-- for full details

CREATE TYPE breakpoint AS ( func OID, linenumber INTEGER, targetName TEXT );
CREATE TYPE frame      AS ( level INT, targetname TEXT, func OID, linenumber INTEGER, args TEXT );

CREATE TYPE targetinfo AS ( target OID, schema OID, nargs INT, argTypes oidvector, targetName NAME, argModes "char"[], argNames TEXT[], targetLang OID, fqName TEXT, returnsSet BOOL, returnType OID );

CREATE TYPE var		   AS ( name TEXT, varClass char, lineNumber INTEGER, isUnique bool, isConst bool, isNotNull bool, dtype OID, value TEXT );
CREATE TYPE proxyInfo  AS ( serverVersionStr TEXT, serverVersionNum INT, proxyAPIVer INT, serverProcessID INT );

CREATE OR REPLACE FUNCTION plpgsql_oid_debug( functionOID OID ) RETURNS INTEGER AS '$libdir/plugins/plugin_debugger' LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION pldbg_abort_target( session INTEGER ) RETURNS SETOF boolean AS  '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_attach_to_port( portNumber INTEGER ) RETURNS INTEGER AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_continue( session INTEGER ) RETURNS breakpoint AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_create_listener() RETURNS INTEGER AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_deposit_value( session INTEGER, varName TEXT, lineNumber INTEGER, value TEXT ) RETURNS boolean AS  '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_drop_breakpoint( session INTEGER, func OID, linenumber INTEGER ) RETURNS boolean AS  '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_get_breakpoints( session INTEGER ) RETURNS SETOF breakpoint AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_get_source( session INTEGER, func OID ) RETURNS TEXT AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_get_stack( session INTEGER ) RETURNS SETOF frame AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_get_proxy_info( ) RETURNS proxyInfo AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_get_variables( session INTEGER ) RETURNS SETOF var AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_select_frame( session INTEGER, frame INTEGER ) RETURNS breakpoint AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_set_breakpoint( session INTEGER, func OID, linenumber INTEGER ) RETURNS boolean AS  '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_set_global_breakpoint( session INTEGER, func OID, linenumber INTEGER, targetPID INTEGER ) RETURNS boolean AS  '$libdir/pldbgapi' LANGUAGE C;
CREATE OR REPLACE FUNCTION pldbg_step_into( session INTEGER ) RETURNS breakpoint AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_step_over( session INTEGER ) RETURNS breakpoint AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_wait_for_breakpoint( session INTEGER ) RETURNS breakpoint  AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_wait_for_target( session INTEGER ) RETURNS INTEGER AS '$libdir/pldbgapi' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pldbg_get_target_info( signature TEXT, targetType "char" ) RETURNS targetInfo AS '$libdir/targetinfo' LANGUAGE C STRICT;


