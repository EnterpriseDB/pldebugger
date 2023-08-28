# Docker Image

## Build the image

Leave empty if you don't want to push the image to docker hub

### Environment variables


* PG_VERSION: PostgreSQL version (from 11 to 15)
* PG_PLATFORM: Target platform, tested on linux/amd64 and linux/arm64 (arm64 is supported from PostgreSQL 14)
* PG_IMAGE: Image name (11.21-bullseye for postgres:11.21-bullseye)


```bash
export PG_PLATFORM=linux/amd64 \
&& PG_VERSION=11 \
&& export PG_IMAGE=postgres-debugger \
&& export DOCKER_USER=galien0xffffff \
&& export BASE_IMAGE="11-bookworm" \
```

```bash
docker buildx build --platform $PG_PLATFORM --build-arg "TAG=$PG_VERSION" --build-arg "BASE_IMAGE=$BASE_IMAGE" -t "$DOCKER_USER/$PG_IMAGE:$PG_VERSION" .
```
