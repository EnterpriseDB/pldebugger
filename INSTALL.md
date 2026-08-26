# Install with docker container

Tested with succes with PG 13 & 14
Replace X with your version

## Run docker container
> docker run --name postgres-14-idea-debugger -p 5442:5432 -e POSTGRES_PASSWORD=postgres -d postgres:1X

## Install Postgres and debugger sources

> apt update && apt upgrade  

> apt install git build-essential libreadline-dev zlib1g-dev bison libkrb5-dev flex postgresql-server-dev-1X

> cd /usr/src/

> git clone https://github.com/postgres/postgres.git

> cd postgres  

> git checkout REL_1X_STABLE

> ./configure

> cd contrib

> git clone https://github.com/ng-galien/pldebugger.git

> cd pldebugger/

> git checkout print-vars

## Install debugger

> make clean && make USE_PGXS=1 && make USE_PGXS=1 install

