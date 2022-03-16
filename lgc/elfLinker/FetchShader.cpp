/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  FetchShader.cpp
 * @brief LGC source file: The class to generate the fetch shader used when linking a pipeline.
 ***********************************************************************************************************************
 */

#include "FetchShader.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/patch/VertexFetch.h"
#include "lgc/util/AddressExtender.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Target/TargetMachine.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Constructor. This is where we store all the information needed to generate the fetch shader; other methods
// do not need to look at PipelineState.
//
// @param pipelineState : The pipeline state for which the fetch shader will be generated.
// @param fetches : The vertex fetch information for the vertex shader.
// @param vsEntryRegInfo : The information about the contents of the parameters to the vertex shader.
FetchShader::FetchShader(PipelineState *pipelineState, ArrayRef<VertexFetchInfo> fetches,
                         const VsEntryRegInfo &vsEntryRegInfo)
    : GlueShader(pipelineState->getLgcContext()), m_vsEntryRegInfo(vsEntryRegInfo) {
  m_fetches.append(fetches.begin(), fetches.end());
  for (const auto &fetch : m_fetches)
    m_fetchDescriptions.push_back(pipelineState->findVertexInputDescription(fetch.location));
}

// =====================================================================================================================
// Get the string for this fetch shader. This is some encoding or hash of the inputs to the createFetchShader function
// that the front-end client can use as a cache key to avoid compiling the same glue shader more than once.
StringRef FetchShader::getString() {
  if (m_shaderString.empty()) {
    for (VertexFetchInfo fetchInfo : m_fetches) {
      m_shaderString += StringRef(reinterpret_cast<const char *>(&fetchInfo.location), sizeof(fetchInfo.location));
      m_shaderString += StringRef(reinterpret_cast<const char *>(&fetchInfo.component), sizeof(fetchInfo.component));
      m_shaderString += getTypeName(fetchInfo.ty);
    }
    m_shaderString += StringRef(reinterpret_cast<const char *>(&m_vsEntryRegInfo), sizeof(m_vsEntryRegInfo)).str();
    for (const VertexInputDescription *description : m_fetchDescriptions) {
      if (!description)
        m_shaderString += StringRef("\0", 1);
      else
        m_shaderString += StringRef(reinterpret_cast<const char *>(description), sizeof(*description));
    }
  }
  return m_shaderString;
}

// =====================================================================================================================
// Get the symbol name of the main shader that this glue shader is prolog or epilog for
StringRef FetchShader::getMainShaderName() {
  return getEntryPointName(m_vsEntryRegInfo.callingConv, /*isFetchlessVs=*/true);
}

// =====================================================================================================================
// Generate the IR module for the fetch shader
Module *FetchShader::generate() {
  // Create the function.
  Function *fetchFunc = createFetchFunc();
  generateFetchShaderBody(fetchFunc);
  replaceShaderInputBuiltInFunctions(fetchFunc);
  return fetchFunc->getParent();
}

// =====================================================================================================================
// Generate the body of the fetch function using the shader input builtins to access the inputs to the shader.
//
// @param [in/out] fetchFunc : The function for the fetch shader.
void FetchShader::generateFetchShaderBody(Function *fetchFunc) { // Process each vertex input.
  std::unique_ptr<VertexFetch> vertexFetch(VertexFetch::create(m_lgcContext));
  auto ret = cast<ReturnInst>(fetchFunc->back().getTerminator());
  BuilderBase builder(ret);
  Value *result = ret->getOperand(0);

  for (unsigned idx = 0; idx != m_fetches.size(); ++idx) {
    const auto &fetch = m_fetches[idx];
    const VertexInputDescription *description = m_fetchDescriptions[idx];
    unsigned structIdx = idx + m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.vgprCount;

    if (description) {
      // Fetch the vertex.
      Value *vertex = vertexFetch->fetchVertex(fetch.ty, description, fetch.location, fetch.component, builder);
      Type *ty = cast<StructType>(result->getType())->getElementType(structIdx);
      vertex = builder.CreateBitCast(vertex, ty);
      result = builder.CreateInsertValue(result, vertex, structIdx);
    }
  }
  ret->setOperand(0, result);
}

// =====================================================================================================================
// Replaces calls to the shader input builtins in fetchFunc with code that will get the appropriate values from the
// arguments.
//
// @param [in/out] fetchFunc : The function for the fetch shader.
void FetchShader::replaceShaderInputBuiltInFunctions(Function *fetchFunc) const {
  auto ret = cast<ReturnInst>(fetchFunc->back().getTerminator());
  BuilderBase builder(ret);
  // Hook up the inputs (vertex buffer, base vertex, base instance,
  // vertex ID, instance ID). The fetchVertex calls
  // left its uses of them as lgc.special.user.data and lgc.shader.input calls.
  for (Function &func : *fetchFunc->getParent()) {
    if (!func.isDeclaration())
      continue;
    if (func.getName().startswith(lgcName::SpecialUserData) || func.getName().startswith(lgcName::ShaderInput)) {
      while (!func.use_empty()) {
        auto call = cast<CallInst>(func.use_begin()->getUser());
        Value *replacement = nullptr;
        replacement = getReplacementForInputBuiltIn(call);
        call->replaceAllUsesWith(replacement);
        call->eraseFromParent();
      }
    }
  }
}

// =====================================================================================================================
// Returns the value that is represented by |call|.  It will be in a position where in can be used in place of all
// uses of |call|.
//
// @param call : A call to a shader input builtin that needs to be replaced.
// @returns : The value that is represented by |call|.
Value *FetchShader::getReplacementForInputBuiltIn(CallInst *call) const {
  switch (cast<ConstantInt>(call->getArgOperand(0))->getZExtValue()) {
  case static_cast<unsigned>(UserDataMapping::VertexBufferTable):
    return getReplacementForVertexBufferTableBuiltIn(call);
  case static_cast<unsigned>(UserDataMapping::BaseVertex):
    return call->getFunction()->getArg(m_vsEntryRegInfo.baseVertex);
  case static_cast<unsigned>(UserDataMapping::BaseInstance):
    return call->getFunction()->getArg(m_vsEntryRegInfo.baseInstance);
  case static_cast<unsigned>(ShaderInput::VertexId):
    return getReplacementForVertexIdBuiltIn(call);
  case static_cast<unsigned>(ShaderInput::InstanceId):
    return getReplacementForInstanceIdBuiltIn(call);
  default:
    llvm_unreachable("Unexpected special user data or shader input");
  }
  return nullptr;
}

// =====================================================================================================================
// Returns the value of the instance id.  All new code will be place at the start of the function containing call.
//
// @param call : A call to the InstanceId shader input builtin.
// @returns : The value of the InstanceId.
Value *FetchShader::getReplacementForInstanceIdBuiltIn(CallInst *call) const {
  Function *callerFunction = call->getFunction();
  return getVgprArgument(m_vsEntryRegInfo.instanceId, callerFunction);
}

// =====================================================================================================================
// Returns the value of the vertex id.  All new code will be place at the start of the function containing call.
//
// @param call : A call to the VertexId shader input builtin.
// @returns : The value of the VertexId.
Value *FetchShader::getReplacementForVertexIdBuiltIn(CallInst *call) const {
  Function *callerFunction = call->getFunction();
  return getVgprArgument(m_vsEntryRegInfo.vertexId, callerFunction);
}

// =====================================================================================================================
// Returns the value of the argument in the function that corresponds to the given VGPR.
//
// @param vgpr : The VGPR number from which the value should come.
// @param function : The function from which to get the argument.
// @returns : The value of the argument.
Value *FetchShader::getVgprArgument(unsigned vgpr, Function *function) const {
  BuilderBase builder(&*function->front().getFirstInsertionPt());
  Argument *vertexId = function->getArg(m_vsEntryRegInfo.sgprCount + vgpr);
  return builder.CreateBitCast(vertexId, builder.getInt32Ty());
}

// =====================================================================================================================
// Returns the value of the address of the VertexBufferTable.  All new code will be place at the start of the
// function containing call.
//
// @param call : A call to the VertexBufferTable shader input builtin.
// @returns : The value of the address of the VertexBufferTable.
Value *FetchShader::getReplacementForVertexBufferTableBuiltIn(CallInst *call) const {
  // Need to extend 32-bit vertex buffer table address to 64 bits.
  Function *callerFunction = call->getFunction();
  AddressExtender extender(callerFunction);
  Value *highAddr = call->getArgOperand(1);
  BuilderBase builder(&*callerFunction->front().getFirstInsertionPt());
  Argument *vertexBufferTable = callerFunction->getArg(m_vsEntryRegInfo.vertexBufferTable);
  return extender.extend(vertexBufferTable, highAddr, call->getType(), builder);
}

// =====================================================================================================================
// Create module with function for the fetch shader. On return, the function contains only the code to copy the
// wave dispatch SGPRs and VGPRs to the return value.
Function *FetchShader::createFetchFunc() {
  // Create the module
  Module *module = new Module("fetchShader", getContext());
  TargetMachine *targetMachine = m_lgcContext->getTargetMachine();
  module->setTargetTriple(targetMachine->getTargetTriple().getTriple());
  module->setDataLayout(targetMachine->createDataLayout());

  // Get the function type. Its inputs are the wave dispatch SGPRs and VGPRs. Its return type is a struct
  // containing the wave dispatch SGPRs and VGPRs, plus the fetched values in VGPRs. In the return type struct,
  // VGPR values must be FP so the back-end puts them into VGPRs; we do the same for the inputs for symmetry.
  SmallVector<Type *, 16> types;
  types.append(m_vsEntryRegInfo.sgprCount, Type::getInt32Ty(getContext()));
  types.append(m_vsEntryRegInfo.vgprCount, Type::getFloatTy(getContext()));
  for (const auto &fetch : m_fetches)
    types.push_back(getVgprTy(fetch.ty));
  Type *retTy = StructType::get(getContext(), types);
  auto entryTys = ArrayRef<Type *>(types).slice(0, m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.vgprCount);
  auto funcTy = FunctionType::get(retTy, entryTys, false);

  // Create the function. Mark SGPR inputs as "inreg".
  Function *func = Function::Create(funcTy, GlobalValue::ExternalLinkage, getGlueShaderName(), module);
  func->setCallingConv(m_vsEntryRegInfo.callingConv);
  for (unsigned i = 0; i != m_vsEntryRegInfo.sgprCount; ++i)
    func->getArg(i)->addAttr(Attribute::InReg);

  // Add mnemonic names to input args.
  func->getArg(m_vsEntryRegInfo.vertexBufferTable)->setName("VertexBufferTable");
  func->getArg(m_vsEntryRegInfo.baseVertex)->setName("BaseVertex");
  func->getArg(m_vsEntryRegInfo.baseInstance)->setName("BaseInstance");
  func->getArg(m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.vertexId)->setName("VertexId");
  func->getArg(m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.instanceId)->setName("InstanceId");

  if (m_lgcContext->getTargetInfo().getGfxIpVersion().major >= 10) {
    // Set up wave32 or wave64 to match the vertex shader.
    func->addFnAttr("target-features", m_vsEntryRegInfo.wave32 ? "+wavefrontsize32" : "+wavefrontsize64");
  }

  BasicBlock *block = BasicBlock::Create(func->getContext(), "", func);
  BuilderBase builder(block);
  if (m_vsEntryRegInfo.callingConv == CallingConv::AMDGPU_HS ||
      m_vsEntryRegInfo.callingConv == CallingConv::AMDGPU_GS) {
    // The VS is the first half of a merged shader, LS-HS or ES-GS. This fetch shader needs to include code
    // to enable the correct lanes for the vertices. It happens that LS vertex count in LS-HS and ES vertex
    // count in ES-GS are in the same place: the low 8 bits of s3.
    constexpr unsigned mergedWaveInfoSgpr = 3;
    builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec_from_input, {},
                            {func->getArg(mergedWaveInfoSgpr), builder.getInt32(0)});
  }

  // Copy the wave dispatch SGPRs and VGPRs from inputs to outputs.
  builder.SetInsertPoint(&func->back());
  Value *retVal = UndefValue::get(retTy);
  for (unsigned i = 0; i != m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.vgprCount; ++i)
    retVal = builder.CreateInsertValue(retVal, func->getArg(i), i);
  builder.CreateRet(retVal);

  return func;
}
