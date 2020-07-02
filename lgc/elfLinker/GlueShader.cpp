/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  GlueShader.cpp
 * @brief LGC source file: Glue shader (fetch shader, parameter/color export shader) generated in linking
 ***********************************************************************************************************************
 */
#include "GlueShader.h"
#include "lgc/BuilderBase.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/patch/VertexFetch.h"
#include "lgc/state/PassManagerCache.h"
#include "lgc/state/ShaderStage.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Compile the glue shader
//
// @param [in/out] outStream : Stream to write ELF to
void GlueShader::compile(raw_pwrite_stream &outStream) {
  // Generate the glue shader IR module.
  std::unique_ptr<Module> module(generate());

  // Add empty PAL metadata, to ensure that the back-end writes its PAL metadata in MsgPack format.
  PalMetadata *palMetadata = new PalMetadata(nullptr);
  palMetadata->record(&*module);
  delete palMetadata;

  // Get the pass manager and run it on the module, generating ELF.
  PassManager &passManager = m_lgcContext->getPassManagerCache()->getGlueShaderPassManager(outStream);
  passManager.run(*module);
}

namespace {

// =====================================================================================================================
// A fetch shader
class FetchShader : public GlueShader {
public:
  FetchShader(PipelineState *pipelineState, ArrayRef<VertexFetchInfo> fetches, const VsEntryRegInfo &vsEntryRegInfo);
  ~FetchShader() override {}

  // Get the string for this glue shader. This is some encoding or hash of the inputs to the create*Shader function
  // that the front-end client can use as a cache key to avoid compiling the same glue shader more than once.
  StringRef getString() override;

  // Get the symbol name of the main shader that this glue shader is prolog or epilog for.
  StringRef getMainShaderName() override;

  // Get whether this glue shader is a prolog (rather than epilog) for its main shader.
  bool isProlog() override { return true; }

  // Get the name of this glue shader.
  StringRef getName() const override { return "fetch shader"; }

protected:
  // Generate the glue shader to IR module
  Module *generate() override;

private:
  Function *createFetchFunc();

  // The information stored here is all that is needed to generate the fetch shader. We deliberately do not
  // have access to PipelineState, so we can hash the information here and let the front-end use it as the
  // key for a cache of glue shaders.
  SmallVector<VertexFetchInfo, 8> m_fetches;
  VsEntryRegInfo m_vsEntryRegInfo;
  SmallVector<const VertexInputDescription *, 8> m_fetchDescriptions;
  // The encoded or hashed (in some way) single string version of the above.
  std::string m_shaderString;
};

} // anonymous namespace

// =====================================================================================================================
// Create a fetch shader object
GlueShader *GlueShader::createFetchShader(PipelineState *pipelineState, ArrayRef<VertexFetchInfo> fetches,
                                          const VsEntryRegInfo &vsEntryRegInfo) {
  return new FetchShader(pipelineState, fetches, vsEntryRegInfo);
}

// =====================================================================================================================
// Constructor. This is where we store all the information needed to generate the fetch shader; other methods
// do not need to look at PipelineState.
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
    m_shaderString =
        StringRef(reinterpret_cast<const char *>(m_fetches.data()), m_fetches.size() * sizeof(VertexFetchInfo)).str();
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

  // Process each vertex input.
  std::unique_ptr<VertexFetch> vertexFetch(VertexFetch::create(m_lgcContext));
  auto ret = cast<ReturnInst>(fetchFunc->back().getTerminator());
  Value *result = ret->getOperand(0);
  BuilderBase builder(ret);

  for (unsigned idx = 0; idx != m_fetches.size(); ++idx) {
    const auto &fetch = m_fetches[idx];
    const VertexInputDescription *description = m_fetchDescriptions[idx];
    unsigned structIdx = idx + m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.vgprCount;
    Type *ty = cast<StructType>(result->getType())->getElementType(structIdx);

    if (description) {
      // Fetch the vertex.
      Value *vertex = vertexFetch->fetchVertex(ty, description, fetch.location, fetch.component, builder);
      result = builder.CreateInsertValue(result, vertex, structIdx);
    }
  }
  ret->setOperand(0, result);

  // Hook up the inputs (vertex buffer, base vertex, base instance, vertex ID, instance ID). The fetchVertex calls
  // left its uses of them as lgc.special.user.data and lgc.shader.input calls.
  for (Function &func : *fetchFunc->getParent()) {
    if (!func.isDeclaration())
      continue;
    if (func.getName().startswith(lgcName::SpecialUserData) || func.getName().startswith(lgcName::ShaderInput)) {
      while (!func.use_empty()) {
        auto call = cast<CallInst>(func.use_begin()->getUser());
        Value *replacement = nullptr;
        switch (cast<ConstantInt>(call->getArgOperand(0))->getZExtValue()) {
        case static_cast<unsigned>(UserDataMapping::VertexBufferTable): {
          // Need to extend 32-bit vertex buffer table address to 64 bits.
          AddressExtender extender(fetchFunc);
          unsigned highAddr = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
          builder.SetInsertPoint(&*fetchFunc->front().getFirstInsertionPt());
          replacement = extender.extend(fetchFunc->getArg(m_vsEntryRegInfo.vertexBufferTable), highAddr,
                                        call->getType(), builder);
          break;
        }
        case static_cast<unsigned>(UserDataMapping::BaseVertex):
          replacement = fetchFunc->getArg(m_vsEntryRegInfo.baseVertex);
          break;
        case static_cast<unsigned>(UserDataMapping::BaseInstance):
          replacement = fetchFunc->getArg(m_vsEntryRegInfo.baseInstance);
          break;
        case static_cast<unsigned>(ShaderInput::VertexId):
          builder.SetInsertPoint(&*fetchFunc->front().getFirstInsertionPt());
          replacement = builder.CreateBitCast(fetchFunc->getArg(m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.vertexId),
                                              builder.getInt32Ty());
          break;
        case static_cast<unsigned>(ShaderInput::InstanceId):
          builder.SetInsertPoint(&*fetchFunc->front().getFirstInsertionPt());
          replacement = builder.CreateBitCast(
              fetchFunc->getArg(m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.instanceId), builder.getInt32Ty());
          break;
        default:
          llvm_unreachable("Unexpected special user data or shader input");
        }
        call->replaceAllUsesWith(replacement);
        call->eraseFromParent();
      }
    }
  }

  return fetchFunc->getParent();
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
  StringRef funcName = getEntryPointName(m_vsEntryRegInfo.callingConv, /*isFetchlessVs=*/false);
  Function *func = Function::Create(funcTy, GlobalValue::ExternalLinkage, funcName, module);
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
    constexpr unsigned MergedWaveInfoSgpr = 3;
    builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec_from_input, {},
                            {func->getArg(MergedWaveInfoSgpr), builder.getInt32(0)});
  }

  // Copy the wave dispatch SGPRs and VGPRs from inputs to outputs.
  builder.SetInsertPoint(&func->back());
  Value *retVal = UndefValue::get(retTy);
  for (unsigned i = 0; i != m_vsEntryRegInfo.sgprCount + m_vsEntryRegInfo.vgprCount; ++i)
    retVal = builder.CreateInsertValue(retVal, func->getArg(i), i);
  builder.CreateRet(retVal);

  return func;
}
