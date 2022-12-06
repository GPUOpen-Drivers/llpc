#!/usr/bin/env bash
# Update the driver and clone a specified version of LLPC.
# Shared code between docker containers.
set -euxo pipefail

# Sync the repos. Replace the base LLPC with a freshly checked-out one.
cat /vulkandriver/build_info.txt
(cd /vulkandriver && repo sync -c --no-clone-bundle -j$(nproc))
git -C /vulkandriver/drivers/llpc remote rename vulkan-github origin
git -C /vulkandriver/drivers/llpc/imported/llvm-dialects remote rename vulkan-github origin
git -C /vulkandriver/drivers/llpc fetch origin +"$LLPC_REPO_SHA":"$LLPC_REPO_REF" --update-head-ok
git -C /vulkandriver/drivers/llpc checkout "$LLPC_REPO_SHA"
git -C /vulkandriver/drivers/llpc submodule update --init
