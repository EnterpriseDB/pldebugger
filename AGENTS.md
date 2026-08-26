# AGENTS.md

## Repository

This repository builds `pldebugger`, a PostgreSQL extension used by clients such
as pgAdmin to debug PL/pgSQL routines.

It delivers the preload library `plugin_debugger` and the SQL extension
`pldbgapi`. Preserve compatibility with existing clients unless a contract
change is explicitly requested and documented.

Useful references:

- `README-pldebugger.md` and `INSTALL.md`: installation and runtime usage;
- `docker/README.md`: container usage and publication;
- `Makefile`: PGXS build and installed artifacts;
- `.github/workflows/ci.yaml`: continuous validation;
- `.github/workflows/docker-publish.yaml`: Docker Hub publication.

Prefer current repository behavior and PostgreSQL APIs over assumptions from old
documentation or other forks.

## Compatibility and operational gotchas

CI supports PostgreSQL 13 through 18. Shared code must follow the existing
`PG_VERSION_NUM` compatibility style.

Treat these as compatibility contracts:

- public SQL function names, arguments, result columns, order, and types;
- client-visible variable names, values, type metadata, and text formats;
- proxy-to-target protocol messages and API versions;
- legacy behavior used by existing debugger consumers.

Prefer an additive API for richer output instead of changing a legacy function
in place.

Review sensitive changes for:

- backend lifecycle, shared-memory ownership, PID reuse, and slot cleanup;
- proxy-to-target sockets and concurrent debugger sessions;
- PostgreSQL memory contexts and allocation APIs;
- datum nullability, type OIDs, typmods, and collation;
- anonymous `RECORD` values without a stable composite type OID.

Building alone does not enable debugging. `plugin_debugger` must be in
`shared_preload_libraries`, followed by a PostgreSQL restart. Install the SQL API
separately in each database:

```sql
CREATE EXTENSION pldbgapi;
```

Extension creation validates packaging, not a complete debugger session.
Changes to stepping, breakpoints, variables, cleanup, or concurrency require a
targeted behavioral test.

## Working and build workflow

- Inspect `git status` before changing files.
- Keep changes focused and preserve unrelated local work.
- Avoid broad formatting alongside functional changes.
- Never suppress a warning or failure merely to make CI green.
- Add focused validation for changed behavior.

The normal PGXS build requires PostgreSQL server development headers:

```sh
pg_config --version
pg_config --pgxs
make clean
make USE_PGXS=1
```

Installation targets the PostgreSQL selected by `pg_config` and may require
elevated permissions:

```sh
make USE_PGXS=1 install
```

Do not install into a user database or system PostgreSQL instance unless the
task explicitly requires it.

`spl_debugger.c` is generated when package support is enabled. Modify
`plpgsql_debugger.c`, not the generated copy.

## Docker

Build from the repository root so the image contains the exact checkout:

```sh
docker build \
  --file docker/Dockerfile \
  --build-arg TAG=18 \
  --build-arg BASE_IMAGE=18-bookworm \
  --tag pldebugger-ci:18 \
  .
```

The runtime image receives only installed artifacts from the builder stage. The
initialization script must use `$PGDATA`; do not hard-code a data directory.

For packaging changes, test the oldest and newest supported PostgreSQL versions.
Also test `trixie` when publication behavior changes.

## CI and publication

`CI` runs for pull requests, pushes to `master` and `print-vars`, and manual
dispatches. PostgreSQL 13–18 jobs build the exact commit, start the `bookworm`
image, create `pldbgapi`, verify preloading, and query the proxy API.

The matrix uses `fail-fast: false` so one transient failure does not hide other
results.

`Publish Docker images` is separate. It pushes multi-architecture `bookworm` and
`trixie` images to Docker Hub. Do not dispatch it as a CI test because it changes
external artifacts.

The Makefile has no comprehensive regression suite. A green container smoke test
is necessary but insufficient for sensitive debugger semantics.

Before handoff:

- run `git diff --check`;
- build against the relevant PostgreSQL headers;
- run targeted SQL or debugger checks;
- run `actionlint` for changed workflows;
- inspect every remote matrix job with `gh`;
- report anything not tested.

## Git hygiene

- Ignore IDE state, build products, logs, and local environment files.
- Stage only files belonging to the requested change.
- Keep commits focused on observable behavior.
- Do not rewrite published history without explicit approval.
- Do not push commits or publish images unless authorized.
- Leave the tracked worktree clean after committed work.
