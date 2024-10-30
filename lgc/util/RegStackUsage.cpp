/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// Extraction, merging and inserting reg/stack usage in PAL metadata between different ELFs.
// A front-end can use this to propagate register and stack usage from library ELFs up to a compute
// shader ELF.

#include "lgc/RegStackUsage.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/util/MsgPackScanner.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ELFObjectFile.h"

#define DEBUG_TYPE "lgc-reg-stack-usage"

using namespace lgc;
using namespace llvm;

namespace {

// Item spec array of PAL metadata items that we are interested in, for MsgPackScanner to use.
// We call MsgPackScanner methods such as getAsInt() passing a reference to one of these items.
// clang-format off
static const struct {
  MsgPackScanner::Item top = {MsgPackScanner::ItemType::Map};
  MsgPackScanner::Item   pipelines = {MsgPackScanner::ItemType::Array, "amdpal.pipelines"};
  MsgPackScanner::Item     pipeline0 = {MsgPackScanner::ItemType::Map};
  MsgPackScanner::Item       hardwareStages = {MsgPackScanner::ItemType::Map, ".hardware_stages"};
  MsgPackScanner::Item         cs = {MsgPackScanner::ItemType::Map, ".cs"};
  MsgPackScanner::Item           csBackendStackSize = {MsgPackScanner::ItemType::Scalar, ".backend_stack_size"};
  MsgPackScanner::Item           csFrontendStackSize = {MsgPackScanner::ItemType::Scalar, ".frontend_stack_size"};
  MsgPackScanner::Item           csCpsGlobal = {MsgPackScanner::ItemType::Scalar, ".cps_global"};
  MsgPackScanner::Item           csScratchEn = {MsgPackScanner::ItemType::Scalar, ".scratch_en"};
  MsgPackScanner::Item           csScratchMemorySize = {MsgPackScanner::ItemType::Scalar, ".scratch_memory_size"};
  MsgPackScanner::Item           csLdsSize = {MsgPackScanner::ItemType::Scalar, ".lds_size"};
  MsgPackScanner::Item           csSgprCount = {MsgPackScanner::ItemType::Scalar, ".sgpr_count"};
  MsgPackScanner::Item           csVgprCount = {MsgPackScanner::ItemType::Scalar, ".vgpr_count"};
  MsgPackScanner::Item           csMemOrdered = {MsgPackScanner::ItemType::Scalar, ".mem_ordered"};
  MsgPackScanner::Item         endCs = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item       endHardwareStages = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item       shaderFunctions = {MsgPackScanner::ItemType::Map, ".shader_functions"};
  MsgPackScanner::Item         theFunc = {MsgPackScanner::ItemType::Map}; // No name, so matches all .shader_functions entries
  MsgPackScanner::Item           funcBackendStackSize = {MsgPackScanner::ItemType::Scalar, ".backend_stack_size"};
  MsgPackScanner::Item           funcFrontendStackSize = {MsgPackScanner::ItemType::Scalar, ".frontend_stack_size"};
  MsgPackScanner::Item           funcStackFrameSizeInBytes = {MsgPackScanner::ItemType::Scalar, ".stack_frame_size_in_bytes"};
  MsgPackScanner::Item           funcLdsSize = {MsgPackScanner::ItemType::Scalar, ".lds_size"};
  MsgPackScanner::Item           funcSgprCount = {MsgPackScanner::ItemType::Scalar, ".sgpr_count"};
  MsgPackScanner::Item           funcVgprCount = {MsgPackScanner::ItemType::Scalar, ".vgpr_count"};
  MsgPackScanner::Item         endTheFunc = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item       endShaderFunctions = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item       shaders = {MsgPackScanner::ItemType::Map, ".shaders"};
  MsgPackScanner::Item         compute = {MsgPackScanner::ItemType::Map, ".compute"};
  MsgPackScanner::Item           shaderSubtype = {MsgPackScanner::ItemType::Scalar, ".shader_subtype"};
  MsgPackScanner::Item         endCompute = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item       endShaders = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item     endPipeline0 = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item   endPipelines = {MsgPackScanner::ItemType::EndContainer};
  MsgPackScanner::Item endTop = {MsgPackScanner::ItemType::EndContainer};
} items;
// clang-format on

static const MsgPackScanner::Spec msgPackScannerSpec(&items);

// Struct for reg/stack usage.
struct Usage {
  unsigned maxRecursionDepth;
  unsigned callableShaderCount;
  unsigned backendStackSize;
  std::optional<unsigned> frontendStackSize;
  unsigned stackFrameSizeInBytes;
  unsigned scratchMemorySize;
  unsigned ldsSize;
  unsigned sgprCount;
  unsigned vgprCount;
  bool cpsGlobal;
  bool scratchEn;
  bool memOrdered;
};

// =====================================================================================================================
// Output Usage textually, for debug.
[[maybe_unused]] static raw_ostream &operator<<(raw_ostream &stream, const Usage &usage) {
  stream << "  maxRecursionDepth " << usage.maxRecursionDepth << "\n"
         << "  callableShaderCount " << usage.callableShaderCount << "\n"
         << "  backendStackSize " << usage.backendStackSize << "\n"
         << "  frontendStackSize " << usage.frontendStackSize << "\n"
         << "  stackFrameSizeInBytes " << usage.stackFrameSizeInBytes << "\n"
         << "  scratchMemorySize " << usage.scratchMemorySize << "\n"
         << "  ldsSize " << usage.ldsSize << "\n"
         << "  sgprCount " << usage.sgprCount << "\n"
         << "  vgprCount " << usage.vgprCount << "\n"
         << "  cpsGlobal " << usage.cpsGlobal << "\n"
         << "  scratchEn " << usage.scratchEn << "\n"
         << "  memOrdered " << usage.memOrdered << "\n";
  return stream;
}

} // anonymous namespace

namespace lgc {

// Class to parse reg/stack usage from PAL metadata and merge it back.
class RegStackUsageImpl {
public:
  // Constructor, setting up MsgPackScanner.
  RegStackUsageImpl() : m_msgPackScanner(msgPackScannerSpec) {}

  // RegStackUsage methods that get forwarded to this class.
  ~RegStackUsageImpl() = default;
  RegStackUsageImpl(StringRef elfBlob, unsigned maxRecursionDepth, uint64_t rayGenUsage);
  RegStackUsageImpl(const Module &module);
  void writeMetadata(Module &module) const;
  void merge(const RegStackUsageImpl &shaderUsage);
  void finalizeAndUpdate(SmallVectorImpl<char> &elfBuffer, size_t startOffset, unsigned frontendGlobalAlignment);

private:
  // Construct from PAL metadata blob. This is only used internally for the "Re-scan the new blob to check it" code.
  RegStackUsageImpl(StringRef palMetadata);

  // Set up m_usage values by scanning PAL metadata blob.
  void scanPalMetadata();

  // Finalize usage before writing back in to the launch kernel.
  void finalize(unsigned frontendGlobalAlignment);

  // Update the ELF with supplied usage info, and rewrite the ELF. This could make the ELF a different size.
  void updateAndWrite(const Usage &usage, SmallVectorImpl<char> &elfBuffer, size_t startOffset);

  // Replace some section data in an ELF.
  void replaceElfData(object::ObjectFile &elf, SmallVectorImpl<char> &elfBuffer, size_t startOffset, size_t dataOffset,
                      size_t oldDataSize, StringRef newData);

  MsgPackScanner m_msgPackScanner;
  Usage m_usage = {};
  StringRef m_elfBlob;
  std::unique_ptr<object::ObjectFile> m_elf;
  unsigned m_noteAlign = 0;
  size_t m_palMetadataNoteOffset = 0;
  StringRef m_palMetadata;
#ifndef NDEBUG
  bool m_finalized = false;
#endif
};

} // namespace lgc

// Metadata name for reg/stack usage. All code that reads and writes it is in this source file.
static const char RegStackUsageMetadataName[] = "lgc.reg.stack.usage";

// =====================================================================================================================
// Forwarding methods from RegStackUsage to RegStackUsageImpl
RegStackUsage::~RegStackUsage() = default;

RegStackUsage::RegStackUsage() : m_impl(std::make_unique<RegStackUsageImpl>()) {
}

RegStackUsage::RegStackUsage(StringRef elfBlob, unsigned maxRecursionDepth, uint64_t rayGenUsage)
    : m_impl(std::make_unique<RegStackUsageImpl>(elfBlob, maxRecursionDepth, rayGenUsage)) {
}

RegStackUsage::RegStackUsage(const Module &module) : m_impl(std::make_unique<RegStackUsageImpl>(module)) {
}

void RegStackUsage::writeMetadata(Module &module) const {
  m_impl->writeMetadata(module);
}

void RegStackUsage::merge(const RegStackUsage &shaderUsage) {
  m_impl->merge(*shaderUsage.m_impl);
}

void RegStackUsage::finalizeAndUpdate(SmallVectorImpl<char> &elfBuffer, size_t startOffset,
                                      unsigned frontendGlobalAlignment) {
  m_impl->finalizeAndUpdate(elfBuffer, startOffset, frontendGlobalAlignment);
}

// =====================================================================================================================
// Construct from ELF blob. This reads the reg/stack usage from the ELF's PAL metadata.
// This is passed rayGenUsage to allow for a future enhancement where frontend stack size is calculated in a
// more sophisticated way that takes into account which shaders are reachable from which rayGens.
//
// @param elfBlob : The ELF blob; must remain valid for the lifetime of the RegStackUsage object
// @param maxRecursionDepth : Max recursion depth for this shader as specified by the app; 0 for traversal
// @param rayGenUsage : bitmap of which rayGens can reach this shader, with bit 63 covering all rayGens
//                      beyond the first 63; 0 for traversal
//
RegStackUsageImpl::RegStackUsageImpl(StringRef elfBlob, unsigned maxRecursionDepth, uint64_t rayGenUsage)
    : m_msgPackScanner(msgPackScannerSpec), m_elfBlob(elfBlob) {
  m_usage.maxRecursionDepth = maxRecursionDepth;

  m_elf = cantFail(object::ObjectFile::createELFObjectFile(MemoryBufferRef(elfBlob, "")));
  for (const object::SectionRef &section : m_elf->sections()) {
    object::ELFSectionRef elfSection(section);
    if (elfSection.getType() == ELF::SHT_NOTE) {
      // This is a .note section. Find the PAL metadata note.
      Error err = ErrorSuccess();
      auto &elfFile = cast<object::ELFObjectFile<object::ELF64LE>>(&*m_elf)->getELFFile();
      auto shdr = cantFail(elfFile.getSection(elfSection.getIndex()));
      for (auto note : elfFile.notes(*shdr, err)) {
        if (note.getName() == Util::Abi::AmdGpuArchName && note.getType() == ELF::NT_AMDGPU_METADATA) {
          // Found the PAL metadata note record. Remember its position (in a sneaky way to get around Elf_Note_Impl
          // hiding some details).
          m_palMetadataNoteOffset = note.getName().data() - m_elfBlob.data() -
                                    sizeof(object::Elf_Nhdr_Impl<object::ELFType<llvm::endianness::little, true>>);
          m_noteAlign = shdr->sh_addralign;
          ArrayRef<uint8_t> desc = note.getDesc(m_noteAlign);
          // Scan the PAL metadata.
          m_palMetadata = StringRef(reinterpret_cast<const char *>(desc.data()), desc.size());
          scanPalMetadata();
          break;
        }
      }
      if (err)
        report_fatal_error("Bad PAL metadata format");
      break;
    }
  }
}

// =====================================================================================================================
// Construct from PAL metadata blob. This is only used internally for the "Re-scan the new blob to check it" code.
RegStackUsageImpl::RegStackUsageImpl(StringRef palMetadata)
    : m_msgPackScanner(msgPackScannerSpec), m_palMetadata(palMetadata) {
  scanPalMetadata();
}

// =====================================================================================================================
// Set up m_usage values by scanning PAL metadata blob.
void RegStackUsageImpl::scanPalMetadata() {
  // Callback function to handle an item being found by MsgPackScanner.
  auto foundItemCallback = [this](MsgPackScanner &msgPackScanner, const MsgPackScanner::Item &item) {
    // For backend stack usage (scratch used within a func in continuations) and frontend stack usage (CPS stack),
    // take the maximum of multiple modules.
    if (&item == &items.csBackendStackSize || &item == &items.funcBackendStackSize)
      m_usage.backendStackSize = std::max(m_usage.backendStackSize, unsigned(msgPackScanner.asInt(item).value_or(0)));
    else if (&item == &items.csFrontendStackSize || &item == &items.funcFrontendStackSize) {
      m_usage.frontendStackSize =
          std::max(m_usage.frontendStackSize.value_or(0), unsigned(msgPackScanner.asInt(item).value_or(0)));
    }
    // For other stack m_usage, sum multiple functions.
    else if (&item == &items.funcStackFrameSizeInBytes)
      m_usage.stackFrameSizeInBytes += msgPackScanner.asInt(item).value_or(0);
    // For LDS and register m_usage, take the maximum of multiple functions.
    else if (&item == &items.csLdsSize || &item == &items.funcLdsSize)
      m_usage.ldsSize = std::max(m_usage.ldsSize, unsigned(msgPackScanner.asInt(item).value_or(0)));
    else if (&item == &items.csSgprCount || &item == &items.funcSgprCount)
      m_usage.sgprCount = std::max(m_usage.sgprCount, unsigned(msgPackScanner.asInt(item).value_or(0)));
    else if (&item == &items.csVgprCount || &item == &items.funcVgprCount)
      m_usage.vgprCount = std::max(m_usage.vgprCount, unsigned(msgPackScanner.asInt(item).value_or(0)));
    else if (&item == &items.csMemOrdered)
      m_usage.memOrdered = msgPackScanner.asBool(item).value_or(false);
    // scratchEn and scratchMemorySize are read solely for the "Re-scan the new blob" check (in updateAndWrite)
    // to work.
    else if (&item == &items.csScratchEn)
      m_usage.scratchEn = msgPackScanner.asBool(item).value_or(false);
    else if (&item == &items.csScratchMemorySize)
      m_usage.scratchMemorySize = msgPackScanner.asInt(item).value_or(0);
    else if (&item == &items.shaderSubtype && msgPackScanner.asString(item) == "Callable")
      ++m_usage.callableShaderCount;
    return Error::success();
  };

  Error err = m_msgPackScanner.scan(m_palMetadata, foundItemCallback);
  if (err)
    report_fatal_error("Bad PAL metadata format");

  LLVM_DEBUG(dbgs() << "Usage:\n" << m_usage);
}

// =====================================================================================================================
// Construct from Module. This reads the reg/stack usage from IR metadata, as written by writeMetadata().
RegStackUsageImpl::RegStackUsageImpl(const llvm::Module &module) : m_msgPackScanner(msgPackScannerSpec) {
  NamedMDNode *namedNode = module.getNamedMetadata(RegStackUsageMetadataName);
  if (namedNode && namedNode->getNumOperands() >= 1) {
    StringRef str = dyn_cast<MDString>(namedNode->getOperand(0)->getOperand(0))->getString();
    assert(str.size() == sizeof(m_usage));
    memcpy(&m_usage, str.data(), sizeof(m_usage));
  }
}

// =====================================================================================================================
// Write the reg/stack usage into IR metadata.
void RegStackUsageImpl::writeMetadata(Module &module) const {
  NamedMDNode *namedNode = module.getOrInsertNamedMetadata(RegStackUsageMetadataName);
  namedNode->clearOperands();
  namedNode->addOperand(MDNode::get(
      module.getContext(),
      MDString::get(module.getContext(), StringRef(reinterpret_cast<const char *>(&m_usage), sizeof(m_usage)))));
}

// =====================================================================================================================
// Merge reg/stack usage from one shader ELF into the accumulated merged usage in "this".
void RegStackUsageImpl::merge(const RegStackUsageImpl &shaderUsage) {
  assert(!m_finalized && "Cannot merge after finalizing");
  m_usage.maxRecursionDepth = std::max(m_usage.maxRecursionDepth, shaderUsage.m_usage.maxRecursionDepth);
  // For backend stack usage (scratch used within a func in continuations) and frontend stack usage (CPS stack),
  // take the maximum of multiple modules.
  m_usage.backendStackSize = std::max(m_usage.backendStackSize, shaderUsage.m_usage.backendStackSize);
  if (m_usage.frontendStackSize || shaderUsage.m_usage.frontendStackSize) {
    m_usage.frontendStackSize =
        std::max(m_usage.frontendStackSize.value_or(0), shaderUsage.m_usage.frontendStackSize.value_or(0));
  }
  // For other stack usage, sum multiple modules.
  m_usage.stackFrameSizeInBytes = m_usage.stackFrameSizeInBytes + shaderUsage.m_usage.stackFrameSizeInBytes;
  // For reg/stack usage, take the maximum of multiple modules.
  m_usage.ldsSize = std::max(m_usage.ldsSize, shaderUsage.m_usage.ldsSize);
  m_usage.sgprCount = std::max(m_usage.sgprCount, shaderUsage.m_usage.sgprCount);
  m_usage.vgprCount = std::max(m_usage.vgprCount, shaderUsage.m_usage.vgprCount);
  m_usage.memOrdered = std::max(m_usage.memOrdered, shaderUsage.m_usage.memOrdered);

  m_usage.callableShaderCount += shaderUsage.m_usage.callableShaderCount;
}

// =====================================================================================================================
// Finalize merged usage in "this" (that comes from indirect shaders), merge into the supplied ELF's usage,
// and update the PAL metadata in the ELF.
//
// @param (in/out) elfBuffer : Buffer containing ELF to read and update
// @param startOffset : Start offset of the ELF in the buffer
// @param frontendGlobalAlignment : Alignment of frontend stack for global CPS; 0 for scratch CPS
//
void RegStackUsageImpl::finalizeAndUpdate(SmallVectorImpl<char> &elfBuffer, size_t startOffset,
                                          unsigned frontendGlobalAlignment) {
  // Create a RegStackUsage for the ELF.
  RegStackUsageImpl elfUsage(StringRef(&elfBuffer[startOffset], elfBuffer.size() - startOffset), 0, 0);
  // Merge its usage into ours.
  merge(elfUsage);
  // Finalize the usage.
  finalize(frontendGlobalAlignment);
  // Update usage in the ELF and rewrite it.
  elfUsage.updateAndWrite(m_usage, elfBuffer, startOffset);
}

// =====================================================================================================================
// Finalize usage before writing back in to the launch kernel.
void RegStackUsageImpl::finalize(unsigned frontendGlobalAlignment) {
  assert(!m_finalized && "Cannot finalize twice");
#ifndef NDEBUG
  m_finalized = true;
#endif
  if (m_usage.frontendStackSize) {
    // Continuations support.
    // Currently this uses a universal whole-pipeline frontendCallDepth and multiplies it in to frontendStackSize.
    // The calculation could be made more sophisticated by:
    // - taking each shader's stage into account when deciding what to multiply by;
    // - calculating separately for each rayGen and its reachable shaders, then taking the max result.
    // The shader stage is available in PAL metadata (already used to detect callable shaders), and the rayGen
    // usage bitmap is passed in to RegStackUsage so it can be used this way in the future.
    m_usage.scratchMemorySize = m_usage.backendStackSize;
    // Get frontend call depth from the max recursion depth seen for any shader.
    unsigned frontendCallDepth = m_usage.maxRecursionDepth;
    // If we have any callable shaders, add on an extra 2, the arbitrary API limit for callable shaders if the
    // app does not set its own stack depth.
    if (m_usage.callableShaderCount != 0)
      frontendCallDepth += 2;
    // Add on an extra 1 to cover these cases, which all happen separately at the leaf level:
    // - At leaf level (we are not allowed to recurse), there might still be a non-reached conditional suspend
    //   point, and the existence of this suspend point even if not reached causes potential stack usage.
    // - The same applies to non-reached CallShader calls even if there no callable shaders.
    // - Traversal and Intersection shaders also require the +1, as their usage is not reflected in the recursion
    //   limit.
    ++frontendCallDepth;

    // Multiply frontendStackSize by the call depth.
    m_usage.frontendStackSize = m_usage.frontendStackSize.value() * frontendCallDepth;
    if (frontendGlobalAlignment == 0) {
      // CPS stack ("frontend" stack) is allocated as a chunk out of scratch. We need to add its size on to
      // scratchMemorySize.
      m_usage.scratchMemorySize += m_usage.frontendStackSize.value();
    } else {
      // CPS stack ("global" stack) is allocated as global. We need to bump it to the specified alignment.
      m_usage.frontendStackSize = alignToPowerOf2(m_usage.frontendStackSize.value(), frontendGlobalAlignment);
    }
  } else {
    // Not continuations. Assume no recursion; we do not have any information on what the recursion depth could be.
    // scratchMemorySize is the compute shader stack usage; stackFrameSizeInBytes is the sum of the stack usage of
    // functions.
    m_usage.scratchMemorySize += m_usage.stackFrameSizeInBytes;
  }
  m_usage.scratchEn = m_usage.scratchMemorySize != 0;

  LLVM_DEBUG(dbgs() << "Finalized usage:\n" << m_usage);
}

// =====================================================================================================================
// Update the ELF with supplied usage info, and rewrite the ELF. This could make the ELF a different size.
//
// @param (in/out) elfBuffer : Buffer containing ELF to update; must be the same ELF at the same location in
//                             memory that was originally scanned by this RegStackUsage
// @param startOffset : Start offset of the ELF in the buffer
//
void RegStackUsageImpl::updateAndWrite(const Usage &usage, SmallVectorImpl<char> &elfBuffer, size_t startOffset) {
  if (usage.frontendStackSize) {
    // Set backendStackSize even if 0, otherwise PAL gives the driver a junk value.
    m_msgPackScanner.set(items.csBackendStackSize, usage.backendStackSize);
    m_msgPackScanner.set(items.csFrontendStackSize, usage.frontendStackSize.value());
  }
  if (usage.scratchEn)
    m_msgPackScanner.setBool(items.csScratchEn, usage.scratchEn);
  if (usage.scratchMemorySize)
    m_msgPackScanner.set(items.csScratchMemorySize, usage.scratchMemorySize);
  if (usage.ldsSize)
    m_msgPackScanner.set(items.csLdsSize, usage.ldsSize);
  if (usage.sgprCount)
    m_msgPackScanner.set(items.csSgprCount, usage.sgprCount);
  if (usage.vgprCount)
    m_msgPackScanner.set(items.csVgprCount, usage.vgprCount);
  if (usage.memOrdered)
    m_msgPackScanner.setBool(items.csMemOrdered, usage.memOrdered);

  // Get MsgPackScanner to write the updated PAL metadata.
  // We cannot write it directly into elfBuffer, overwriting the original ELF, because MsgPackScanner::write
  // reads the unmodified parts of PAL metadata from there.
  SmallString<0> newPalMetadata;
  raw_svector_ostream stream(newPalMetadata);
  m_msgPackScanner.write(stream);

#ifndef NDEBUG
  // Re-scan the new blob to check it.
  // Tolerate usage.scratchEn false but newUsage.m_usage.scratchEn true as LGC seems to always set it true.
  // Tolerate backendStackSize disagreeing if frontendStack size is 0, as we do not bother to set the former.
  LLVM_DEBUG(dbgs() << "\nRescan the new blob\n");
  RegStackUsageImpl newUsage(newPalMetadata);
  assert((usage.frontendStackSize.value_or(0) == 0 || usage.backendStackSize == newUsage.m_usage.backendStackSize) &&
         usage.frontendStackSize.value_or(0) == newUsage.m_usage.frontendStackSize.value_or(0) &&
         usage.scratchEn <= newUsage.m_usage.scratchEn &&
         usage.scratchMemorySize == newUsage.m_usage.scratchMemorySize && usage.ldsSize == newUsage.m_usage.ldsSize &&
         usage.sgprCount == newUsage.m_usage.sgprCount && usage.vgprCount == newUsage.m_usage.vgprCount &&
         usage.memOrdered == newUsage.m_usage.memOrdered);
#endif

  // Align size of both old and new PAL metadata. Pad the new PAL metadata appropriately.
  size_t alignedOldPalMetadataSize = alignToPowerOf2(m_palMetadata.size(), m_noteAlign);
  size_t newPalMetadataSize = newPalMetadata.size(); // Size before aligning
  newPalMetadata.append(alignToPowerOf2(newPalMetadataSize, m_noteAlign) - newPalMetadataSize, '\0'); // Align it

  // Write the new size into the .note record header that is just before the PAL metadata.
  auto noteHeader = reinterpret_cast<object::Elf_Nhdr_Impl<object::ELFType<llvm::endianness::little, true>> *>(
      &elfBuffer[startOffset + m_palMetadataNoteOffset]);
  noteHeader->n_descsz = newPalMetadataSize;

  // Resize and overwrite the PAL metadata blob in the ELF.
  size_t palMetadataOffset = m_palMetadata.data() - m_elfBlob.data();
  replaceElfData(*m_elf, elfBuffer, startOffset, palMetadataOffset, alignedOldPalMetadataSize, newPalMetadata);
  m_elf = {};
}

// =====================================================================================================================
// Replace some section data in an ELF.
// Special cases of this are deleting some data (newData has 0 size) and inserting some data (oldDataSize is 0).
// This expands or contracts the buffer as necessary, and changes the size of the section containing the change,
// and the file offset of all sections after the change. It does not update the object::ObjectFile, which thus
// becomes invalid.
//
// @param elf : ELF file object
// @param (in/out) elfBuffer : Writable buffer containing ELF, possibly with some prefix
// @param startOffset : Size of prefix in the buffer before we get to the ELF
// @param dataOffset : Offset of data to remove within the ELF, and where to insert new data
// @param oldDataSize : Size of old data to remove
// @param newData : New data to insert in its place
//
void RegStackUsageImpl::replaceElfData(object::ObjectFile &elf, SmallVectorImpl<char> &elfBuffer, size_t startOffset,
                                       size_t dataOffset, size_t oldDataSize, StringRef newData) {
  ssize_t sizeDelta = newData.size() - oldDataSize;
  char *elfPtr = &elfBuffer[startOffset];
  if (sizeDelta != 0) {
    assert((sizeDelta & 3) == 0 && "Change would upset file alignment of things after it");

    // Iterate through sections to modify headers.
    for (const object::SectionRef &section : m_elf->sections()) {
      object::ELFSectionRef elfSection(section);
      auto sectHeader =
          reinterpret_cast<object::ELFType<endianness::little, true>::Shdr *>(elfSection.getRawDataRefImpl().p);
      StringRef contents = cantFail(elfSection.getContents());
      if (contents.begin() - elfPtr <= dataOffset && contents.end() - elfPtr > dataOffset) {
        // This section contains the data being replaced. Change its size.
        sectHeader->sh_size += sizeDelta;
      } else if (contents.begin() - elfPtr > dataOffset) {
        // This section is after the data being replaced. Change its file offset.
        sectHeader->sh_offset += sizeDelta;
      }
    }

    // Modify offsets in ELF header.
    auto elfHeader = reinterpret_cast<object::ELFType<endianness::little, true>::Ehdr *>(&elfBuffer[startOffset]);
    assert(elfHeader->e_phoff == 0 && "Executable ELF not supported");
    if (elfHeader->e_shoff > dataOffset)
      elfHeader->e_shoff += sizeDelta;

    // Resize the ELF appropriately.
    size_t oldElfSize = elfBuffer.size() - startOffset;
    if (sizeDelta > 0) {
      elfBuffer.resize(elfBuffer.size() + sizeDelta);
      elfPtr = &elfBuffer[startOffset];
    }
    memmove(elfPtr + dataOffset + newData.size(), elfPtr + dataOffset + oldDataSize,
            oldElfSize - (dataOffset + oldDataSize));
    if (sizeDelta < 0) {
      elfBuffer.resize(elfBuffer.size() + sizeDelta);
      elfPtr = &elfBuffer[startOffset];
    }
  }

  // Write the new data.
  memcpy(elfPtr + dataOffset, newData.data(), newData.size());
}
