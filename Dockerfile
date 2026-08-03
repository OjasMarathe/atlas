# ---- build stage: vcpkg + cmake compile ----
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      git curl zip unzip tar ca-certificates jq \
      build-essential cmake ninja-build pkg-config \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
WORKDIR /src

# Copy the manifest first: it pins the vcpkg baseline used just below, and keeping it ahead of
# the source COPYs lets the (slow) dependency layer cache across code-only changes.
COPY vcpkg.json ./

# Full clone, then check out the exact commit pinned in vcpkg.json's "builtin-baseline".
# A --depth 1 clone of vcpkg's tip does NOT contain the baseline commit, and Configure then
# fails to resolve dependencies — the same trap that broke the first CI run.
RUN git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout "$(jq -r '."builtin-baseline"' vcpkg.json)" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

COPY CMakeLists.txt CMakePresets.json ./
COPY proto ./proto
COPY src ./src
# tests/ and tools/ are required even though only two targets are built: CMakeLists declares
# every target, and CMake validates each declared source path at configure time.
COPY tests ./tests
COPY tools ./tools

RUN cmake --preset default && cmake --build build --target atlas_node atlas_cli

# ---- runtime stage: just the binary ----
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/atlas_node /usr/local/bin/atlas_node
# The CLI ships too, so `docker compose exec metadata atlas nodes` works against a live cluster.
COPY --from=build /src/build/atlas /usr/local/bin/atlas
ENTRYPOINT ["/usr/local/bin/atlas_node"]
