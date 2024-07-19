################################################################################
##
## This Makefile builds plugin_debugger.so. It consists of a PL/pgSQL
## interpreter plugin, and a set of functions that form an SQL interface
## to the PL/pgSQL debugger.
##
## Copyright (c) 2004-2024 EnterpriseDB Corporation. All Rights Reserved.
##
## Licensed under the Artistic License v2.0, see 
##		https://opensource.org/licenses/artistic-license-2.0
## for full details

EXTENSION  = pldbgapi
MODULE_big = plugin_debugger

OBJS	   = plpgsql_debugger.o plugin_debugger.o dbgcomm.o pldbgapi.o
ifdef INCLUDE_PACKAGE_SUPPORT
OBJS += spl_debugger.o
endif
DATA       = pldbgapi--1.1.sql pldbgapi--unpackaged--1.1.sql pldbgapi--1.0--1.1.sql
DOCS	   = README-pldebugger.md

# PGXS build needs PostgreSQL 9.2 or later. Earlier versions didn't install
# plpgsql.h, so you needed the full source tree to access it.
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir       = contrib/pldebugger
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# plpgsql_debugger.c needs plpgsql.h. Beginning with server version 9.2,
# it is installed into include/server, but when building without pgxs,
# with the pldebugger directory being directly in the server source tree's
# contrib directory, we need to tell the compiler where to find it.
plpgsql_debugger.o: CFLAGS += -I$(top_builddir)/src/pl/plpgsql/src
plpgsql_debugger.bc: CPPFLAGS += -I$(top_srcdir)/src/pl/plpgsql/src -I$(top_builddir)/src/pl/plpgsql/src

################################################################################
## If we're building against EnterpriseDB's Advanced Server, also build a
## debugger module for the SPL language. It's pretty much the same as the
## PL/pgSQL one, but the structs have some extra fields and are thus not
## binary-compatible. We make a copy of the .c file, and pass the
## INCLUDE_PACKAGE_SUPPORT=1 flag to compile it against SPL instead of PL/pgSQL.
##
## To make debugging the debugger itself simpler, all the functions are
## mechanically renamed from plpgsql_* to spl_*.
##
## To enable this, you need to run make as "make INCLUDE_PACKAGE_SUPPORT=1"
## 
ifdef INCLUDE_PACKAGE_SUPPORT
spl_debugger.c:  plpgsql_debugger.c
	sed -e 's/plpgsql_/spl_/g' $(module_srcdir)plpgsql_debugger.c > spl_debugger.c

spl_debugger.o: 	CFLAGS += -DINCLUDE_PACKAGE_SUPPORT=1 -I$(top_builddir)/src/pl/edb-spl/src
spl_debugger.bc:	CPPFLAGS += -I$(top_srcdir)/src/pl/plpgsql/src -I$(top_builddir)/src/pl/plpgsql/src

# There's some tiny differences in plugin_debugger.c, if we're including SPL
# language. Pass the INCLUDE_PACKAGE_SUPPORT flag to plugin_debugger.c too.
plugin_debugger.o: CFLAGS += -DINCLUDE_PACKAGE_SUPPORT=1
endif
