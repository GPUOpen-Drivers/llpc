/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Google LLC All Rights Reserved.
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
 * @file  llpcPatchFetchShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class Llpc::PatchFetchShader.
 ***********************************************************************************************************************
 */
#include "SystemValues.h"
#include "VertexFetch.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/ResourceUsage.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llpc-patch-fetch-shader"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Pass to generate fetch shader if required
class PatchFetchShader : public Patch {
public:
  static char ID;
  PatchFetchShader() : Patch(ID) {}

  bool runOnModule(Module &module) override;

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
    // Pass does not preserve PipelineShaders as it adds a new shader.
  }

private:
  PatchFetchShader(const PatchFetchShader &) = delete;
  PatchFetchShader &operator=(const PatchFetchShader &) = delete;

  FunctionType *generateFetchShaderEntryPointType(uint64_t *inRegMask);
  Function *createEntryPoint();
  void buildFetchShaderBody(Function *entryPoint);
  Value *fetchVertexInput(Type *fetchType, Type *returnType, unsigned location, unsigned component,
                          VertexFetch *vertexFetch);

  PipelineState *m_pipelineState;               // Pipeline state
  std::unique_ptr<llvm::IRBuilder<>> m_builder; // The IRBuilder.
};

char PatchFetchShader::ID = 0;

} // namespace lgc

// =====================================================================================================================
// Create pass to generate copy shader if required.
ModulePass *lgc::createPatchFetchShader() {
  return new PatchFetchShader();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool PatchFetchShader::runOnModule(Module &module) // [in,out] LLVM module to be run on
{
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Fetch-Shader\n");
  Patch::init(&module);
  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);

  if (!m_pipelineState->getLgcContext()->buildingFetchShader())
    return false;

  m_builder.reset(new IRBuilder<>(module.getContext()));
  Function *entryPoint = createEntryPoint();
  buildFetchShaderBody(entryPoint);

  return true;
}

// =====================================================================================================================
// Generate the body of a fetch shader in the current context.
//
// @param entryPoint : The entry point to be modified.
void PatchFetchShader::buildFetchShaderBody(Function *entryPoint) {
  BasicBlock *basicBlock = BasicBlock::Create(*m_context, ".entry", entryPoint);

  // The vertex fetch object needs an Instruction for the insertion point, so we cannot simply add to the end of the
  // basic block.  So we create the return instruction now, and insert before that.
  Value *retValue = UndefValue::get(entryPoint->getReturnType());
  m_builder->SetInsertPoint(basicBlock);
  Instruction *insertionPoint = m_builder->CreateRet(retValue);
  m_builder->SetInsertPoint(insertionPoint);

  // Pass through the fetch shader inputs.  Doing a bitcast to make sure the values end up in the the appropriate
  // register type.  Integer values are placed in SGPRS, and floating point values are in VGPRS.
  for (unsigned parameterIdx = 0; parameterIdx < entryPoint->arg_size(); parameterIdx++) {
    Value *parameter = entryPoint->getArg(parameterIdx);
    if (parameter->getType() != retValue->getType()->getStructElementType(parameterIdx)) {
      parameter = m_builder->CreateZExtOrBitCast(parameter, retValue->getType()->getStructElementType(parameterIdx));
    }
    retValue = m_builder->CreateInsertValue(retValue, parameter, {parameterIdx});
  }

  // Initialize the vertex fetch object.
  PipelineSystemValues pipelineSysValues;
  pipelineSysValues.initialize(m_pipelineState);
  VertexFetch vertexFetch(entryPoint, pipelineSysValues.get(entryPoint), m_pipelineState);

  // Fetch the vertex inputs, and add them to the return value.
  unsigned index = entryPoint->arg_size();
  VsInterfaceData *vsInterfaceData = m_pipelineState->getLgcContext()->getVsInterfaceData();
  const auto &vsInputTypeInfo = vsInterfaceData->getVertexInputTypeInfo();
  for (auto vertexInputInfo : vsInputTypeInfo) {
    unsigned location = vertexInputInfo.first.first;
    unsigned component = vertexInputInfo.first.second;
    Type *fetchType = vsInterfaceData->getVertexInputType(location, component, m_context);
    Type *returnType = entryPoint->getReturnType()->getStructElementType(index);
    Value *inputValue = fetchVertexInput(fetchType, returnType, location, component, &vertexFetch);
    retValue = m_builder->CreateInsertValue(retValue, inputValue, {index});
    ++index;
  }

  // Update the return value in the return instruction.
  m_builder->GetInsertPoint()->setOperand(0, retValue);
}

// =====================================================================================================================
// Creates an entry point for a fetch shader in the current context.  The body of the function will be empty.
//
Function *PatchFetchShader::createEntryPoint() {
  uint64_t inRegMask = 0;
  FunctionType *entryPointTy = generateFetchShaderEntryPointType(&inRegMask);
  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, "fetch", m_module);
  entryPoint->setCallingConv(CallingConv::AMDGPU_VS);
  entryPoint->addFnAttr(Attribute::NoUnwind);

  // Update attributes of new entry-point
  for (auto arg = entryPoint->arg_begin(), end = entryPoint->arg_end(); arg != end; ++arg) {
    auto argIdx = arg->getArgNo();
    if (inRegMask & (1ull << argIdx)) {
      arg->addAttr(Attribute::InReg);
    }
  }

  // Add execution model metadata to the function.
  auto execModelMeta = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), ShaderStageFetch));
  auto execModelMetaNode = MDNode::get(*m_context, execModelMeta);
  const static char ShaderStageMetadata[] = "lgc.shaderstage"; // Also defined in ./lgc/state/ShaderStage.cpp
  entryPoint->addMetadata(ShaderStageMetadata, *execModelMetaNode);

  // Tell pipeline state there is a fetch shader.
  m_pipelineState->setShaderStageMask(m_pipelineState->getShaderStageMask() | (1U << ShaderStageFetch));
  return entryPoint;
}

// =====================================================================================================================
// Returns the type for the fetch shader.  Updates the interface data in the pipeline state as needed.
//
// @param inRegMask [out]: The bitmask indicating which arguments in the function type should be placed in SGPRS.
FunctionType *PatchFetchShader::generateFetchShaderEntryPointType(uint64_t *inRegMask) {
  std::vector<Type *> argTys;
  std::vector<Type *> retTys;
  LgcContext *builderContext = m_pipelineState->getLgcContext();
  VsInterfaceData *vsInterfaceData = builderContext->getVsInterfaceData();
  *inRegMask = 0;

  for (unsigned i = 0; i <= vsInterfaceData->getLastSgpr(); i++) {
    // Add SGPR inputs.  The return type is Int32 to make sure they are assigned to an SGPR on exit.
    argTys.push_back(Type::getInt32Ty(*m_context));
    retTys.push_back(Type::getInt32Ty(*m_context));
  }
  *inRegMask = (1 << (vsInterfaceData->getLastSgpr() + 1)) - 1;

  InterfaceData *intfData = m_pipelineState->getShaderInterfaceData(ShaderStageFetch);
  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFetch)->builtInUsage.vs;

  intfData->entryArgIdxs.vs.vbTablePtr = vsInterfaceData->getVertexBufferRegister();

  auto &entryArgIdxs = intfData->entryArgIdxs;
  entryArgIdxs.initialized = true;
  entryArgIdxs.vs.baseVertex = vsInterfaceData->getBaseVertexRegister();
  entryArgIdxs.vs.baseInstance = vsInterfaceData->getBaseInstanceRegister();

  // Add system values.  The return types are changed to Float32 to indicate that they must be in a VGPR on exit.

  // Vertex ID
  builtInUsage.vertexIndex = true;
  entryArgIdxs.vs.vertexId = argTys.size();
  argTys.push_back(Type::getInt32Ty(*m_context));
  retTys.push_back(Type::getFloatTy(*m_context));

  // Relative vertex ID (auto index)
  entryArgIdxs.vs.relVertexId = argTys.size();
  argTys.push_back(Type::getInt32Ty(*m_context));
  retTys.push_back(Type::getFloatTy(*m_context));

  // Primitive ID
  builtInUsage.primitiveId = true;
  entryArgIdxs.vs.instanceId = argTys.size();
  entryArgIdxs.vs.primitiveId = argTys.size();
  argTys.push_back(Type::getInt32Ty(*m_context));
  retTys.push_back(Type::getFloatTy(*m_context));

  // Instance ID
  builtInUsage.instanceIndex = true;
  entryArgIdxs.vs.instanceId = argTys.size();
  argTys.push_back(Type::getInt32Ty(*m_context));
  retTys.push_back(Type::getFloatTy(*m_context));

  // Add the vertex input types to the return struct.
  const auto &vsInputTypeInfo = vsInterfaceData->getVertexInputTypeInfo();
  for (auto vertexInputInfo : vsInputTypeInfo) {
    unsigned location = vertexInputInfo.first.first;
    unsigned component = vertexInputInfo.first.second;
    Type *type = vsInterfaceData->getVertexInputType(location, component, m_context);

    // To make sure that these values are returned in VGPRs, we must cast the type to a floating point type.
    if (!type->getScalarType()->isFloatTy()) {
      if (type->isVectorTy()) {
        type = VectorType::get(Type::getFloatTy(*m_context), cast<VectorType>(type)->getElementCount());
      } else {
        type = Type::getFloatTy(*m_context);
      }
    }
    retTys.push_back(type);
  }

  Type *returnStructType = StructType::create(*m_context, retTys);
  return FunctionType::get(returnStructType, argTys, false);
}

// =====================================================================================================================
// Load the vertex input at the given location and return that value.
//
// @param fetchType : The type of the vertex input at the given location.
// @param returnType : The type that the returned value should be.
// @param location : The location of the vertex input to load.
// @param vertexFetch : An already initialzed VertexFetch object.
Value *PatchFetchShader::fetchVertexInput(Type *fetchType, Type *returnType, unsigned location, unsigned component,
                                          VertexFetch *vertexFetch) {
  assert(fetchType != nullptr);
  assert(returnType != nullptr);
  assert(vertexFetch != nullptr);

  Value *input = nullptr;
  auto vertex = vertexFetch->run(fetchType, location, component, &*m_builder->GetInsertPoint());

  // Cast vertex fetch results if necessary
  const Type *vertexTy = vertex->getType();
  if (vertexTy != returnType) {
    assert(canBitCast(vertexTy, returnType));
    input = m_builder->CreateBitCast(vertex, returnType);
  } else {
    input = vertex;
  }

  return input;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchFetchShader, DEBUG_TYPE, "Patch LLVM for fetch shader generation", false, false)
