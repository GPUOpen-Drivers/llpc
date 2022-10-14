#
# Dockerfile for LLPC Continuous Integration.
# Sample invocation:
#    docker build .                                                                                         \
#      --file docker/llpc-clang-tidy.Dockerfile                                                             \
#      --build-arg LLPC_REPO_NAME=GPUOpen-Drivers/llpc                                                      \
#      --build-arg LLPC_REPO_REF=<GIT_REF>                                                                  \
#      --tag llpc-ci/llpc
#
# Required arguments:
# - AMDVLK_IMAGE: Base image name for prebuilt amdvlk
# - LLPC_REPO_NAME: Name of the llpc repository to clone
# - LLPC_REPO_REF: ref name to checkout
# - LLPC_REPO_SHA: SHA of the commit to checkout
# - LLPC_BASE_REF: ref name for the base of the tested change
#

FROM ubuntu:20.04

ARG LLPC_REPO_NAME
ARG LLPC_REPO_REF
ARG LLPC_REPO_SHA
ARG LLPC_BASE_REF

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
WORKDIR /vulkandriver/drivers/llpc
RUN git clone https://github.com/${LLPC_REPO_NAME}.git . \
    && git fetch origin +${LLPC_REPO_SHA}:${LLPC_REPO_REF} --update-head-ok \
    && git checkout ${LLPC_REPO_SHA} \
    && touch /vulkandriver/env.sh

# Checkout others
WORKDIR /vulkandriver/drivers/
RUN git clone -b amd-master --depth=1 https://github.com/GPUOpen-Drivers/MetroHash.git ./third_party/metrohash \
    && git clone -b amd-master --depth=1 https://github.com/GPUOpen-Drivers/CWPack ./third_party/cwpack \
    && git clone -b amd-gfx-gpuopen-dev --depth=1 https://github.com/GPUOpen-Drivers/llvm-project

# Copy helper scripts into container.
COPY docker/*.sh /vulkandriver/

# Build LLPC.
WORKDIR /vulkandriver/builds/ci-build
RUN echo "Extra env vars (/vulkandriver/env.sh): " \
    && cat /vulkandriver/env.sh \
    && source /vulkandriver/env.sh \
    && cmake "/vulkandriver/drivers/llpc" \
          -G Ninja \
          -DCMAKE_BUILD_TYPE=Release

# Run CMake.
WORKDIR /vulkandriver/builds/ci-build
RUN source /vulkandriver/env.sh \
    && cmake .

# Run clang-tidy. Detect failures by searching for a colon. An empty line or "No relevant changes found." signals success.
WORKDIR /vulkandriver/drivers/llpc
RUN ln -s /vulkandriver/builds/ci-build/compile_commands.json \
    && git diff "origin/$LLPC_BASE_REF" -U0 \
         | /vulkandriver/drivers/llvm-project/clang-tools-extra/clang-tidy/tool/clang-tidy-diff.py \
             -p1 -j$(nproc) -iregex '.*\\.(cpp|cc|c\\+\\+|cxx|c|cl|h|hpp|m|mm)' >not-tidy.diff \
    && if ! grep -q : not-tidy.diff ; then \
        echo "Clean code. Success."; \
    else \
        echo "Code not tidy."; \
        echo "Please run clang-tidy-diff on your changes and push again:"; \
        echo "    git diff origin/$LLPC_BASE_REF -U0 --no-color | ../llvm-project/clang-tools-extra/clang-tidy/tool/clang-tidy-diff.py -p1 -fix"; \
        echo ""; \
        echo "To disable a lint, add \`// NOLINT\` at the end of the line."; \
        echo ""; \
        echo "Diff:"; \
        cat not-tidy.diff; \
        echo ""; \
        exit 3; \
    fi
