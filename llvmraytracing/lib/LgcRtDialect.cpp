/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
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

// Implementation of the lgc.rt dialect definition

#include "lgc/LgcRtDialect.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"

#define GET_INCLUDES
#define GET_DIALECT_DEFS
#include "LgcRtDialect.cpp.inc"

using namespace llvm;

namespace llvm {
class LLVMContext;
} // namespace llvm

namespace {

// Shader stage metadata to identify the shader stage of a given function.
constexpr const char ShaderStageMetadata[] = "lgc.rt.shaderstage";

// PAQ (payload access qualifier) metadata on a shader function, with an array
// of ints of the same form as the paq argument to the trace.ray dialect op,
// giving the size and possibly further access qualification for the payload.
// Example:
//
//  define ..MyClosestHitShader@@..(ptr addrspace(5) %0, ptr addrspace(5) %1)
//  !lgc.rt.paq !3
//  ...
//  !3 = !{[1 x i32][i32 16]}
//
// In this example, the array has a single entry, and in that case it is just
// the payload size in bytes, and assumes that all shader types can read and
// write the whole payload.
constexpr const char PaqMetadata[] = "lgc.rt.paq";

// Argument size metadata on a callable shader, giving the argument size in
// bytes.
constexpr const char ArgSizeMetadata[] = "lgc.rt.arg.size";

// Attribute size metadata on certain shader types, giving the attribute size in
// bytes.
constexpr const char AttributeSizeMetadata[] = "lgc.rt.attribute.size";

// Pipeline-wide max attribute size module metadata, giving the maximum
// attribute size in bytes.
constexpr const char MaxAttributeSizeMetadata[] = "lgc.rt.max.attribute.size";

// Pipeline-wide max payload size module metadata, giving the maximum
// payload size in bytes.
constexpr const char MaxPayloadSizeMetadata[] = "lgc.rt.max.payload.size";

// ==============================================================================================
// Helper to create an MDNode containing a constant.
MDNode *getMdNodeForNumericConstant(LLVMContext &context, size_t value) {
  return MDNode::get(context, {ConstantAsMetadata::get(ConstantInt::get(
                                  Type::getInt32Ty(context), value))});
}

// ==============================================================================================
// Helper to create an MDNode containing a constant.
std::optional<size_t> extractNumericConstantFromMdNode(MDNode *node) {
  if (!node)
    return std::nullopt;
  assert(node->getNumOperands() == 1);
  if (auto *value = mdconst::dyn_extract<ConstantInt>(node->getOperand(0)))
    return value->getZExtValue();

  return std::nullopt;
}

// ==============================================================================================
// Wrapper around setMetadata for unsigned integer cases, global object/function
// version.
void setMetadataNumericValue(GlobalObject *func, StringRef Kind, size_t size) {
  func->setMetadata(Kind,
                    getMdNodeForNumericConstant(func->getContext(), size));
}

// ==============================================================================================
// Helper to obtain a constant from global object/function metadata.
std::optional<size_t> getMetadataNumericValue(const GlobalObject *obj,
                                              StringRef Kind) {
  MDNode *node = obj->getMetadata(Kind);
  return extractNumericConstantFromMdNode(node);
}

// ==============================================================================================
// Wrapper around setMetadata for unsigned integer cases, module version.
void setMetadataNumericValue(Module *module, StringRef Kind, size_t size) {
  auto *node = module->getOrInsertNamedMetadata(Kind);
  node->clearOperands();
  node->addOperand(getMdNodeForNumericConstant(module->getContext(), size));
}

// ==============================================================================================
// Helper to obtain a constant from a named metadata value.
std::optional<size_t> getMetadataNumericValue(const llvm::Module *module,
                                              StringRef Kind) {
  NamedMDNode *node = module->getNamedMetadata(Kind);
  if (!node)
    return std::nullopt;
  assert(node->getNumOperands() == 1);
  return extractNumericConstantFromMdNode(node->getOperand(0));
}

} // namespace

// ==============================================================================================
// Get the metadata IDs associated with the lgc.rt dialect, so the caller knows
// which ones can be removed when the dialect is processed.
void lgc::rt::getLgcRtMetadataIds(LLVMContext &context,
                                  SmallVectorImpl<unsigned> &ids) {
  ids.push_back(context.getMDKindID(ShaderStageMetadata));
  ids.push_back(context.getMDKindID(PaqMetadata));
  ids.push_back(context.getMDKindID(ArgSizeMetadata));
  ids.push_back(context.getMDKindID(AttributeSizeMetadata));
}

// ==============================================================================================
// Sets the given shader stage to a LLVM function. If std::nullopt is
// passed, then the shader stage metadata is removed from the function.
// func can instead be a GlobalVariable, allowing a front-end to use a
// GlobalVariable to represent a shader retrieved from the cache, and wants to
// mark it with a shader stage.
void lgc::rt::setLgcRtShaderStage(GlobalObject *func,
                                  std::optional<RayTracingShaderStage> stage) {
  if (stage.has_value())
    setMetadataNumericValue(func, ShaderStageMetadata,
                            static_cast<size_t>(stage.value()));
  else
    func->eraseMetadata(func->getContext().getMDKindID(ShaderStageMetadata));
}

// ==============================================================================================
// Get the lgc.rt shader stage from a given function. If there is no shader
// stage metadata apparent, then std::nullopt is returned.
// func can instead be a GlobalVariable, allowing a front-end to use a
// GlobalVariable to represent a shader retrieved from the cache, and wants to
// mark it with a shader stage.
std::optional<lgc::rt::RayTracingShaderStage>
lgc::rt::getLgcRtShaderStage(const GlobalObject *func) {
  std::optional<size_t> mdValue =
      getMetadataNumericValue(func, ShaderStageMetadata);
  if (mdValue.has_value()) {
    return RayTracingShaderStage(*mdValue);
  }
  return std::nullopt;
}

// ==============================================================================================
// Get PAQ (payload access qualifier) metadata for a ray-tracing shader
// function, or nullptr if none. We allow for the PAQ metadata not existing
// because the DXIL language reader sets it in its bitcode reader callback,
// without at that stage being able to check that it is correctly set on all
// appropriate shaders.
Constant *lgc::rt::getShaderPaq(Function *func) {
  MDNode *node = func->getMetadata(PaqMetadata);
  if (node)
    return mdconst::dyn_extract<Constant>(node->getOperand(0));
  return nullptr;
}

// ==============================================================================================
// Set PAQ (payload access qualifier) metadata for a ray-tracing shader
// function. The PAQ is a constant i32 array. For now, it has a single entry
// giving the size in bytes of the payload. For now, the PAQ is a single i32
// constant giving the size in bytes of the payload.
// TODO: Extend to an array of i32 constants specifying byte offset ranges with
// access bits, finishing with the size in bytes.
void lgc::rt::setShaderPaq(Function *func, Constant *paq) {
  func->setMetadata(PaqMetadata, MDNode::get(func->getContext(),
                                             {ConstantAsMetadata::get(paq)}));
}

// ==============================================================================================
// Get PAQ (payload access qualifier) from size in bytes, for the simple case
// that is the only information we have on the payload.
Constant *lgc::rt::getPaqFromSize(LLVMContext &context, size_t size) {
  Type *i32Ty = Type::getInt32Ty(context);
  return ConstantArray::get(ArrayType::get(i32Ty, 1),
                            ConstantInt::get(i32Ty, size));
}

// ==============================================================================================
// Get arg size (in bytes) metadata for a ray-tracing callable shader function.
// We don't allow for the metadata not existing -- that would cause an assert in
// this code. We assume that the language reader correctly called
// setShaderArgSize for any callable shader.
size_t lgc::rt::getShaderArgSize(Function *func) {
  std::optional<size_t> result = getMetadataNumericValue(func, ArgSizeMetadata);

  assert(result.has_value() &&
         "lgc::rt::getShaderArgSize: ArgSize metadata missing - forgot "
         "to call setShaderArgSize?");

  return result.value();
}

// ==============================================================================================
// Set arg size (in bytes) metadata for a ray-tracing callable shader function.
void lgc::rt::setShaderArgSize(Function *func, size_t size) {
  setMetadataNumericValue(func, ArgSizeMetadata, size);
}

// ==============================================================================================
// Get attribute size (in bytes) metadata for a ray-tracing shader function.
std::optional<size_t> lgc::rt::getShaderHitAttributeSize(const Function *func) {
  return getMetadataNumericValue(func, AttributeSizeMetadata);
}

// ==============================================================================================
// Set attribute size (in bytes) metadata for a ray-tracing shader function.
void lgc::rt::setShaderHitAttributeSize(Function *func, size_t size) {
  assert(getMaxHitAttributeSize(func->getParent()).value_or(size) >= size);
  setMetadataNumericValue(func, AttributeSizeMetadata, size);
}

// ==============================================================================================
// Get max hit attribute size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function hit attribute sizes.
std::optional<size_t>
lgc::rt::getMaxHitAttributeSize(const llvm::Module *module) {
  return getMetadataNumericValue(module, MaxAttributeSizeMetadata);
}

// ==============================================================================================
// Set max hit attribute size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function hit attribute sizes.
void lgc::rt::setMaxHitAttributeSize(llvm::Module *module, size_t size) {
  setMetadataNumericValue(module, MaxAttributeSizeMetadata, size);
}

// ==============================================================================================
// Get max payload size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function payload sizes.
std::optional<size_t> lgc::rt::getMaxPayloadSize(const llvm::Module *module) {
  return getMetadataNumericValue(module, MaxPayloadSizeMetadata);
}

// ==============================================================================================
// Set max hit attribute size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function payload sizes.
void lgc::rt::setMaxPayloadSize(llvm::Module *module, size_t size) {
  setMetadataNumericValue(module, MaxPayloadSizeMetadata, size);
}
