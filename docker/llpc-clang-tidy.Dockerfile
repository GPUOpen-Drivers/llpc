#
# Dockerfile for LLPC Continuous Integration.
# Sample invocation:
#    docker build .                                                                                         \
#      --file docker/llpc-clang-tidy.Dockerfile                                                             \
#      --build-arg AMDVLK_IMAGE=amdvlkadmin/amdvlk_release_clang:nightly                                    \
#      --build-arg LLPC_REPO_NAME=GPUOpen-Drivers/llpc                                                      \
#      --build-arg LLPC_REPO_REF=<GIT_REF>                                                                  \
#      --build-arg LLPC_REPO_SHA=<GIT_SHA>                                                                  \
#      --tag llpc-ci/llpc
#
# Required arguments:
# - AMDVLK_IMAGE: Base image name for prebuilt amdvlk
# - LLPC_REPO_NAME: Name of the llpc repository to clone
# - LLPC_REPO_REF: ref name to checkout
# - LLPC_REPO_SHA: SHA of the commit to checkout
# - LLPC_BASE_REF: ref name for the base of the tested change
#

# Resume build from the specified image.
ARG AMDVLK_IMAGE
FROM "$AMDVLK_IMAGE"

ARG LLPC_REPO_NAME
ARG LLPC_REPO_REF
ARG LLPC_REPO_SHA
ARG LLPC_BASE_REF

# Use bash instead of sh in this docker file.
SHELL ["/bin/bash", "-c"]

COPY docker/update-llpc.sh /vulkandriver/

RUN /vulkandriver/update-llpc.sh
RUN git -C /vulkandriver/drivers/llpc fetch origin "$LLPC_BASE_REF" --update-head-ok

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
