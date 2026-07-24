# ---- build stage: vcpkg + cmake compile ----
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      git curl zip unzip tar ca-certificates \
      build-essential cmake ninja-build pkg-config \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src
# Copy the manifest first so the (slow) dependency build layer caches across code changes.
COPY vcpkg.json CMakeLists.txt CMakePresets.json ./
COPY proto ./proto
COPY src ./src
COPY tests ./tests

RUN cmake --preset default && cmake --build build --target atlas_node

# ---- runtime stage: just the binary ----
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/atlas_node /usr/local/bin/atlas_node
ENTRYPOINT ["/usr/local/bin/atlas_node"]
