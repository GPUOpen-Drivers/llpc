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
# - LLPC_REPO_REF: ref name to checkout
# - LLPC_REPO_SHA: SHA of the commit to checkout
#

# Resume build from the specified image.
ARG AMDVLK_IMAGE
FROM "$AMDVLK_IMAGE"

ARG LLPC_REPO_NAME
ARG LLPC_REPO_REF
ARG LLPC_REPO_SHA

# Use bash instead of sh in this docker file.
SHELL ["/bin/bash", "-c"]

# Sync the repos. Replace the base LLPC with a freshly checked-out one.
RUN cat /vulkandriver/build_info.txt \
    && (cd /vulkandriver && repo sync -c --no-clone-bundle -j$(nproc)) \
    && git -C /vulkandriver/drivers/llpc remote add origin https://github.com/"$LLPC_REPO_NAME".git \
    && git -C /vulkandriver/drivers/llpc fetch origin +"$LLPC_REPO_SHA":"$LLPC_REPO_REF" --update-head-ok \
    && git -C /vulkandriver/drivers/llpc checkout "$LLPC_REPO_SHA"

# Build LLPC.
WORKDIR /vulkandriver/builds/ci-build
RUN source /vulkandriver/env.sh \
    && cmake --build . \
    && cmake --build . --target amdllpc \
    && cmake --build . --target spvgen

# Run the lit test suite.
RUN source /vulkandriver/env.sh \
    && cmake --build . --target check-amdllpc check-amdllpc-units -- -v \
    && cmake --build . --target check-lgc check-lgc-units -- -v
