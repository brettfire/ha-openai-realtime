# Use Home Assistant base image (required for Home Assistant addons)
# Format: ghcr.io/home-assistant/{arch}-base:latest
# Override with: --build-arg BUILD_FROM=ghcr.io/home-assistant/aarch64-base:latest
ARG BUILD_FROM=ghcr.io/home-assistant/aarch64-base:latest

# Build stage - contains all build tools
FROM ${BUILD_FROM} AS builder

# Set shell (use sh for Alpine, bash will be installed)
SHELL ["/bin/sh", "-o", "pipefail", "-c"]

# Install build dependencies
# LLVM 15 from Alpine 3.17 is required to compile llvmlite 0.44.0,
# the version pipecat-ai 0.0.97 pins transitively via numba==0.61.2.
RUN \
    apk add --no-cache \
        bash \
        python3 \
        python3-dev \
        py3-pip \
        py3-wheel \
        py3-setuptools \
        curl \
        gcc \
        g++ \
        make \
        cmake \
    && echo "Installing LLVM 15 from Alpine 3.17 repository (required for llvmlite 0.44.0)..." \
    && apk add --no-cache --repository=http://dl-cdn.alpinelinux.org/alpine/v3.17/main \
        llvm15-dev llvm15-static clang15 \
    && ln -sf /usr/lib/llvm15/bin/llvm-config /usr/bin/llvm-config \
    && /usr/bin/llvm-config --version \
    && echo "LLVM 15 installed successfully"

# Switch to bash after installation
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV PIP_BREAK_SYSTEM_PACKAGES=1

# Pre-install older setuptools + wheel into the system Python BEFORE
# building any C-extension wheels. We then build with
# --no-build-isolation so pip reuses these instead of creating a fresh
# build venv (which would download setuptools>=75, removing
# Distribution.spawn(dry_run=...) and breaking llvmlite 0.44.0's
# setup.py with `TypeError: spawn() got an unexpected keyword argument
# 'dry_run'`).
RUN pip3 install --no-cache-dir --upgrade 'setuptools<70' 'wheel<0.43'

# Copy frozen requirements (the exact `pip freeze` output of the last
# successful upstream-built image at ghcr.io/fjfricke/.../v0.1.0). This
# is the single source of truth for runtime deps — no poetry, no
# version drift.
COPY requirements-frozen.txt /tmp/requirements-frozen.txt

# Build llvmlite FIRST, with --no-build-isolation, so it uses the
# pinned setuptools above. After this step llvmlite is installed and
# subsequent pip installs won't rebuild it. numpy is a build dep of
# llvmlite, so pre-install it the same way. Cython is also needed.
RUN LLVM_CONFIG=/usr/bin/llvm-config pip3 install --no-cache-dir --no-build-isolation \
        $(grep -E '^(numpy|Cython|cython)==' /tmp/requirements-frozen.txt) \
    && LLVM_CONFIG=/usr/bin/llvm-config pip3 install --no-cache-dir --no-build-isolation \
        $(grep -E '^llvmlite==' /tmp/requirements-frozen.txt)

# Install the remainder. By this point llvmlite is satisfied so pip
# won't rebuild it. Some downstream packages may compile during this
# step; build isolation is fine for them since they don't share
# llvmlite's setuptools incompatibility.
RUN pip3 install --no-cache-dir -r /tmp/requirements-frozen.txt

# Runtime stage - minimal image without build tools
FROM ${BUILD_FROM}

# Set shell (use sh for Alpine, bash will be installed)
SHELL ["/bin/sh", "-o", "pipefail", "-c"]

# Install only runtime dependencies (bash first for compatibility)
RUN \
    apk add --no-cache \
        bash \
        python3 \
        py3-pip \
        curl \
        ffmpeg \
        alsa-lib \
        alsa-utils

# Switch to bash after installation
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Copy installed Python packages from builder stage
RUN \
    --mount=from=builder,source=/usr/lib,target=/builder-lib \
    --mount=from=builder,source=/usr/local/bin,target=/builder-bin \
    PYTHON_SITE=$(python3 -c "import site; print(site.getsitepackages()[0])") \
    && echo "Target site-packages: $PYTHON_SITE" \
    && find /builder-lib -type d -name "site-packages" | while read src_dir; do \
        echo "Copying from $src_dir to $PYTHON_SITE"; \
        mkdir -p "$PYTHON_SITE"; \
        cp -r "$src_dir"/* "$PYTHON_SITE"/ 2>/dev/null || true; \
    done \
    && if [ -d "/builder-bin" ]; then \
        cp -r /builder-bin/* /usr/local/bin/ 2>/dev/null || true; \
    fi

# Copy application code
COPY app/ /app/

# Copy root filesystem
COPY root/ /
RUN chmod a+x /run.sh

# Set entrypoint
CMD ["/run.sh"]
