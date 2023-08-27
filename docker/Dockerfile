# Filename: Dockerfile

#POSTGRES IMAGE
ARG TAG=latest
FROM postgres:${TAG}

# SET ENV
ARG TAG
ENV PG_LIB=postgresql-server-dev-${TAG}
ENV PG_BRANCH=REL_${TAG}_STABLE
ENV PLUGIN_BRANCH=print-vars

# APT
RUN apt --yes update \
    && apt --yes upgrade \
    && apt --yes install git build-essential libreadline-dev zlib1g-dev bison libkrb5-dev flex $PG_LIB \
    && cd /usr/src/ \
    && git clone --progress -b $PG_BRANCH --single-branch https://github.com/postgres/postgres.git \
    && cd postgres \
    && ./configure \
    && cd /usr/src/postgres/contrib \
    && git clone -b $PLUGIN_BRANCH --single-branch https://github.com/ng-galien/pldebugger.git \
    && cd pldebugger \
    && make clean && make USE_PGXS=1 && make USE_PGXS=1 install \
    && rm -r /usr/src/postgres \
    && apt --yes remove --purge git build-essential libreadline-dev zlib1g-dev bison libkrb5-dev flex $PG_LIB \
    && apt --yes autoremove  \
    && apt --yes clean

# CONFIG
COPY *.sql /docker-entrypoint-initdb.d/
COPY *.sh /docker-entrypoint-initdb.d/
RUN chmod a+r /docker-entrypoint-initdb.d/*
