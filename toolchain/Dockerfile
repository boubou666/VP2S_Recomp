# VP2S_Recomp toolchain: ISO extraction, MIPS/R5900 binutils, Python analysis,
# and a C++20 build environment (CMake, Ninja, g++) for the recompiler.
# Build:  docker build -t vp2s-toolchain toolchain
# Run:    docker run --rm -it -v D:\VP2S_Recomp:/work vp2s-toolchain
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        7zip \
        binutils-mips-linux-gnu \
        python3 \
        python3-pip \
        python3-venv \
        file \
        xxd \
        git \
        ca-certificates \
        cmake \
        ninja-build \
        g++ \
        pkg-config \
        libx11-dev libxcursor-dev libxrandr-dev libxinerama-dev libxi-dev \
        libgl1-mesa-dev libasound2-dev \
    && rm -rf /var/lib/apt/lists/*

# Python libs for ELF parsing and analysis scripts
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir pyelftools capstone
ENV PATH="/opt/venv/bin:${PATH}"

WORKDIR /work
