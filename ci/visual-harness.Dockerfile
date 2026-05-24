# syntax=docker/dockerfile:1.7
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG SKIA_RELEASE_TAG=chrome/m149
ARG SKIA_LINUX_X64_SHA256=53e2bfb5225148311da9bbcb7e65da4479acf774bc3d40b0341530cdc48e97b6
ARG SKIA_LINUX_X64_URL=https://github.com/danielraffel/skia-builder/releases/download/chrome/m149/skia-build-linux-x64-gpu-release.zip
# skia-python intentionally stays one milestone behind — the PyPI bindings
# trail the C++ surface, and this smoke is the optional fallback. The C++
# raster harness is the source of truth for goldens on m149.
ARG SKIA_PYTHON_VERSION=144.0.post2

ENV SKIA_DIR=/opt/pulp/skia-build
ENV PULP_VISUAL_REQUIRE_SKIA=1
ENV PATH=/opt/pulp/visual-venv/bin:${PATH}

RUN --mount=type=cache,id=pulp-visual-apt-cache-amd64,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,id=pulp-visual-apt-lists-amd64,target=/var/lib/apt/lists,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        fontconfig \
        libegl1 \
        libgl1 \
        python3 \
        python3-pip \
        python3-venv \
        unzip

RUN --mount=type=cache,id=pulp-visual-skia-linux-x64,target=/var/cache/pulp/skia,sharing=locked \
    set -eu; \
    mkdir -p /opt/pulp /var/cache/pulp/skia; \
    archive="/var/cache/pulp/skia/skia-linux-x64-${SKIA_LINUX_X64_SHA256}.zip"; \
    if [ -f "${archive}" ] && echo "${SKIA_LINUX_X64_SHA256}  ${archive}" | sha256sum -c -; then \
        echo "Using cached Skia archive ${archive}"; \
    else \
        rm -f "${archive}" "${archive}.tmp"; \
        curl -L --fail --retry 3 --output "${archive}.tmp" "${SKIA_LINUX_X64_URL}"; \
        echo "${SKIA_LINUX_X64_SHA256}  ${archive}.tmp" | sha256sum -c -; \
        mv "${archive}.tmp" "${archive}"; \
    fi; \
    mkdir -p "${SKIA_DIR}"; \
    unzip -q "${archive}" -d "${SKIA_DIR}"

COPY external/fonts/*.ttf /usr/local/share/fonts/pulp/
RUN fc-cache -f

RUN --mount=type=cache,id=pulp-visual-pip-amd64,target=/root/.cache/pip,sharing=locked \
    python3 -m venv /opt/pulp/visual-venv \
    && python -m pip install --upgrade pip \
    && python -m pip install "pytest>=8,<9" "skia-python==${SKIA_PYTHON_VERSION}"

WORKDIR /workspace
CMD ["python", "-m", "pytest", "tools/harness/visual/tests/"]
