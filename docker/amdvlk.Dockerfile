#
# Dockerfile for LLPC Continuous Integration.
# Contains the base image used for incremental builds of llpc.
# Sample invocation:
#    docker build . --file docker/amdvlk.Dockerfile               \
#                   --build-arg BRANCH=dev                        \
#                   --build-arg CONFIG=Release                    \
#                   --build-arg ASSERTIONS=ON                     \
#                   --build-arg FEATURES="+clang+sanitizers"      \
#                   --build-arg GENERATOR=Ninja                   \
#                   --tag kuhar/amdvlk:nightly
#
# Required arguments:
# - BRANCH: The base AMDVLK branch to use (e.g., master, dev, releases/<name>)
# - CONFIG: Debug or Release
# - ASSERTIONS: OFF or ON
# - FEATURES: A '+'-spearated set of features to enable
# - GENERATOR: CMake generator to use (e.g., "Unix Makefiles", Ninja)
#

FROM ubuntu:18.04

ARG BRANCH
ARG CONFIG
ARG ASSERTIONS
ARG FEATURES
ARG GENERATOR

# Install required packages.
RUN apt-get update \
    && apt-get install -yqq --no-install-recommends \
       build-essential pkg-config cmake cmake-data \
       gcc g++ ninja-build binutils-gold \
       clang-9 libclang-common-9-dev lld-9 \
       python python-distutils-extra python3 python3-distutils \
       libssl-dev libx11-dev libxcb1-dev x11proto-dri2-dev libxcb-dri3-dev \
       libxcb-dri2-0-dev libxcb-present-dev libxshmfence-dev libxrandr-dev \
       libwayland-dev \
       git repo curl vim-tiny \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/ld ld /usr/bin/ld.gold 10 \
    && update-alternatives --install /usr/bin/lld lld /usr/bin/lld-9 10 \
    && update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-9 10

# Checkout all repositories. Replace llpc with the version in LLPC_SOURCE_DIR.
WORKDIR /vulkandriver
RUN repo init -u https://github.com/GPUOpen-Drivers/AMDVLK.git -b "$BRANCH" \
    && repo sync -c --no-clone-bundle -j$(nproc) \
    && cd /vulkandriver/drivers/spvgen/external \
    && python2 fetch_external_sources.py

# Build LLPC.
WORKDIR /vulkandriver/builds/ci-build
RUN EXTRA_FLAGS="" \
    && if echo "$FEATURES" | grep -q "+gcc" ; then \
         EXTRA_FLAGS="$EXTRA_FLAGS -DCMAKE_C_COMPILER=gcc"; \
         EXTRA_FLAGS="$EXTRA_FLAGS -DCMAKE_CXX_COMPILER=g++"; \
       fi \
    && if echo "$FEATURES" | grep -q "+clang" ; then \
         EXTRA_FLAGS="$EXTRA_FLAGS -DCMAKE_C_COMPILER=clang-9"; \
         EXTRA_FLAGS="$EXTRA_FLAGS -DCMAKE_CXX_COMPILER=clang++-9"; \
         EXTRA_FLAGS="$EXTRA_FLAGS -DLLVM_USE_LINKER=lld"; \
         EXTRA_FLAGS="$EXTRA_FLAGS -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"; \
         EXTRA_FLAGS="$EXTRA_FLAGS -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"; \
       fi \
    && if echo "$FEATURES" | grep -q "+shadercache" ; then \
         EXTRA_FLAGS="$EXTRA_FLAGS -DLLPC_ENABLE_SHADER_CACHE=1"; \
       fi \
    && echo "Extra CMake flags: $EXTRA_FLAGS" \
    && cmake "/vulkandriver/drivers/xgl" \
          -G "$GENERATOR" \
          -DCMAKE_BUILD_TYPE="$CONFIG" \
          -DXGL_BUILD_LIT=ON \
          -DXGL_ENABLE_ASSERTIONS="$ASSERTIONS" \
          -DICD_ANALYSIS_WARNINGS_AS_ERRORS=ON \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          $EXTRA_FLAGS \
    && cmake --build . \
    && cmake --build . --target amdllpc \
    && cmake --build . --target spvgen \
    && cmake --build . --target FileCheck \
    && cmake --build . --target count \
    && cmake --build . --target not

# Run the lit test suite.
RUN cmake --build . --target check-amdllpc -- -v \
    && (echo "Base image built on $(date)" | tee /vulkandriver/build_info.txt)
