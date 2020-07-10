#
# Dockerfile for LLPC Continuous Integration.
# Sample invocation:
#    docker build . --file docker/llpc.Dockerfile                                             \
#                   --build-arg AMDVLK_IMAGE=gcr.io/stadia-open-source/amdvlk:nightly         \
#                   --build-arg LLPC_REPO_NAME=GPUOpen-Drivers/llpc                           \
#                   --build-arg LLPC_REPO_REF=<GIT_REF>                                       \
#                   --build-arg LLPC_REPO_SHA=<GIT_SHA>                                       \
#                   --tag llpc-ci/llpc
#
# Required arguments:
# - AMDVLK_IMAGE: Base image name for prebuilt amdvlk
# - LLPC_REPO_NAME: Name of the llpc repository to clone
# - RESPOSITORY_SHA: SHA of the commit to checkout
#

# Resume build from the specified image.
ARG AMDVLK_IMAGE
FROM "$AMDVLK_IMAGE"

ARG FEATURES
ARG LLPC_REPO_NAME
ARG LLPC_REPO_REF
ARG LLPC_REPO_SHA

# Sync the repos. Replace the base LLPC with a freshly checked-out one.
RUN cat /vulkandriver/build_info.txt \
    && (cd /vulkandriver && repo sync -c --no-clone-bundle -j$(nproc)) \
    && rm -rf /vulkandriver/drivers/llpc \
    && git clone https://github.com/"$LLPC_REPO_NAME".git /vulkandriver/drivers/llpc \
    && git -C /vulkandriver/drivers/llpc fetch origin +"$LLPC_REPO_SHA":"$LLPC_REPO_REF" --update-head-ok \
    && git -C /vulkandriver/drivers/llpc checkout "$LLPC_REPO_SHA"

# Build LLPC.
WORKDIR /vulkandriver/builds/ci-build
RUN if echo "$FEATURES" | grep -q "+sanitizers" ; then \
      export ASAN_OPTIONS=detect_leaks=0 \
      && export LD_PRELOAD=/usr/lib/llvm-9/lib/clang/9.0.0/lib/linux/libclang_rt.asan-x86_64.so; \
    fi \
    && cmake --build . \
    && cmake --build . --target amdllpc \
    && cmake --build . --target spvgen

# Run the lit test suite.
RUN if echo "$FEATURES" | grep -q "+sanitizers" ; then \
      export ASAN_OPTIONS=detect_leaks=0 \
      && export LD_PRELOAD=/usr/lib/llvm-9/lib/clang/9.0.0/lib/linux/libclang_rt.asan-x86_64.so; \
    fi \
    && cmake --build . --target check-amdllpc -- -v \
    && cmake --build . --target check-lgc -- -v
