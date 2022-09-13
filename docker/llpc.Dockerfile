#
# Dockerfile for LLPC Continuous Integration.
# Sample invocation:
#    docker build .                                                                                       \
#      --file docker/llpc.Dockerfile                                                                      \
#      --build-arg AMDVLK_IMAGE=us-docker.pkg.dev/stadia-open-source/amdvlk-public-ci/gcc_release:nightly \
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

# Resume build from the specified image.
ARG AMDVLK_IMAGE
FROM "$AMDVLK_IMAGE"

ARG LLPC_REPO_NAME
ARG LLPC_REPO_REF
ARG LLPC_REPO_SHA
ARG FEATURES

# Use bash instead of sh in this docker file.
SHELL ["/bin/bash", "-c"]

RUN export DEBIAN_FRONTEND=noninteractive && export TZ=America/New_York \
    && apt-get update \
    && apt-get install -yqq --no-install-recommends \
       gpg gpg-agent \
    && rm -rf /var/lib/apt/lists/*

# Update the VulkanSDK 1.3.216 or higher, install the shader compiler tools for gpurt.
RUN wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | apt-key add - \
    && wget -qO /etc/apt/sources.list.d/lunarg-vulkan-1.3.216-focal.list https://packages.lunarg.com/vulkan/1.3.216/lunarg-vulkan-1.3.216-focal.list \
    && apt-get update \
    && apt-get install -yqq --no-install-recommends dxc glslang-tools \
    && rm -rf /var/lib/apt/lists/* \
    && dxc --version \
    && spirv-remap --version

# Copy helper scripts into container.
COPY docker/*.sh /vulkandriver/

# Sync the repos. Replace the base LLPC with a freshly checked-out one.
RUN /vulkandriver/update-llpc.sh

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

# Generate code coverage report for LLPC.
RUN if echo "$FEATURES" | grep -q "+coverage" ; then \
      /vulkandriver/generate-coverage-report.sh; \
    fi
