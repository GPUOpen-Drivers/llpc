/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  NullFragmentShader.cpp
 * @brief LGC source file: The class to generate the null fragment shader when linking.
 ***********************************************************************************************************************
 */

#include "NullFragmentShader.h"
#include "lgc/patch/FragColorExport.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Target/TargetMachine.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Generate the IR module for the null fragment shader
//
// @returns : The module containing the null fragment shader.
Module *NullFragmentShader::generate() {
  Module *module = generateEmptyModule();
  Function *entryPoint = FragColorExport::generateNullFragmentShader(*module, m_pipelineState, getGlueShaderName());
  addDummyExportIfNecessary(entryPoint);
  return module;
}

// =====================================================================================================================
// Adds a dummy export to the entry point if it is needed.
//
// @param [in/out] entryPoint : The function in which to add the dummy export.
void NullFragmentShader::addDummyExportIfNecessary(Function *entryPoint) const {
  if (m_lgcContext->getTargetInfo().getGfxIpVersion().major < 10) {
    auto ret = cast<ReturnInst>(entryPoint->back().getTerminator());
    BuilderBase builder(ret);
    FragColorExport::addDummyExport(builder);
  }
}

// =====================================================================================================================
// Creates an empty module to be used for generating the null fragment shader.
//
// @returns : The new module.
Module *NullFragmentShader::generateEmptyModule() const {
  Module *module = new Module("nullFragmentShader", getContext());
  TargetMachine *targetMachine = m_lgcContext->getTargetMachine();
  module->setTargetTriple(targetMachine->getTargetTriple().getTriple());
  module->setDataLayout(targetMachine->createDataLayout());
  return module;
}
