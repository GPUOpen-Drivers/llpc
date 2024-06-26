/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  SubgroupBuilder.h
 * @brief LLPC header file: declaration of lgc::SubgroupBuilder implementation classes
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/builder/BuilderImpl.h"

namespace lgc {

// =====================================================================================================================
// SubgroupBuilder class
//
// This class is meant to only be used by the LowerSubgroupOps pass. Using it from other passes could result in bugs
// when the wave size gets changed during a later stage.
class SubgroupBuilder : public BuilderImpl {
public:
  SubgroupBuilder(Pipeline *pipeline) : BuilderImpl(pipeline){};

  // =====================================================================================================================
  // Create a subgroup elect.
  //
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateSubgroupElect(const llvm::Twine &instName = "");

  // Create a subgroup any
  //
  // @param value : The value to compare
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateSubgroupAny(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup all.
  //
  // @param value : The value to compare
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateSubgroupAll(llvm::Value *const value, const llvm::Twine &instName = "") {
    return createSubgroupAll(value, getShaderStage(GetInsertBlock()->getParent()).value(), instName);
  }

  // Create a subgroup all equal.
  //
  // @param value : The value to compare
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateSubgroupAllEqual(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup rotate call.
  //
  // @param value : The value to read from the chosen rotated lane to all active lanes.
  // @param delta : The delta/offset added to lane id.
  // @param clusterSize : The cluster size if exists.
  // @param instName : Name to give final instruction.
  llvm::Value *CreateSubgroupRotate(llvm::Value *const value, llvm::Value *const delta, llvm::Value *const clusterSize,
                                    const llvm::Twine &instName = "");

private:
  SubgroupBuilder() = delete;
  SubgroupBuilder(const SubgroupBuilder &) = delete;
  SubgroupBuilder &operator=(const SubgroupBuilder &) = delete;

  // The subgroup operation with explicit shader stage as parameter.
  llvm::Value *createSubgroupAll(llvm::Value *const value, ShaderStageEnum shaderStage, const llvm::Twine &instName);
};

} // namespace lgc
