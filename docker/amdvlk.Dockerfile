#
# Dockerfile for LLPC Continuous Integration.
# Contains the base image used for incremental builds of llpc.
# Sample invocation:
#    docker build . --file docker/amdvlk.Dockerfile               \
#                   --build-arg CONFIG=Release                    \
#                   --build-arg BRANCH=dev                        \
#                   --build-arg GENERATOR=Ninja                   \
#                   --tag kuhar/amdvlk:nightly
#
# Required arguments:
# - CONFIG: Debug or Release
# - BRANCH: The base AMDVLK branch to use (e.g., master, dev, releases/<name>)
# - GENERATOR: CMake generator to use (e.g., "Unix Makefiles", Ninja)
#

FROM ubuntu:18.04

ARG BRANCH
ARG CONFIG
ARG GENERATOR

# Install required packages.
RUN apt-get update \
    && apt-get install -yqq --no-install-recommends \
    	build-essential cmake gcc g++ ninja-build binutils-gold \
    	python python-distutils-extra python3 python3-distutils \
    	libssl-dev libx11-dev libxcb1-dev x11proto-dri2-dev libxcb-dri3-dev \
    	libxcb-dri2-0-dev libxcb-present-dev libxshmfence-dev libxrandr-dev \
    	git repo curl vim-tiny \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/ld ld /usr/bin/ld.gold 10

# Checkout all repositories. Replace llpc with the version in LLPC_SOURCE_DIR.
WORKDIR /vulkandriver
RUN repo init -u https://github.com/GPUOpen-Drivers/AMDVLK.git -b "$BRANCH" \
    && repo sync -c --no-clone-bundle -j$(nproc) \
    && cd /vulkandriver/drivers/spvgen/external \
    && python2 fetch_external_sources.py

# Build LLPC.
WORKDIR /vulkandriver/builds/ci-build
RUN cmake "/vulkandriver/drivers/xgl" \
          -G "$GENERATOR" \
          -DXGL_BUILD_LIT=ON \
          -DCMAKE_BUILD_TYPE="$CONFIG" \
    && cmake --build . \
    && cmake --build . --target amdllpc \
    && cmake --build . --target spvgen \
    && cmake --build . --target FileCheck \
    && cmake --build . --target count \
    && cmake --build . --target not

# Run the lit test suite.
RUN cmake --build . --target check-amdllpc -- -v \
    && (echo "Base image built on $(date)" | tee /vulkandriver/build_info.txt)
