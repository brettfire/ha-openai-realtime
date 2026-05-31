# Use Home Assistant's Debian base image. We need Debian (not the
# default Alpine `-base` variants) because both llvmlite==0.47.0 and
# numba==0.65.1 — transitive deps of pipecat-ai via onnxruntime /
# resampy — ship manylinux glibc wheels but not musl. On the older
# Alpine setup we had to install LLVM 15 from Alpine 3.17's repo and
# compile llvmlite from source with a special `--no-build-isolation`
# dance. Switching to Debian lets pip use the prebuilt wheels
# directly, cutting build time from ~10 min to ~90 s and eliminating
# all that build-deps complexity.
ARG BUILD_FROM=ghcr.io/home-assistant/aarch64-base-debian:trixie

FROM ${BUILD_FROM}

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV LANG=C.UTF-8 \
    DEBIAN_FRONTEND=noninteractive \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    PIP_BREAK_SYSTEM_PACKAGES=1

# Runtime deps:
#   - python3 / python3-pip: Trixie ships Python 3.13, matching the
#     cp313 wheel tags pipecat 1.3.0's dep chain provides.
#   - ffmpeg / libsndfile1: pipecat audio processing fallbacks.
#   - ca-certificates: TLS for OpenAI + Home Assistant API calls.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        python3 \
        python3-pip \
        ca-certificates \
        ffmpeg \
        libsndfile1 \
    && rm -rf /var/lib/apt/lists/*

# Install the locked Python dependency set. requirements-frozen.txt is
# refreshed against the pipecat-ai version pinned in pyproject.toml;
# regenerate via `pip freeze > requirements-frozen.txt` from a fresh
# venv after bumping pipecat-ai.
COPY requirements-frozen.txt /tmp/requirements-frozen.txt
# --ignore-installed: Debian's apt-installed Python packages
# (e.g. `packaging`) have no RECORD file, so pip can't uninstall
# them when our freeze pins a different version. Since we own the
# whole container's Python environment, just overlay our pins on
# top instead of trying to uninstall first.
RUN pip3 install --ignore-installed -r /tmp/requirements-frozen.txt

# Copy application code and HA addon root (run.sh, etc.).
COPY app/ /app/
COPY root/ /
RUN chmod a+x /run.sh

CMD ["/run.sh"]
