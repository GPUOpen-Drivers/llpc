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

FROM ubuntu:20.04

ARG BRANCH
ARG CONFIG
ARG ASSERTIONS
ARG FEATURES
ARG GENERATOR

# Use bash instead of sh in this docker file.
SHELL ["/bin/bash", "-c"]

# Install required packages.
# Use pip to install an up-to-date version of CMake. The apt package is
# too old for LLVM.
RUN export DEBIAN_FRONTEND=noninteractive && export TZ=America/New_York \
    && apt-get update \
    && TOOLCHAIN_PACKAGES="gcc g++ binutils-gold" \
    && if echo "$FEATURES" | grep -q "+clang" ; then \
         TOOLCHAIN_PACKAGES="clang-9 libclang-common-9-dev lld-9 clang-tidy-10"; \
       fi \
    && apt-get install -yqq --no-install-recommends \
       build-essential pkg-config ninja-build \
       $TOOLCHAIN_PACKAGES \
       python python3 python3-distutils python3-pip \
       libssl-dev libx11-dev libxcb1-dev x11proto-dri2-dev libxcb-dri3-dev \
       libxcb-dri2-0-dev libxcb-present-dev libxshmfence-dev libxrandr-dev \
       libwayland-dev \
       git curl wget openssh-client \
    && rm -rf /var/lib/apt/lists/* \
    && python3 -m pip install --no-cache-dir --upgrade pip \
    && python3 -m pip install --no-cache-dir --upgrade cmake \
    && if echo "$FEATURES" | grep -q "+clang" ; then \
         update-alternatives --install /usr/bin/lld lld /usr/bin/lld-9 10 ; \
         update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-9 10 ; \
       else \
        update-alternatives --install /usr/bin/ld ld /usr/bin/ld.gold 10 ; \
      fi

# Checkout all repositories. Replace llpc with the version in LLPC_SOURCE_DIR.
# The /vulkandriver/env.sh file is for extra env variables used by later commands.
WORKDIR /vulkandriver
RUN wget -P /usr/bin/ https://storage.googleapis.com/git-repo-downloads/repo \
    && chmod +x /usr/bin/repo \
    && repo init -u https://github.com/GPUOpen-Drivers/AMDVLK.git -b "$BRANCH" \
    && repo sync -c --no-clone-bundle -j$(nproc) \
    && touch ./env.sh \
    && cd /vulkandriver/drivers/spvgen/external \
    && python3 fetch_external_sources.py

# Copy update script into container
COPY docker/update-llpc.sh /vulkandriver/

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
    && if echo "$FEATURES" | grep -q "+sanitizers" ; then \
         EXTRA_FLAGS="$EXTRA_FLAGS -DXGL_USE_SANITIZER=Address;Undefined"; \
         echo "export ASAN_OPTIONS=detect_leaks=0" >> /vulkandriver/env.sh; \
         echo "export LD_PRELOAD=/usr/lib/llvm-9/lib/clang/9.0.1/lib/linux/libclang_rt.asan-x86_64.so" >> /vulkandriver/env.sh; \
       else \
         EXTRA_FLAGS="$EXTRA_FLAGS -DXGL_BUILD_CACHE_CREATOR=ON"; \
       fi \
    && echo "Extra CMake flags: $EXTRA_FLAGS" \
    && echo "Extra env vars (/vulkandriver/env.sh): " \
    && cat /vulkandriver/env.sh \
    && source /vulkandriver/env.sh \
    && cmake "/vulkandriver/drivers/xgl" \
          -G "$GENERATOR" \
          -DCMAKE_BUILD_TYPE="$CONFIG" \
          -DXGL_BUILD_TESTS=ON \
          -DXGL_ENABLE_ASSERTIONS="$ASSERTIONS" \
          -DICD_ANALYSIS_WARNINGS_AS_ERRORS=OFF \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          $EXTRA_FLAGS \
    && cmake --build . \
    && cmake --build . --target lgc spvgen count FileCheck llvm-objdump not

# Run the lit test suite.
RUN source /vulkandriver/env.sh \
    && cmake --build . --target check-amdllpc check-amdllpc-units -- -v \
    && cmake --build . --target check-lgc check-lgc-units -- -v

# Save build info to /vulkandriver/build_info.txt.
RUN cd /vulkandriver \
    && (printf "Base image built on $(date)\n\n" | tee build_info.txt) \
    && (repo forall -p -c "git log -1 --pretty=format:'%H %s by %an <%ae>'" | tee -a build_info.txt) \
    && (echo | tee -a build_info.txt)
