# Helix lease daemon — multi-stage build.
#
#   build stage: compile libhelix.a + example_lease_server using musl on Alpine.
#   run stage:   ship just the static binary on a minimal Alpine base.
#
# The final image is ~10 MB.

# -------- build --------
FROM alpine:3.20 AS build

RUN apk add --no-cache build-base linux-headers

WORKDIR /src
COPY Makefile ./
COPY include  ./include
COPY src      ./src
COPY examples ./examples

# The C source links libpthread; -static would require musl-static, which is
# fine on Alpine. We keep a dynamic link so the image stays simple.
RUN make example

# -------- run --------
FROM alpine:3.20

RUN apk add --no-cache libstdc++ libgcc curl \
    && addgroup -S helix && adduser -S -G helix helix

HEALTHCHECK --interval=2s --timeout=1s --start-period=2s --retries=10 \
  CMD curl -fsS http://localhost:9099/v1/health || exit 1

COPY --from=build /src/build/example_lease_server /usr/local/bin/helix-lease-server

USER helix
EXPOSE 9099

# Args: PORT, WORKERS. Override via `command:` in compose.
ENV HELIX_PORT=9099 HELIX_WORKERS=64

ENTRYPOINT ["/bin/sh", "-c", "exec /usr/local/bin/helix-lease-server \"$HELIX_PORT\" \"$HELIX_WORKERS\""]
