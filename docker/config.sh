#!/bin/bash

sed -i "s/#shared_preload_libraries = ''/shared_preload_libraries = 'plugin_debugger'/g" /var/lib/postgresql/data/postgresql.conf