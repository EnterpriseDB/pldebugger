# Install with docker container

## Run docker container
> docker run --name postgres-14-idea-debugger -p 5442:5432 -e POSTGRES_PASSWORD=postgres -d postgres:14

## Install Postgres sources

> cd /usr/src/
> apt update
> apt upgrade
> apt install git
> git clone https://github.com/postgres/postgres.git
> apt install build-essential libreadline-dev zlib1g-dev bison
> cd postgres
> ./configure

## Install pldebugger sources
> cd postgres/contrib/pldebugger/
> git clone https://github.com/ng-galien/pldebugger.git
> cd /usr/src/postgres/contrib/pldebugger/

# Compile
> make

# Install
> make USER_PGXS=1 install
