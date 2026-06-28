# Spring Boot demo — multi-stage build.
#
#   build stage: gradle 9 + JDK 21 → bootJar
#   run stage:   slim JRE 17 + the jar

# -------- build --------
FROM gradle:9.0-jdk21 AS build

WORKDIR /src
COPY settings.gradle.kts build.gradle.kts ./
COPY src ./src

RUN gradle --no-daemon bootJar

# -------- run --------
FROM eclipse-temurin:17-jre-jammy

RUN useradd -r -u 1001 -g root app

COPY --from=build /src/build/libs/*.jar /app/app.jar

USER app
WORKDIR /app
EXPOSE 8081

# `helix.daemon-url` points at the daemon's compose service name.
ENV HELIX_DAEMON_URL=http://helix-daemon:9099 \
    SERVER_PORT=8081

ENTRYPOINT ["sh", "-c", "exec java -jar /app/app.jar --helix.daemon-url=$HELIX_DAEMON_URL --server.port=$SERVER_PORT"]
