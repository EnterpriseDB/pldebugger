--
-- This script uninstalls the PL/PGSQL debugger API.
--
-- Note: this isn't needed on 9.1 and above, as the functions and types are
-- packaged in an extension. You can just drop the extension with
-- DROP EXTENSION command. This is still needed to uninstall on older
-- versions, however.
--

DROP FUNCTION pldbg_get_target_info(TEXT, "char");
DROP FUNCTION pldbg_wait_for_target(INTEGER);
DROP FUNCTION pldbg_wait_for_breakpoint(INTEGER);
DROP FUNCTION pldbg_step_over(INTEGER);
DROP FUNCTION pldbg_step_into(INTEGER);
DROP FUNCTION pldbg_set_global_breakpoint(INTEGER, OID, INTEGER, INTEGER);
DROP FUNCTION pldbg_set_breakpoint(INTEGER, OID, INTEGER);
DROP FUNCTION pldbg_select_frame(INTEGER, INTEGER);
DROP FUNCTION pldbg_get_variables(INTEGER);
DROP FUNCTION pldbg_get_proxy_info();
DROP FUNCTION pldbg_get_stack(INTEGER);
DROP FUNCTION pldbg_get_source(INTEGER, OID);
DROP FUNCTION pldbg_get_breakpoints(INTEGER);
DROP FUNCTION pldbg_drop_breakpoint(INTEGER, OID, INTEGER);
DROP FUNCTION pldbg_deposit_value(INTEGER, TEXT, INTEGER, TEXT);
DROP FUNCTION pldbg_create_listener();
DROP FUNCTION pldbg_continue(INTEGER);
DROP FUNCTION pldbg_attach_to_port(INTEGER);
DROP FUNCTION pldbg_abort_target(INTEGER);
DROP FUNCTION pldbg_oid_debug(OID);
DROP FUNCTION plpgsql_oid_debug(OID);

DROP TYPE proxyInfo;
DROP TYPE var;
DROP TYPE targetinfo;
DROP TYPE frame;
DROP TYPE breakpoint;
