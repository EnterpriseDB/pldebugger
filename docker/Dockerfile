ARG BASE_IMAGE=latest
FROM postgres:${BASE_IMAGE} AS builder

ARG TAG
ENV PG_LIB=postgresql-server-dev-${TAG}

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        libkrb5-dev \
        "$PG_LIB" \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/pldebugger
COPY . .

RUN make --jobs="$(nproc)" USE_PGXS=1 \
    && make USE_PGXS=1 install DESTDIR=/opt/pldebugger

FROM postgres:${BASE_IMAGE}

COPY --from=builder /opt/pldebugger/ /
COPY docker/config.sh /docker-entrypoint-initdb.d/10-pldebugger-config.sh

RUN chmod 755 /docker-entrypoint-initdb.d/10-pldebugger-config.sh
