#
# Dockerfile for LLPC Continuous Integration.
# Sample invocation:
#    docker build .                                                                                       \
#      --file docker/llpc.Dockerfile                                                                      \
#      --build-arg LLPC_REPO_NAME=GPUOpen-Drivers/llpc                                                    \
#      --build-arg LLPC_REPO_REF=<GIT_REF>                                                                \
#      --build-arg LLPC_REPO_SHA=<GIT_SHA>                                                                \
#      --build-arg FEATURES="+coverage"                                                                   \
#      --tag llpc-ci/llpc
#
# Required arguments:
# - AMDVLK_IMAGE: Base image name for prebuilt amdvlk
# - LLPC_REPO_NAME: Name of the llpc repository to clone
# - LLPC_REPO_REF: ref name to checkout
# - LLPC_REPO_SHA: SHA of the commit to checkout
# - FEATURES: A '+'-separated set of features to enable such as code coverage ('+coverage')
#

FROM ubuntu:20.04

ARG LLPC_REPO_NAME
ARG LLPC_REPO_REF
ARG LLPC_REPO_SHA
ARG FEATURES

# Use bash instead of sh in this docker file.
SHELL ["/bin/bash", "-c"]

# Install required packages.
# Use pip to install an up-to-date version of CMake. The apt package is
# too old for LLVM.
RUN export DEBIAN_FRONTEND=noninteractive && export TZ=America/New_York \
    && apt-get update \
    && apt-get install -yqq --no-install-recommends \
       build-essential pkg-config ninja-build \
       gcc g++ binutils-gold \
       llvm-11 clang-11 clang-tidy-12 libclang-common-11-dev lld-11 \
       python python3 python3-distutils python3-pip \
       libssl-dev libx11-dev libxcb1-dev x11proto-dri2-dev libxcb-dri3-dev \
       libxcb-dri2-0-dev libxcb-present-dev libxshmfence-dev libxrandr-dev \
       libwayland-dev \
       git curl wget openssh-client \
       gpg gpg-agent \
    && rm -rf /var/lib/apt/lists/* \
    && python3 -m pip install --no-cache-dir --upgrade pip \
    && python3 -m pip install --no-cache-dir --upgrade cmake \
    && for tool in clang clang++ llvm-cov llvm-profdata llvm-symbolizer lld ld.lld ; do \
         update-alternatives --install /usr/bin/"$tool" "$tool" /usr/bin/"$tool"-11 10 ; \
        done \
    && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-12 10 \
    && update-alternatives --install /usr/bin/ld ld /usr/bin/ld.gold 10

# Checkout llpc
WORKDIR /vulkandriver
RUN git clone https://github.com/${LLPC_REPO_NAME}.git . \
    && git fetch origin +${LLPC_REPO_SHA}:${LLPC_REPO_REF} --update-head-ok \
    && git checkout ${LLPC_REPO_SHA} \
    && touch ./env.sh

# Copy helper scripts into container.
COPY docker/*.sh /vulkandriver/

# Build LLPC.
WORKDIR /vulkandriver/builds/ci-build
RUN EXTRA_COMPILER_FLAGS=() \
    && EXTRA_LINKER_FLAGS=() \
    && EXTRA_FLAGS=("-DXGL_BUILD_CACHE_CREATOR=ON") \
    && SANITIZERS=() \
    && if echo "$FEATURES" | grep -q "+gcc" ; then \
         EXTRA_FLAGS+=("-DCMAKE_C_COMPILER=gcc"); \
         EXTRA_FLAGS+=("-DCMAKE_CXX_COMPILER=g++"); \
       fi \
    && if echo "$FEATURES" | grep -q "+clang" ; then \
         EXTRA_FLAGS+=("-DCMAKE_C_COMPILER=clang"); \
         EXTRA_FLAGS+=("-DCMAKE_CXX_COMPILER=clang++"); \
         EXTRA_FLAGS+=("-DLLVM_USE_LINKER=lld"); \
         EXTRA_LINKER_FLAGS+=("-fuse-ld=lld"); \
       fi \
    && if echo "$FEATURES" | grep -q "+shadercache" ; then \
         EXTRA_FLAGS+=("-DLLPC_ENABLE_SHADER_CACHE=1"); \
       fi \
    && if echo "$FEATURES" | grep -q "+asan" ; then \
         SANITIZERS+=("Address"); \
         echo "export ASAN_OPTIONS=detect_leaks=0" >> /vulkandriver/env.sh; \
         echo "export LD_PRELOAD=$(clang -print-file-name=libclang_rt.asan-x86_64.so)" >> /vulkandriver/env.sh; \
       fi \
    && if echo "$FEATURES" | grep -q "+ubsan" ; then \
         SANITIZERS+=("Undefined"); \
       fi \
    && if echo "$FEATURES" | grep -q "+tsan" ; then \
         SANITIZERS+=("Thread"); \
       fi \
    && if [ ${#SANITIZERS[@]} -ne 0 ]; then  \
         SANITIZERS_SEPARATED_LIST="${SANITIZERS[@]}"; \
         SANITIZERS_SEPARATED_LIST="${SANITIZERS_SEPARATED_LIST// /;}"; \
         EXTRA_FLAGS+=("-DXGL_USE_SANITIZER='${SANITIZERS_SEPARATED_LIST}'"); \
       fi \
    && if echo "$FEATURES" | grep -q "+coverage" ; then \
         EXTRA_COMPILER_FLAGS+=("-fprofile-instr-generate=/vulkandriver/profile%2m.profraw" "-fcoverage-mapping"); \
         EXTRA_LINKER_FLAGS+=("-fprofile-instr-generate=/vulkandriver/profile%2m.profraw" "-fcoverage-mapping"); \
       fi \
    && if echo "$FEATURES" | grep -q "+assertions" ; then \
         EXTRA_FLAGS+=("-DXGL_ENABLE_ASSERTIONS=ON"); \
       fi \
    && if [ ${#EXTRA_COMPILER_FLAGS[@]} -ne 0 ]; then \
         EXTRA_FLAGS+=("-DCMAKE_C_FLAGS='${EXTRA_COMPILER_FLAGS[*]}'" "-DCMAKE_CXX_FLAGS='${EXTRA_COMPILER_FLAGS[*]}'"); \
       fi \
    && if [ ${#EXTRA_LINKER_FLAGS[@]} -ne 0 ]; then \
         EXTRA_FLAGS+=("-DCMAKE_EXE_LINKER_FLAGS='${EXTRA_LINKER_FLAGS[*]}'" "-DCMAKE_SHARED_LINKER_FLAGS='${EXTRA_LINKER_FLAGS[*]}'"); \
       fi \
    && echo "Extra CMake flags: ${EXTRA_FLAGS[@]}" \
    && echo "Extra env vars (/vulkandriver/env.sh): " \
    && cat /vulkandriver/env.sh \
    && source /vulkandriver/env.sh \
    && cmake "/vulkandriver/llpc" \
          -G Ninja \
          -DCMAKE_BUILD_TYPE="$CONFIG" \
          "${EXTRA_FLAGS[@]}" \
    && cmake --build . --target check-amdllpc check-amdllpc-units -- -v \
    && cmake --build . --target check-lgc check-lgc-units -- -v

# Generate code coverage report for LLPC.
RUN if echo "$FEATURES" | grep -q "+coverage" ; then \
      /vulkandriver/generate-coverage-report.sh; \
    fi
