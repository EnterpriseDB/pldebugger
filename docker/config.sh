#!/bin/bash
set -e

printf "\nshared_preload_libraries = 'plugin_debugger'\n" \
  >> "$PGDATA/postgresql.conf"
