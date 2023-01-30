#
# Dockerfile for LLPC Continuous Integration.
# Contains the base image used for incremental builds of llpc.
# Sample invocation:
#    docker build . --file docker/amdvlk.Dockerfile               \
#                   --build-arg BRANCH=dev                        \
#                   --build-arg CONFIG=Release                    \
#                   --build-arg FEATURES="+clang+ubsan+asan"      \
#                   --build-arg GENERATOR=Ninja                   \
#                   --tag kuhar/amdvlk:nightly
#
# Required arguments:
# - BRANCH: The base AMDVLK branch to use (e.g., master, dev, releases/<name>)
# - CONFIG: Debug or Release
# - FEATURES: A '+'-separated set of features to enable
# - GENERATOR: CMake generator to use (e.g., "Unix Makefiles", Ninja)
#

FROM ubuntu:20.04

ARG BRANCH
ARG CONFIG
ARG FEATURES
ARG GENERATOR

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

# Update the VulkanSDK 1.3.216 or higher, install the shader compiler tools for gpurt.
RUN wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | apt-key add - \
    && wget -qO /etc/apt/sources.list.d/lunarg-vulkan-1.3.216-focal.list https://packages.lunarg.com/vulkan/1.3.216/lunarg-vulkan-1.3.216-focal.list \
    && apt-get update \
    && apt-get install -yqq --no-install-recommends dxc glslang-tools \
    && rm -rf /var/lib/apt/lists/* \
    && dxc --version \
    && spirv-remap --version

# Checkout all repositories. Replace llpc with the version in LLPC_SOURCE_DIR.
# The /vulkandriver/env.sh file is for extra env variables used by later commands.
WORKDIR /vulkandriver
RUN wget -P /usr/bin/ https://storage.googleapis.com/git-repo-downloads/repo \
    && chmod +x /usr/bin/repo \
    && repo init -u https://github.com/GPUOpen-Drivers/AMDVLK.git -b "$BRANCH" -m build_with_tools.xml \
    && repo sync -c --no-clone-bundle -j$(nproc) \
    && touch ./env.sh

# Copy update script into container
COPY docker/update-llpc.sh /vulkandriver/

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
         echo "export LD_LIBRARY_PATH=$(dirname $(clang -print-file-name=libclang_rt.asan-x86_64.so))" >> /vulkandriver/env.sh; \
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
    && cmake "/vulkandriver/drivers/xgl" \
          -G "$GENERATOR" \
          -DCMAKE_BUILD_TYPE="$CONFIG" \
          -DXGL_BUILD_TESTS=ON \
          -DXGL_BUILD_TOOLS=ON \
          -DICD_ANALYSIS_WARNINGS_AS_ERRORS=OFF \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          "${EXTRA_FLAGS[@]}" \
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
