# Docker Image

## Build the image

Leave empty if you don't want to push the image to docker hub

### Build the image for a specific platform

> change PG_PLATFORM to amd64 or arm64

```bash
export PG_PLATFORM=arm64 && PG_VERSION=15 && export PG_IMAGE=postgres-debugger && export DOCKER_USER=galien0xffffff/ \
&& docker build --platform "linux/$PG_PLATFORM" --build-arg "TAG=$PG_VERSION" -t "$DOCKER_USER$PG_IMAGE:$PG_VERSION-$PG_PLATFORM" .
```

### Push the image to docker hub

```bash
export PG_VERSION=15 && export PG_IMAGE=postgres-debugger && export DOCKER_USER=galien0xffffff/ \
&& docker push "$DOCKER_USER$PG_IMAGE:$PG_VERSION-amd64" \
&& docker push "$DOCKER_USER$PG_IMAGE:$PG_VERSION-arm64"
```

## Manifest

### Create the manifest

```bash
export PG_VERSION=15 && export PG_IMAGE=postgres-debugger && export DOCKER_USER=galien0xffffff/ \
&& docker manifest create "$DOCKER_USER$PG_IMAGE:$PG_VERSION" \
--amend "$DOCKER_USER$PG_IMAGE:$PG_VERSION-amd64" \
--amend "$DOCKER_USER$PG_IMAGE:$PG_VERSION-arm64"
```

### Push the manifest

```bash
export PG_VERSION=15 && export PG_IMAGE=postgres-debugger && export DOCKER_USER=galien0xffffff/ \
&& docker manifest push "$DOCKER_USER$PG_IMAGE:$PG_VERSION"
```


## Run the image

```bash
export PG_VERSION=14 \
&& export PG_IMAGE=postgres-with-debugger \
&& docker run -p 55$PG_VERSION:5432 --name "PostgresSQL-$PG_VERSION-debug" -e POSTGRES_PASSWORD=postgres -d "$PG_IMAGE:$PG_VERSION"
```
