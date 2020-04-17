/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcElfWriter.cpp
 * @brief LLPC source file: contains implementation of LLPC ELF writing utilities.
 ***********************************************************************************************************************
 */
#include "llpcElfWriter.h"
#include "../../lgc/imported/chip/gfx9/gfx9_plus_merged_offset.h"
#include "../../lgc/imported/chip/gfx9/gfx9_plus_merged_registers.h"
#include "llpcContext.h"
#include "lgc/LgcContext.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include <algorithm>
#include <string.h>

#define DEBUG_TYPE "llpc-elf-writer"

using namespace llvm;
using namespace Vkgc;

namespace {

// R_AMDGPU_ABS32 is only used in asserts, so it is unused in release builds, which does not work well with -Werror.
// Therefore, this is a define instead of a constexpr.
#define R_AMDGPU_ABS32 6
// Descriptor size
static constexpr unsigned DescriptorSizeResource = 8 * sizeof(unsigned);
static constexpr unsigned DescriptorSizeSampler = 4 * sizeof(unsigned);

// Represent a relocation entry. Used internally for pipeline linking.
struct RelocationEntry {
  Vkgc::ElfReloc reloc; // The relocation entry from the ELF
  const char *name;     // Name of the symbol associated with the relocation
};

} // namespace

namespace Llpc {
// The names of API shader stages used in PAL metadata, in ShaderStage order.
static const char *const ApiStageNames[] = {".vertex", ".hull", ".domain", ".geometry", ".pixel", ".compute"};

// The names of hardware shader stages used in PAL metadata, in Util::Abi::HardwareStage order.
static const char *const HwStageNames[] = {".ls", ".hs", ".es", ".gs", ".vs", ".ps", ".cs"};

// =====================================================================================================================
//
// @param gfxIp : Graphics IP version info
template <class Elf>
ElfWriter<Elf>::ElfWriter(GfxIpVersion gfxIp)
    : m_gfxIp(gfxIp), m_textSecIdx(InvalidValue), m_noteSecIdx(InvalidValue), m_relocSecIdx(InvalidValue),
      m_symSecIdx(InvalidValue), m_strtabSecIdx(InvalidValue) {
}

// =====================================================================================================================
template <class Elf> ElfWriter<Elf>::~ElfWriter() {
  for (auto &section : m_sections)
    delete[] section.data;
  m_sections.clear();

  for (auto &note : m_notes)
    delete[] note.data;
  m_notes.clear();

  for (auto &sym : m_symbols) {
    if (sym.nameOffset == InvalidValue)
      delete[] sym.pSymName;
  }
  m_symbols.clear();
}

// =====================================================================================================================
// Merge base section and input section into merged section
//
// @param pSection1 : The first section buffer to merge
// @param section1Size : Byte size of the first section
// @param pPrefixString1 : Prefix string of the first section's contents
// @param pSection2 : The second section buffer to merge
// @param section2Offset : Byte offset of the second section
// @param pPrefixString2 : Prefix string of the second section's contents
// @param [out] pNewSection : Merged section
template <class Elf>
void ElfWriter<Elf>::mergeSection(const SectionBuffer *pSection1, size_t section1Size, const char *pPrefixString1,
                                  const SectionBuffer *pSection2, size_t section2Offset, const char *pPrefixString2,
                                  SectionBuffer *pNewSection) {
  std::string prefix1;
  std::string prefix2;

  // Build prefix1 if it is needed
  if (pPrefixString1) {
    if (strncmp(reinterpret_cast<const char *>(pSection1->data), pPrefixString1, strlen(pPrefixString1)) != 0) {
      prefix1 = pPrefixString1;
      prefix1 += ":\n";
    }
  }

  // Build appendPrefixString if it is needed
  if (pPrefixString2) {
    if (strncmp(reinterpret_cast<const char *>(pSection2->data + section2Offset), pPrefixString2,
                strlen(pPrefixString2)) != 0) {
      prefix2 = pPrefixString2;
      prefix2 += ":\n";
    }
  }

  // Build merged section
  size_t newSectionSize =
      section1Size + (pSection2->secHead.sh_size - section2Offset) + prefix1.length() + prefix2.length();

  auto mergedData = new uint8_t[newSectionSize];
  *pNewSection = *pSection1;
  pNewSection->data = mergedData;
  pNewSection->secHead.sh_size = newSectionSize;

  auto data = mergedData;

  // Copy prefix1
  if (prefix1.length() > 0) {
    memcpy(data, prefix1.data(), prefix1.length());
    data += prefix1.length();
  }

  // Copy base section content
  auto baseCopySize = std::min(section1Size, static_cast<size_t>(pSection1->secHead.sh_size));
  memcpy(data, pSection1->data, baseCopySize);
  data += baseCopySize;

  // Fill alignment data with NOP instruction to match backend's behavior
  if (baseCopySize < section1Size) {
    // NOTE: All disassemble section don't have any alignmeent requirement, so it happen only if we merge
    // .text section.
    constexpr unsigned nop = 0xBF800000;
    unsigned *dataDw = reinterpret_cast<unsigned *>(data);
    for (unsigned i = 0; i < (section1Size - baseCopySize) / sizeof(unsigned); ++i)
      dataDw[i] = nop;
    data += (section1Size - baseCopySize);
  }

  // Copy append prefix
  if (prefix2.length() > 0) {
    memcpy(data, prefix2.data(), prefix2.length());
    data += prefix2.length();
  }

  // Copy append section content
  memcpy(data, pSection2->data + section2Offset, pSection2->secHead.sh_size - section2Offset);
}

// =====================================================================================================================
// A woarkaround to support erase in llvm::msgpack::MapDocNode.
class MapDocNode : public llvm::msgpack::MapDocNode {
public:
  MapTy::iterator erase(MapTy::iterator where) { return Map->erase(where); }
};

// =====================================================================================================================
// Merges the map item pair from source map to destination map for llvm::msgpack::MapDocNode.
//
// @param [in,out] destMap : Destination map
// @param srcMap : Source map
// @param key : Key to check in source map
template <class Elf>
void ElfWriter<Elf>::mergeMapItem(llvm::msgpack::MapDocNode &destMap, llvm::msgpack::MapDocNode &srcMap, unsigned key) {
  auto srcKeyNode = srcMap.getDocument()->getNode(key);
  auto srcIt = srcMap.find(srcKeyNode);
  if (srcIt != srcMap.end()) {
    assert(srcIt->first.getUInt() == key);
    destMap[destMap.getDocument()->getNode(key)] = srcIt->second;
  } else {
    auto destKeyNode = destMap.getDocument()->getNode(key);
    auto destIt = destMap.find(destKeyNode);
    if (destIt != destMap.end()) {
      assert(destIt->first.getUInt() == key);
      static_cast<MapDocNode &>(destMap).erase(destIt);
    }
  }
}

// =====================================================================================================================
// Update descriptor offset to USER_DATA in metaNote, in place in the messagepack document.
//
// @param context : context related to ElfNote
// @param document : [in, out] The parsed message pack document of the metadata note.
static void updateRootDescriptorRegisters(Context *context, msgpack::Document &document) {
  auto pipeline = document.getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0];
  auto registers = pipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Registers].getMap(true);
  const unsigned mmSpiShaderUserDataVs0 = 0x2C4C;
  const unsigned mmSpiShaderUserDataPs0 = 0x2c0c;
  const unsigned mmComputeUserData0 = 0x2E40;
  unsigned userDataBaseRegisters[] = {mmSpiShaderUserDataVs0, mmSpiShaderUserDataPs0, mmComputeUserData0};
  const unsigned vsPsUserDataCount = context->getGfxIpVersion().major < 9 ? 16 : 32;
  unsigned userDataCount[] = {vsPsUserDataCount, vsPsUserDataCount, 16};
  for (auto stage = 0; stage < sizeof(userDataBaseRegisters) / sizeof(unsigned); ++stage) {
    unsigned baseRegister = userDataBaseRegisters[stage];
    unsigned registerCount = userDataCount[stage];
    for (unsigned i = 0; i < registerCount; ++i) {
      unsigned key = baseRegister + i;
      auto keyNode = registers.getDocument()->getNode(key);
      auto keyIt = registers.find(keyNode);
      if (keyIt != registers.end()) {
        assert(keyIt->first.getUInt() == key);
        // Reloc Descriptor user data value is consisted by DescRelocMagic | set.
        unsigned regValue = keyIt->second.getUInt();
        if (DescRelocMagic == (regValue & DescRelocMagicMask)) {
          const PipelineShaderInfo *shaderInfo = nullptr;
          if (baseRegister == mmComputeUserData0) {
            auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
            shaderInfo = &pipelineInfo->cs;
          } else {
            auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
            shaderInfo = baseRegister == mmSpiShaderUserDataVs0 ? &pipelineInfo->vs : &pipelineInfo->fs;
          }
          unsigned set = regValue & DescSetMask;
          for (unsigned j = 0; j < shaderInfo->userDataNodeCount; ++j) {
            if (shaderInfo->pUserDataNodes[j].type == ResourceMappingNodeType::DescriptorTableVaPtr &&
                set == shaderInfo->pUserDataNodes[j].tablePtr.pNext[0].srdRange.set) {
              // If it's descriptor user data, then update its offset to it.
              unsigned value = shaderInfo->pUserDataNodes[j].offsetInDwords;
              keyIt->second = registers.getDocument()->getNode(value);
              // Update userDataLimit if neccessary
              unsigned userDataLimit = pipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit].getUInt();
              pipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit] =
                  document.getNode(std::max(userDataLimit, value + 1));
              break;
            }
          }
        }
      }
    }
  }
}

// =====================================================================================================================
// Merges fragment shader related info for meta notes.
//
// @param pContext : The first note section to merge
// @param pNote1 : The second note section to merge (contain fragment shader info)
// @param pNote2 : Note section contains fragment shader info
// @param [out] pNewNote : Merged note section
template <class Elf>
void ElfWriter<Elf>::mergeMetaNote(Context *pContext, const ElfNote *pNote1, const ElfNote *pNote2, ElfNote *pNewNote) {
  msgpack::Document destDocument;
  msgpack::Document srcDocument;

  auto success =
      destDocument.readFromBlob(StringRef(reinterpret_cast<const char *>(pNote1->data), pNote1->hdr.descSize), false);
  assert(success);

  success =
      srcDocument.readFromBlob(StringRef(reinterpret_cast<const char *>(pNote2->data), pNote2->hdr.descSize), false);
  assert(success);
  (void(success)); // unused

  auto destPipeline =
      destDocument.getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0];
  auto srcPipeline =
      srcDocument.getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0];

  // Copy .num_interpolants
  auto srcNumIterpIt = srcPipeline.getMap(true).find(StringRef(Util::Abi::PipelineMetadataKey::NumInterpolants));
  if (srcNumIterpIt != srcPipeline.getMap(true).end())
    destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::NumInterpolants] = srcNumIterpIt->second;

  // Copy .spill_threshold
  auto destSpillThreshold = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::SpillThreshold].getUInt();
  auto srcSpillThreshold = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::SpillThreshold].getUInt();
  destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::SpillThreshold] =
      destDocument.getNode(std::min(srcSpillThreshold, destSpillThreshold));

  // Copy .user_data_limit
  auto destUserDataLimit = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit].getUInt();
  auto srcUserDataLimit = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit].getUInt();
  destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit] =
      destDocument.getNode(std::max(destUserDataLimit, srcUserDataLimit));

  // Copy whole .ps hw stage
  auto destHwStages = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::HardwareStages].getMap(true);
  auto srcHwStages = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::HardwareStages].getMap(true);
  auto hwPsStageName = HwStageNames[static_cast<unsigned>(Util::Abi::HardwareStage::Ps)];
  destHwStages[hwPsStageName] = srcHwStages[hwPsStageName];

  // Copy whole .pixel shader
  auto destShaders = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Shaders].getMap(true);
  auto srcShaders = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Shaders].getMap(true);
  destShaders[ApiStageNames[ShaderStageFragment]] = srcShaders[ApiStageNames[ShaderStageFragment]];

  // Update pipeline hash
  auto pipelineHash = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
  pipelineHash[0] = destDocument.getNode(pContext->getPiplineHashCode());
  pipelineHash[1] = destDocument.getNode(pContext->getPiplineHashCode());

  // List of fragment shader related registers.
  static const unsigned PsRegNumbers[] = {
      0x2C0A, // mmSPI_SHADER_PGM_RSRC1_PS
      0x2C0B, // mmSPI_SHADER_PGM_RSRC2_PS
      0xA1C4, // mmSPI_SHADER_Z_FORMAT
      0xA1C5, // mmSPI_SHADER_COL_FORMAT
      0xA1B8, // mmSPI_BARYC_CNTL
      0xA1B6, // mmSPI_PS_IN_CONTROL
      0xA1B3, // mmSPI_PS_INPUT_ENA
      0xA1B4, // mmSPI_PS_INPUT_ADDR
      0xA1B5, // mmSPI_INTERP_CONTROL_0
      0xA293, // mmPA_SC_MODE_CNTL_1
      0xA203, // mmDB_SHADER_CONTROL
      0xA08F, // mmCB_SHADER_MASK
      0xA2F8, // mmPA_SC_AA_CONFIG
      // The following ones are GFX9+ only, but we don't need to handle them specially as those register
      // numbers are not used at all on earlier chips.
      0xA310, // mmPA_SC_SHADER_CONTROL
      0xA210, // mmPA_STEREO_CNTL
      0xC25F, // mmGE_STEREO_CNTL
      0xC262, // mmGE_USER_VGPR_EN
      0x2C06, // mmSPI_SHADER_PGM_CHKSUM_PS
      0x2C32, // mmSPI_SHADER_USER_ACCUM_PS_0
      0x2C33, // mmSPI_SHADER_USER_ACCUM_PS_1
      0x2C34, // mmSPI_SHADER_USER_ACCUM_PS_2
      0x2C35, // mmSPI_SHADER_USER_ACCUM_PS_3
  };

  // Merge fragment shader related registers. For each of the registers listed above, plus the input
  // control registers and the user data registers, copy the value from srcRegisters to destRegisters.
  // Where the register is set in destRegisters but not srcRegisters, clear it.
  auto destRegisters = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Registers].getMap(true);
  auto srcRegisters = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Registers].getMap(true);

  for (unsigned regNumber : ArrayRef<unsigned>(PsRegNumbers))
    mergeMapItem(destRegisters, srcRegisters, regNumber);

  const unsigned mmSpiPsInputCntl0 = 0xa191;
  const unsigned mmSpiPsInputCntl31 = 0xa1b0;
  for (unsigned regNumber = mmSpiPsInputCntl0; regNumber != mmSpiPsInputCntl31 + 1; ++regNumber)
    mergeMapItem(destRegisters, srcRegisters, regNumber);

  const unsigned mmSpiShaderUserDataPs0 = 0x2c0c;
  unsigned psUserDataCount = pContext->getGfxIpVersion().major < 9 ? 16 : 32;
  for (unsigned regNumber = mmSpiShaderUserDataPs0; regNumber != mmSpiShaderUserDataPs0 + psUserDataCount; ++regNumber)
    mergeMapItem(destRegisters, srcRegisters, regNumber);

  updateRootDescriptorRegisters(pContext, destDocument);

  std::string destBlob;
  destDocument.writeToBlob(destBlob);
  *pNewNote = *pNote1;
  auto data = new uint8_t[destBlob.size() + 4]; // 4 is for additional alignment spece
  memcpy(data, destBlob.data(), destBlob.size());
  pNewNote->hdr.descSize = destBlob.size();
  pNewNote->data = data;
}

// =====================================================================================================================
// Gets symbol according to symbol name, and creates a new one if it doesn't exist.
//
// @param pSymbolName : Symbol name
template <class Elf> ElfSymbol *ElfWriter<Elf>::getSymbol(const char *pSymbolName) {
  for (auto &symbol : m_symbols) {
    if (strcmp(pSymbolName, symbol.pSymName) == 0)
      return &symbol;
  }

  // Create new symbol
  ElfSymbol newSymbol = {};
  char *newSymName = new char[strlen(pSymbolName) + 1];
  strcpy(newSymName, pSymbolName);

  newSymbol.pSymName = newSymName;
  newSymbol.nameOffset = InvalidValue;
  newSymbol.secIdx = InvalidValue;
  newSymbol.info.type = STT_FUNC;
  newSymbol.info.binding = STB_LOCAL;
  m_symbols.push_back(newSymbol);

  return &m_symbols.back();
}

// =====================================================================================================================
// Gets note according to noteType.
//
// @param noteType : Note type
template <class Elf> ElfNote ElfWriter<Elf>::getNote(Util::Abi::PipelineAbiNoteType noteType) {
  for (auto &note : m_notes) {
    if (note.hdr.type == noteType)
      return note;
  }

  ElfNote note = {};
  return note;
}

// =====================================================================================================================
// Replaces exist note with input note according to input note type.
//
// @param pNote : Input note
template <class Elf> void ElfWriter<Elf>::setNote(ElfNote *pNote) {
  for (auto &note : m_notes) {
    if (note.hdr.type == pNote->hdr.type) {
      assert(note.data != pNote->data);
      delete[] note.data;
      note = *pNote;
      return;
    }
  }

  llvm_unreachable("Should never be called!");
}

// =====================================================================================================================
// Replace exist section with input section according to section index
//
// @param secIndex : Section index
// @param pSection : Input section
template <class Elf> void ElfWriter<Elf>::setSection(unsigned secIndex, SectionBuffer *pSection) {
  assert(secIndex < m_sections.size());
  assert(pSection->name == m_sections[secIndex].name);
  assert(pSection->data != m_sections[secIndex].data);

  delete[] m_sections[secIndex].data;
  m_sections[secIndex] = *pSection;
}

// =====================================================================================================================
// Determines the size needed for a memory buffer to store this ELF.
template <class Elf> size_t ElfWriter<Elf>::getRequiredBufferSizeBytes() {
  // Update offsets and size values
  calcSectionHeaderOffset();

  size_t totalBytes = sizeof(typename Elf::FormatHeader);

  // Iterate through the section list
  for (auto &section : m_sections)
    totalBytes += alignTo(section.secHead.sh_size, sizeof(unsigned));

  totalBytes += m_header.e_shentsize * m_header.e_shnum;
  totalBytes += m_header.e_phentsize * m_header.e_phnum;

  return totalBytes;
}

// =====================================================================================================================
// Assembles ELF notes and add them to .note section
template <class Elf> void ElfWriter<Elf>::assembleNotes() {
  if (m_noteSecIdx == InvalidValue)
    return;
  auto noteSection = &m_sections[m_noteSecIdx];
  const unsigned noteHeaderSize = sizeof(NoteHeader) - 8;
  unsigned noteSize = 0;
  for (auto &note : m_notes) {
    const unsigned noteNameSize = alignTo(note.hdr.nameSize, sizeof(unsigned));
    noteSize += noteHeaderSize + noteNameSize + alignTo(note.hdr.descSize, sizeof(unsigned));
  }

  delete[] noteSection->data;
  uint8_t *data = new uint8_t[std::max(noteSize, noteHeaderSize)];
  assert(data);
  noteSection->data = data;
  noteSection->secHead.sh_size = noteSize;

  for (auto &note : m_notes) {
    memcpy(data, &note.hdr, noteHeaderSize);
    data += noteHeaderSize;
    const unsigned noteNameSize = alignTo(note.hdr.nameSize, sizeof(unsigned));
    memcpy(data, &note.hdr.name, noteNameSize);
    data += noteNameSize;
    const unsigned noteDescSize = alignTo(note.hdr.descSize, sizeof(unsigned));
    memcpy(data, note.data, noteDescSize);
    data += noteDescSize;
  }

  assert(noteSection->secHead.sh_size == static_cast<unsigned>(data - noteSection->data));
}

// =====================================================================================================================
// Assembles ELF symbols and symbol info to .symtab section
template <class Elf> void ElfWriter<Elf>::assembleSymbols() {
  if (m_symSecIdx == InvalidValue)
    return;
  auto strTabSection = &m_sections[m_strtabSecIdx];
  auto newStrTabSize = 0;
  unsigned symbolCount = 0;
  for (auto &symbol : m_symbols) {
    if (symbol.nameOffset == InvalidValue)
      newStrTabSize += strlen(symbol.pSymName) + 1;

    if (symbol.secIdx != InvalidValue)
      symbolCount++;
  }

  if (newStrTabSize > 0) {
    unsigned strTabOffset = strTabSection->secHead.sh_size;
    auto strTabBuffer = new uint8_t[strTabSection->secHead.sh_size + newStrTabSize];
    memcpy(strTabBuffer, strTabSection->data, strTabSection->secHead.sh_size);
    delete[] strTabSection->data;

    strTabSection->data = strTabBuffer;
    strTabSection->secHead.sh_size += newStrTabSize;

    for (auto &symbol : m_symbols) {
      if (symbol.nameOffset == InvalidValue) {
        auto symNameSize = strlen(symbol.pSymName) + 1;
        memcpy(strTabBuffer + strTabOffset, symbol.pSymName, symNameSize);
        symbol.nameOffset = strTabOffset;
        delete[] symbol.pSymName;
        symbol.pSymName = reinterpret_cast<const char *>(strTabBuffer + strTabOffset);
        strTabOffset += symNameSize;
      }
    }
  }

  auto symSectionSize = sizeof(typename Elf::Symbol) * symbolCount;
  auto symbolSection = &m_sections[m_symSecIdx];

  if (!symbolSection->data)
    symbolSection->data = new uint8_t[symSectionSize];
  else if (symSectionSize > symbolSection->secHead.sh_size) {
    delete[] symbolSection->data;
    symbolSection->data = new uint8_t[symSectionSize];
  }
  symbolSection->secHead.sh_size = symSectionSize;

  auto symbolToWrite = reinterpret_cast<typename Elf::Symbol *>(const_cast<uint8_t *>(symbolSection->data));
  for (auto &symbol : m_symbols) {
    if (symbol.secIdx != InvalidValue) {
      symbolToWrite->st_name = symbol.nameOffset;
      symbolToWrite->st_info.all = symbol.info.all;
      symbolToWrite->st_other = 0;
      symbolToWrite->st_shndx = symbol.secIdx;
      symbolToWrite->st_value = symbol.value;
      symbolToWrite->st_size = symbol.size;
      ++symbolToWrite;
    }
  }

  assert(symbolSection->secHead.sh_size ==
         static_cast<unsigned>(reinterpret_cast<uint8_t *>(symbolToWrite) - symbolSection->data));
}

// =====================================================================================================================
// Determines the offset of the section header table by totaling the sizes of each binary chunk written to the ELF file,
// accounting for alignment.
template <class Elf> void ElfWriter<Elf>::calcSectionHeaderOffset() {
  unsigned sharedHdrOffset = 0;

  const unsigned elfHdrSize = sizeof(typename Elf::FormatHeader);
  const unsigned hdrSize = sizeof(typename Elf::Phdr);

  sharedHdrOffset += elfHdrSize;
  sharedHdrOffset += m_header.e_phnum * hdrSize;

  for (auto &section : m_sections) {
    const unsigned secSzBytes = alignTo(section.secHead.sh_size, sizeof(unsigned));
    sharedHdrOffset += secSzBytes;
  }

  m_header.e_phoff = m_header.e_phnum > 0 ? elfHdrSize : 0;
  m_header.e_shoff = sharedHdrOffset;
  m_header.e_shstrndx = m_strtabSecIdx;
  m_header.e_shnum = m_sections.size();
}

// =====================================================================================================================
// Writes the data out to the given buffer in ELF format. Assumes the buffer has been pre-allocated with adequate
// space, which can be determined with a call to "GetRequireBufferSizeBytes()".
//
// @param pElf : Output buffer to write ELF data
template <class Elf> void ElfWriter<Elf>::writeToBuffer(ElfPackage *pElf) {
  assert(pElf);

  // Update offsets and size values
  assembleNotes();
  assembleSymbols();

  const size_t reqSize = getRequiredBufferSizeBytes();
  pElf->resize(reqSize);
  auto data = pElf->data();
  memset(data, 0, reqSize);

  char *buffer = static_cast<char *>(data);

  // ELF header comes first
  const unsigned elfHdrSize = sizeof(typename Elf::FormatHeader);
  memcpy(buffer, &m_header, elfHdrSize);
  buffer += elfHdrSize;

  assert(m_header.e_phnum == 0);

  // Write each section buffer
  for (auto &section : m_sections) {
    section.secHead.sh_offset = static_cast<unsigned>(buffer - data);
    const unsigned sizeBytes = section.secHead.sh_size;
    memcpy(buffer, section.data, sizeBytes);
    buffer += alignTo(sizeBytes, sizeof(unsigned));
  }

  const unsigned secHdrSize = sizeof(typename Elf::SectionHeader);
  for (auto &section : m_sections) {
    memcpy(buffer, &section.secHead, secHdrSize);
    buffer += secHdrSize;
  }

  assert((buffer - data) == reqSize);
}

// =====================================================================================================================
// Copies ELF content from a ElfReader.
//
// @param reader : The ElfReader to copy from.
template <class Elf> Result ElfWriter<Elf>::copyFromReader(const ElfReader<Elf> &reader) {
  Result result = Result::Success;
  m_header = reader.getHeader();
  m_sections.resize(reader.getSections().size());
  for (size_t i = 0; i < reader.getSections().size(); ++i) {
    auto section = reader.getSections()[i];
    m_sections[i].secHead = section->secHead;
    m_sections[i].name = section->name;
    auto data = new uint8_t[section->secHead.sh_size + 1];
    memcpy(data, section->data, section->secHead.sh_size);
    data[section->secHead.sh_size] = 0;
    m_sections[i].data = data;
  }

  m_map = reader.getMap();
  assert(m_header.e_phnum == 0);

  m_noteSecIdx = m_map[NoteName];
  m_textSecIdx = m_map[TextName];
  m_symSecIdx = m_map[SymTabName];
  m_strtabSecIdx = m_map[StrTabName];
  assert(m_noteSecIdx > 0);
  assert(m_textSecIdx > 0);
  assert(m_symSecIdx > 0);
  assert(m_strtabSecIdx > 0);

  auto relocSec = m_map.find(RelocName);
  if (relocSec != m_map.end()) {
    m_relocSecIdx = relocSec->second;
  }

  auto noteSection = &m_sections[m_noteSecIdx];
  const unsigned noteHeaderSize = sizeof(NoteHeader) - 8;
  size_t offset = 0;
  while (offset < noteSection->secHead.sh_size) {
    const NoteHeader *note = reinterpret_cast<const NoteHeader *>(noteSection->data + offset);
    const unsigned noteNameSize = alignTo(note->nameSize, 4);
    ElfNote noteNode;
    memcpy(&noteNode.hdr, note, noteHeaderSize);
    memcpy(noteNode.hdr.name, note->name, noteNameSize);

    const unsigned noteDescSize = alignTo(note->descSize, 4);
    auto data = new uint8_t[noteDescSize];
    memcpy(data, noteSection->data + offset + noteHeaderSize + noteNameSize, noteDescSize);
    noteNode.data = data;

    offset += noteHeaderSize + noteNameSize + noteDescSize;
    m_notes.push_back(noteNode);
  }

  auto symSection = &m_sections[m_symSecIdx];
  const char *strTab = reinterpret_cast<const char *>(m_sections[m_strtabSecIdx].data);

  auto symbols = reinterpret_cast<const typename Elf::Symbol *>(symSection->data);
  auto symCount = static_cast<unsigned>(symSection->secHead.sh_size / symSection->secHead.sh_entsize);

  for (unsigned idx = 0; idx < symCount; ++idx) {
    ElfSymbol symbol = {};
    symbol.secIdx = symbols[idx].st_shndx;
    symbol.secName = m_sections[symbol.secIdx].name;
    symbol.pSymName = strTab + symbols[idx].st_name;
    symbol.size = symbols[idx].st_size;
    symbol.value = symbols[idx].st_value;
    symbol.info.all = symbols[idx].st_info.all;
    symbol.nameOffset = symbols[idx].st_name;
    m_symbols.push_back(symbol);
  }

  std::sort(m_symbols.begin(), m_symbols.end(), [](const ElfSymbol &a, const ElfSymbol &b) {
    return a.secIdx < b.secIdx || (a.secIdx == b.secIdx && a.value < b.value);
  });
  return result;
}

// =====================================================================================================================
// Reads ELF content from a buffer.
//
// @param pBuffer : Buffer to read data from
// @param bufSize : Size of the buffer
template <class Elf> Result ElfWriter<Elf>::ReadFromBuffer(const void *pBuffer, size_t bufSize) {
  ElfReader<Elf> reader(m_gfxIp);
  auto result = reader.ReadFromBuffer(pBuffer, &bufSize);
  if (result != Llpc::Result::Success)
    return result;
  return copyFromReader(reader);
}

// =====================================================================================================================
// Gets section data by section index.
//
// @param secIdx : Section index
// @param [out] ppSectionData : Section data
template <class Elf>
Result ElfWriter<Elf>::getSectionDataBySectionIndex(unsigned secIdx, const SectionBuffer **ppSectionData) const {
  Result result = Result::ErrorInvalidValue;
  if (secIdx < m_sections.size()) {
    *ppSectionData = &m_sections[secIdx];
    result = Result::Success;
  }
  return result;
}

// =====================================================================================================================
// Gets the count of relocations in the relocation section.
template <class Elf> unsigned ElfWriter<Elf>::getRelocationCount() {
  unsigned relocCount = 0;
  if (m_relocSecIdx >= 0) {
    auto &section = m_sections[m_relocSecIdx];
    relocCount = static_cast<unsigned>(section.secHead.sh_size / section.secHead.sh_entsize);
  }
  return relocCount;
}

// =====================================================================================================================
// Gets info of the relocation in the relocation section according to the specified index.
//
// @param idx : Relocation index
// @param pReloc : [out] Info of the relocation
template <class Elf> void ElfWriter<Elf>::getRelocation(unsigned idx, ElfReloc *reloc) {
  auto &section = m_sections[m_relocSecIdx];

  auto relocs = reinterpret_cast<const typename Elf::Reloc *>(section.data);
  reloc->offset = relocs[idx].r_offset;
  reloc->symIdx = relocs[idx].r_symbol;
  reloc->type = relocs[idx].r_type;
  reloc->addend = 0;
  reloc->useExplicitAddend = false;
}

// =====================================================================================================================
// Gets the count of symbols in the symbol table section.
template <class Elf> unsigned ElfWriter<Elf>::getSymbolCount() const {
  return m_symbols.size();
}

// =====================================================================================================================
// Gets info of the symbol in the symbol table section according to the specified index.
//
// @param idx : Symbol index
// @param pSymbol : [out] Info of the symbol
template <class Elf> void ElfWriter<Elf>::getSymbol(unsigned idx, ElfSymbol *symbol) {
  *symbol = m_symbols[idx];
}

// =====================================================================================================================
// Update descriptor offset to USER_DATA in metaNote.
//
// @param pContext : context related to ElfNote
// @param pNote : Note section to update
// @param [out] pNewNote : new note section
template <class Elf> void ElfWriter<Elf>::updateMetaNote(Context *pContext, const ElfNote *pNote, ElfNote *pNewNote) {
  msgpack::Document document;

  auto success =
      document.readFromBlob(StringRef(reinterpret_cast<const char *>(pNote->data), pNote->hdr.descSize), false);
  assert(success);
  (void(success)); // unused

  updateRootDescriptorRegisters(pContext, document);

  std::string blob;
  document.writeToBlob(blob);
  *pNewNote = *pNote;
  auto data = new uint8_t[blob.size()];
  memcpy(data, blob.data(), blob.size());
  pNewNote->hdr.descSize = blob.size();
  pNewNote->data = data;
}

// =====================================================================================================================
// Retrieves the section data for the specified section name, if it exists.
//
// @param pName : [in] Name of the section to look for
// @param ppData : [out] Pointer to section data
// @param pDataLength : [out] Size of the section data
template <class Elf>
Result ElfWriter<Elf>::getSectionData(const char *name, const void **ppData, size_t *dataLength) const {
  Result result = Result::ErrorInvalidValue;

  auto entry = m_map.find(name);
  if (entry != m_map.end()) {
    *ppData = m_sections[entry->second].data;
    *dataLength = static_cast<size_t>(m_sections[entry->second].secHead.sh_size);
    result = Result::Success;
  }

  return result;
}

// =====================================================================================================================
// Gets all associated symbols by section index.
//
// @param secIdx : Section index
// @param [out] secSymbols : ELF symbols
template <class Elf>
void ElfWriter<Elf>::GetSymbolsBySectionIndex(unsigned secIdx, std::vector<ElfSymbol *> &secSymbols) {
  unsigned symCount = m_symbols.size();

  for (unsigned idx = 0; idx < symCount; ++idx) {
    if (m_symbols[idx].secIdx == secIdx)
      secSymbols.push_back(&m_symbols[idx]);
  }
}

// =====================================================================================================================
// Update descriptor root offset in ELF binary
//
// @param pContext : Pipeline context
// @param [out] pPipelineElf : Final ELF binary
template <class Elf> void ElfWriter<Elf>::updateElfBinary(Context *pContext, ElfPackage *pPipelineElf) {
  // Merge PAL metadata
  ElfNote metaNote = {};
  metaNote = getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);

  assert(metaNote.data);
  ElfNote newMetaNote = {};
  updateMetaNote(pContext, &metaNote, &newMetaNote);
  setNote(&newMetaNote);

  writeToBuffer(pPipelineElf);
}

// =====================================================================================================================
// Merge ELF binary of fragment shader and ELF binary of non-fragment shaders into single ELF binary
//
// @param pContext : Pipeline context
// @param pFragmentElf : ELF binary of fragment shader
// @param [out] pPipelineElf : Final ELF binary
template <class Elf>
void ElfWriter<Elf>::mergeElfBinary(Context *pContext, const BinaryData *pFragmentElf, ElfPackage *pPipelineElf) {
  auto fragmentIsaSymbolName =
      Util::Abi::PipelineAbiSymbolNameStrings[static_cast<unsigned>(Util::Abi::PipelineSymbolType::PsMainEntry)];
  auto fragmentIntrlTblSymbolName =
      Util::Abi::PipelineAbiSymbolNameStrings[static_cast<unsigned>(Util::Abi::PipelineSymbolType::PsShdrIntrlTblPtr)];
  auto fragmentDisassemblySymbolName =
      Util::Abi::PipelineAbiSymbolNameStrings[static_cast<unsigned>(Util::Abi::PipelineSymbolType::PsDisassembly)];
  auto fragmentIntrlDataSymbolName =
      Util::Abi::PipelineAbiSymbolNameStrings[static_cast<unsigned>(Util::Abi::PipelineSymbolType::PsShdrIntrlData)];
  auto fragmentAmdIlSymbolName =
      Util::Abi::PipelineAbiSymbolNameStrings[static_cast<unsigned>(Util::Abi::PipelineSymbolType::PsAmdIl)];

  ElfReader<Elf64> reader(m_gfxIp);

  auto fragmentCodesize = pFragmentElf->codeSize;
  auto result = reader.ReadFromBuffer(pFragmentElf->pCode, &fragmentCodesize);
  assert(result == Result::Success);
  (void(result)); // unused

  // Merge GPU ISA code
  const ElfSectionBuffer<Elf64::SectionHeader> *nonFragmentTextSection = nullptr;
  ElfSectionBuffer<Elf64::SectionHeader> *fragmentTextSection = nullptr;
  std::vector<ElfSymbol> fragmentSymbols;
  std::vector<ElfSymbol *> nonFragmentSymbols;

  auto fragmentTextSecIndex = reader.GetSectionIndex(TextName);
  auto nonFragmentSecIndex = GetSectionIndex(TextName);
  reader.getSectionDataBySectionIndex(fragmentTextSecIndex, &fragmentTextSection);
  reader.GetSymbolsBySectionIndex(fragmentTextSecIndex, fragmentSymbols);

  getSectionDataBySectionIndex(nonFragmentSecIndex, &nonFragmentTextSection);
  GetSymbolsBySectionIndex(nonFragmentSecIndex, nonFragmentSymbols);
  ElfSymbol *fragmentIsaSymbol = nullptr;
  ElfSymbol *nonFragmentIsaSymbol = nullptr;
  std::string firstIsaSymbolName;

  for (auto symbol : nonFragmentSymbols) {
    if (firstIsaSymbolName.empty()) {
      // NOTE: Entry name of the first shader stage is missed in disassembly section, we have to add it back
      // when merge disassembly sections.
      if (strncmp(symbol->pSymName, "_amdgpu_", strlen("_amdgpu_")) == 0)
        firstIsaSymbolName = symbol->pSymName;
    }

    if (strcmp(symbol->pSymName, fragmentIsaSymbolName) == 0)
      nonFragmentIsaSymbol = symbol;

    if (!nonFragmentIsaSymbol)
      continue;

    // Reset all symbols after _amdgpu_ps_main
    symbol->secIdx = InvalidValue;
  }

  size_t isaOffset =
      !nonFragmentIsaSymbol ? alignTo(nonFragmentTextSection->secHead.sh_size, 0x100) : nonFragmentIsaSymbol->value;
  for (auto &fragmentSymbol : fragmentSymbols) {
    if (strcmp(fragmentSymbol.pSymName, fragmentIsaSymbolName) == 0) {
      // Modify ISA code
      fragmentIsaSymbol = &fragmentSymbol;
      ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
      mergeSection(nonFragmentTextSection, isaOffset, nullptr, fragmentTextSection, fragmentIsaSymbol->value, nullptr,
                   &newSection);
      setSection(nonFragmentSecIndex, &newSection);
    }

    if (!fragmentIsaSymbol)
      continue;

    // Update fragment shader related symbols
    ElfSymbol *symbol = nullptr;
    symbol = getSymbol(fragmentSymbol.pSymName);
    symbol->secIdx = nonFragmentSecIndex;
    symbol->secName = nullptr;
    symbol->value = isaOffset + fragmentSymbol.value - fragmentIsaSymbol->value;
    symbol->size = fragmentSymbol.size;
  }

  // LLPC doesn't use per pipeline internal table, and LLVM backend doesn't add symbols for disassembly info.
  assert(reader.isValidSymbol(fragmentIntrlTblSymbolName) == false &&
         reader.isValidSymbol(fragmentDisassemblySymbolName) == false &&
         reader.isValidSymbol(fragmentIntrlDataSymbolName) == false &&
         reader.isValidSymbol(fragmentAmdIlSymbolName) == false);
  (void(fragmentIntrlTblSymbolName));    // unused
  (void(fragmentDisassemblySymbolName)); // unused
  (void(fragmentIntrlDataSymbolName));   // unused
  (void(fragmentAmdIlSymbolName));       // unused

  // Merge ISA disassemble
  auto fragmentDisassemblySecIndex = reader.GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
  auto nonFragmentDisassemblySecIndex = GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
  ElfSectionBuffer<Elf64::SectionHeader> *fragmentDisassemblySection = nullptr;
  const ElfSectionBuffer<Elf64::SectionHeader> *nonFragmentDisassemblySection = nullptr;
  reader.getSectionDataBySectionIndex(fragmentDisassemblySecIndex, &fragmentDisassemblySection);
  getSectionDataBySectionIndex(nonFragmentDisassemblySecIndex, &nonFragmentDisassemblySection);
  if (nonFragmentDisassemblySection) {
    assert(fragmentDisassemblySection);
    // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
    // text search will be incorrect. It is only needed for ElfReader, ElfWriter always append a null terminator
    // for all section data.
    auto fragmentDisassemblySectionEnd =
        fragmentDisassemblySection->data + fragmentDisassemblySection->secHead.sh_size - 1;
    uint8_t lastChar = *fragmentDisassemblySectionEnd;
    const_cast<uint8_t *>(fragmentDisassemblySectionEnd)[0] = '\0';
    auto fragmentDisassembly =
        strstr(reinterpret_cast<const char *>(fragmentDisassemblySection->data), fragmentIsaSymbolName);
    const_cast<uint8_t *>(fragmentDisassemblySectionEnd)[0] = lastChar;

    auto fragmentDisassemblyOffset =
        !fragmentDisassembly ? 0
                             : (fragmentDisassembly - reinterpret_cast<const char *>(fragmentDisassemblySection->data));

    auto disassemblyEnd =
        strstr(reinterpret_cast<const char *>(nonFragmentDisassemblySection->data), fragmentIsaSymbolName);
    auto disassemblySize = !disassemblyEnd
                               ? nonFragmentDisassemblySection->secHead.sh_size
                               : disassemblyEnd - reinterpret_cast<const char *>(nonFragmentDisassemblySection->data);

    ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
    mergeSection(nonFragmentDisassemblySection, disassemblySize, firstIsaSymbolName.c_str(), fragmentDisassemblySection,
                 fragmentDisassemblyOffset, fragmentIsaSymbolName, &newSection);
    setSection(nonFragmentDisassemblySecIndex, &newSection);
  }

  // Merge LLVM IR disassemble
  const std::string llvmIrSectionName = std::string(Util::Abi::AmdGpuCommentLlvmIrName);
  ElfSectionBuffer<Elf64::SectionHeader> *fragmentLlvmIrSection = nullptr;
  const ElfSectionBuffer<Elf64::SectionHeader> *nonFragmentLlvmIrSection = nullptr;

  auto fragmentLlvmIrSecIndex = reader.GetSectionIndex(llvmIrSectionName.c_str());
  auto nonFragmentLlvmIrSecIndex = GetSectionIndex(llvmIrSectionName.c_str());
  reader.getSectionDataBySectionIndex(fragmentLlvmIrSecIndex, &fragmentLlvmIrSection);
  getSectionDataBySectionIndex(nonFragmentLlvmIrSecIndex, &nonFragmentLlvmIrSection);

  if (nonFragmentLlvmIrSection) {
    assert(fragmentLlvmIrSection);

    // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
    // text search will be incorrect. It is only needed for ElfReader, ElfWriter always append a null terminator
    // for all section data.
    auto fragmentLlvmIrSectionEnd = fragmentLlvmIrSection->data + fragmentLlvmIrSection->secHead.sh_size - 1;
    uint8_t lastChar = *fragmentLlvmIrSectionEnd;
    const_cast<uint8_t *>(fragmentLlvmIrSectionEnd)[0] = '\0';
    auto fragmentLlvmIrStart =
        strstr(reinterpret_cast<const char *>(fragmentLlvmIrSection->data), fragmentIsaSymbolName);
    const_cast<uint8_t *>(fragmentLlvmIrSectionEnd)[0] = lastChar;

    auto fragmentLlvmIrOffset =
        !fragmentLlvmIrStart ? 0 : (fragmentLlvmIrStart - reinterpret_cast<const char *>(fragmentLlvmIrSection->data));

    auto llvmIrEnd = strstr(reinterpret_cast<const char *>(nonFragmentLlvmIrSection->data), fragmentIsaSymbolName);
    auto llvmIrSize = !llvmIrEnd ? nonFragmentLlvmIrSection->secHead.sh_size
                                 : llvmIrEnd - reinterpret_cast<const char *>(nonFragmentLlvmIrSection->data);

    ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
    mergeSection(nonFragmentLlvmIrSection, llvmIrSize, firstIsaSymbolName.c_str(), fragmentLlvmIrSection,
                 fragmentLlvmIrOffset, fragmentIsaSymbolName, &newSection);
    setSection(nonFragmentLlvmIrSecIndex, &newSection);
  }

  // Merge PAL metadata
  ElfNote nonFragmentMetaNote = {};
  nonFragmentMetaNote = getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);

  assert(nonFragmentMetaNote.data);
  ElfNote fragmentMetaNote = {};
  ElfNote newMetaNote = {};
  fragmentMetaNote = reader.getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
  mergeMetaNote(pContext, &nonFragmentMetaNote, &fragmentMetaNote, &newMetaNote);
  setNote(&newMetaNote);

  writeToBuffer(pPipelineElf);
}

// =====================================================================================================================
// Reset the contents to an empty ELF file.
template <class Elf> void ElfWriter<Elf>::reinitialize() {
  m_map.clear();
  m_notes.clear();
  m_symbols.clear();

  m_sections.push_back({});

  m_strtabSecIdx = m_sections.size();
  m_sections.push_back({});
  m_sections.back().name = StrTabName;
  m_map[StrTabName] = m_strtabSecIdx;

  m_textSecIdx = m_sections.size();
  m_sections.push_back({});
  m_sections.back().name = TextName;
  m_map[TextName] = m_textSecIdx;

  m_symSecIdx = m_sections.size();
  m_sections.push_back({});
  m_sections.back().name = SymTabName;
  m_map[SymTabName] = m_symSecIdx;

  m_noteSecIdx = m_sections.size();
  m_sections.push_back({});
  m_sections.back().name = NoteName;
  m_map[NoteName] = m_noteSecIdx;

  m_relocSecIdx = InvalidValue;
}

// =====================================================================================================================
// Retrieves the actual descriptor offset at the specified binding from UserDataNode.
//
// @param descSet : DescriptorSet index of the resource
// @param binding : Binding slot of the resource
// @param nodeType : Type of resource requested by the shader
// @param nodes: The UserDataNode provided at runtime
// @param nodeCount : Number of resource nodes in UserDataNode
static unsigned getDescriptorResourceOffset(unsigned descSet, unsigned binding, ResourceMappingNodeType nodeType,
                                            const ResourceMappingNode *nodes, unsigned nodeCount) {
  for (unsigned i = 0; i < nodeCount; ++i) {
    const ResourceMappingNode *resource = nodes + i;

    if (resource->type == ResourceMappingNodeType::DescriptorTableVaPtr) {
      unsigned offset = getDescriptorResourceOffset(descSet, binding, nodeType, resource->tablePtr.pNext,
                                                    resource->tablePtr.nodeCount);
      if (offset != InvalidValue) {
        return offset;
      }
      continue;
    }
    if (resource->type > ResourceMappingNodeType::DescriptorBuffer) {
      continue;
    }
    if (resource->srdRange.set != descSet || resource->srdRange.binding != binding) {
      continue;
    }

    if (nodeType == ResourceMappingNodeType::DescriptorSampler &&
        resource->type == ResourceMappingNodeType::DescriptorCombinedTexture) {
      return (resource->offsetInDwords + 8) * sizeof(unsigned); // Offset by DescriptorSizeResource.
    } else {
      return (resource->offsetInDwords) * sizeof(unsigned);
    }
  }
  return InvalidValue;
}

// =====================================================================================================================
// Retrieves the actual descriptor stride at the specified binding from UserDataNode.
//
// @param descSet : DescriptorSet index of the resource
// @param binding : Binding slot of the resource
// @param nodes : The UserDataNode provided at runtime
// @param nodeCount : Number of resource nodes in UserDataNode
static unsigned getDescriptorResourceStride(unsigned descSet, unsigned binding, const ResourceMappingNode *nodes,
                                            unsigned nodeCount) {
  for (unsigned i = 0; i < nodeCount; ++i) {
    const ResourceMappingNode *resource = nodes + i;

    if (resource->type == ResourceMappingNodeType::DescriptorTableVaPtr) {
      unsigned offset =
          getDescriptorResourceStride(descSet, binding, resource->tablePtr.pNext, resource->tablePtr.nodeCount);
      if (offset != InvalidValue) {
        return offset;
      }
      continue;
    }
    if (resource->type > ResourceMappingNodeType::DescriptorBuffer) {
      continue;
    }
    if (resource->srdRange.set != descSet || resource->srdRange.binding != binding) {
      continue;
    }

    switch (resource->type) {
    case ResourceMappingNodeType::DescriptorSampler:
      return DescriptorSizeSampler;
      break;
    case ResourceMappingNodeType::DescriptorResource:
    case ResourceMappingNodeType::DescriptorFmask:
      return DescriptorSizeResource;
    case ResourceMappingNodeType::DescriptorCombinedTexture:
      return (DescriptorSizeResource + DescriptorSizeSampler);
    default:
      llvm_unreachable("Unexpected resource node type");
      break;
    }
  }
  return InvalidValue;
}

// =====================================================================================================================
// Get value for a descriptor offset relocation (doff_x_y_t symbol).
//
// @param context : [in] Pipeline compilation context
// @param relocEntry : The relocation entry to fixup
// @param isGraphicsPipeline : Whether we are processing a compute or graphics pipeline
// @param value : [out] The value of the relocation
bool getDescriptorOffsetRelocationValue(Context *context, RelocationEntry relocEntry, bool isGraphicsPipeline,
                                        unsigned *value) {
  size_t idx = 0;
  const char *relocName = relocEntry.name + 5;
  unsigned descSet = std::stoi(relocName, &idx);
  relocName += idx + 1;
  idx = 0;
  unsigned binding = std::stoi(relocName, &idx);
  relocName += idx + 1;
  ResourceMappingNodeType type = ResourceMappingNodeType::Unknown;
  switch (*relocName) {
  case 's':
    type = ResourceMappingNodeType::DescriptorSampler;
    break;
  case 'r':
    type = ResourceMappingNodeType::DescriptorResource;
    break;
  case 'b':
    type = ResourceMappingNodeType::DescriptorBuffer;
    break;
  }

  if (isGraphicsPipeline) {
    auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
    *value = getDescriptorResourceOffset(descSet, binding, type, pipelineInfo->vs.pUserDataNodes,
                                         pipelineInfo->vs.userDataNodeCount);
    if (*value == InvalidValue) {
      *value = getDescriptorResourceOffset(descSet, binding, type, pipelineInfo->fs.pUserDataNodes,
                                           pipelineInfo->fs.userDataNodeCount);
    }
  } else {
    auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
    *value = getDescriptorResourceOffset(descSet, binding, type, pipelineInfo->cs.pUserDataNodes,
                                         pipelineInfo->cs.userDataNodeCount);
  }

  return *value != InvalidValue;
}

// =====================================================================================================================
// Get value for a descriptor stride relocation (dstride_x_y symbol).
//
// @param context : [in] Pipeline compilation context
// @param relocEntry : The relocation entry to fixup
// @param isGraphicsPipeline : Whether we are processing a compute or graphics pipeline
// @param value : [out] The value of the relocation
bool getDescriptorStrideRelocationValue(Context *context, RelocationEntry relocEntry, bool isGraphicsPipeline,
                                        unsigned *value) {
  size_t idx = 0;
  const char *relocName = relocEntry.name + 8;
  unsigned descSet = std::stoi(relocName, &idx);
  relocName += idx + 1;
  idx = 0;
  unsigned binding = std::stoi(relocName, &idx);
  relocName += idx + 1;

  if (isGraphicsPipeline) {
    auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
    *value = getDescriptorResourceStride(descSet, binding, pipelineInfo->vs.pUserDataNodes,
                                         pipelineInfo->vs.userDataNodeCount);
    if (*value == InvalidValue) {
      *value = getDescriptorResourceStride(descSet, binding, pipelineInfo->fs.pUserDataNodes,
                                           pipelineInfo->fs.userDataNodeCount);
    }
  } else {
    auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
    *value = getDescriptorResourceStride(descSet, binding, pipelineInfo->cs.pUserDataNodes,
                                         pipelineInfo->cs.userDataNodeCount);
  }

  return *value != InvalidValue;
}

// =====================================================================================================================
// Get value a device index relocation ($deviceIdx symbol).
//
// @param context : [in] Pipeline compilation context
// @param relocEntry : The relocation entry to fixup
// @param isGraphicsPipeline : Whether we are processing a compute or graphics pipeline
// @param value : [out] The value of the relocation
bool getDeviceIndexRelocationValue(Context *context, RelocationEntry relocEntry, bool isGraphicsPipeline,
                                   unsigned *value) {
  auto pipelineBuildInfo = context->getPipelineContext()->getPipelineBuildInfo();
  if (isGraphicsPipeline) {
    *value = static_cast<const GraphicsPipelineBuildInfo *>(pipelineBuildInfo)->iaState.deviceIndex;
  } else {
    *value = static_cast<const ComputePipelineBuildInfo *>(pipelineBuildInfo)->deviceIndex;
  }
  return true;
}

// =====================================================================================================================
// Get value of a numSamples relocation ($numSamples symbol).
//
// @param context : [in] Pipeline compilation context
// @param relocEntry : The relocation entry to fixup
// @param isGraphicsPipeline : Whether we are processing a compute or graphics pipeline
// @param value : [out] The value of the relocation
bool getNumSamplesRelocationValue(Context *context, RelocationEntry relocEntry, bool isGraphicsPipeline,
                                  unsigned *value) {
  auto pipelineBuildInfo = context->getPipelineContext()->getPipelineBuildInfo();
  assert(isGraphicsPipeline && "numSamples relocation is for graphics pipeline only.");
  *value = static_cast<const GraphicsPipelineBuildInfo *>(pipelineBuildInfo)->rsState.numSamples;
  return true;
}

// =====================================================================================================================
// Get value of a numSamples relocation ($samplePatternIdx symbol).
//
// @param context : [in] Pipeline compilation context
// @param relocEntry : The relocation entry to fixup
// @param isGraphicsPipeline : Whether we are processing a compute or graphics pipeline
// @param value : [out] The value of the relocation
bool getSamplePatternIdxRelocationValue(Context *context, RelocationEntry relocEntry, bool isGraphicsPipeline,
                                        unsigned *value) {
  auto pipelineBuildInfo = context->getPipelineContext()->getPipelineBuildInfo();
  assert(isGraphicsPipeline && "samplePatternIdx relocation is for graphics pipeline only.");
  *value = static_cast<const GraphicsPipelineBuildInfo *>(pipelineBuildInfo)->rsState.samplePatternIdx;
  return true;
}

// =====================================================================================================================
// Get the value of a relocation symbol. Returns true if success, and false if the symbol is unknown.
//
// @param context : [in] Pipeline compilation context
// @param relocEntry : The relocation entry to fixup
// @param isGraphicsPipeline : Whether we are processing a compute or graphics pipeline
// @param value : [out] The value of the relocation
bool getRelocationSymbolValue(Context *context, RelocationEntry relocEntry, bool isGraphicsPipeline, unsigned *value) {
  if (strncmp(relocEntry.name, "doff_", 5) == 0) {
    return getDescriptorOffsetRelocationValue(context, relocEntry, isGraphicsPipeline, value);
  } else if (strncmp(relocEntry.name, "dstride_", 8) == 0) {
    return getDescriptorStrideRelocationValue(context, relocEntry, isGraphicsPipeline, value);
  } else if (strcmp(relocEntry.name, "$deviceIdx") == 0) {
    return getDeviceIndexRelocationValue(context, relocEntry, isGraphicsPipeline, value);
  } else if (strcmp(relocEntry.name, "$numSamples") == 0) {
    return getNumSamplesRelocationValue(context, relocEntry, isGraphicsPipeline, value);
  } else if (strcmp(relocEntry.name, "$samplePatternIdx") == 0) {
    return getSamplePatternIdxRelocationValue(context, relocEntry, isGraphicsPipeline, value);
  }
  return false;
}

// =====================================================================================================================
// Fixup relocations in the ELF with actual values.
//
// @param writer : [in,out] Writer to target ELF
// @param relocations : [in] Relocation entries
// @param context : [in] Pipeline compilation context
// @param isGraphicsPipeline : Whether we are processing a compute or graphics pipeline
template <class Elf>
void fixUpRelocations(ElfWriter<Elf> *writer, const std::vector<RelocationEntry> &relocations, Context *context,
                      bool isGraphicsPipeline) {
  char *data = nullptr;
  size_t dataLength = 0;
  writer->getSectionData(TextName, const_cast<const void **>(reinterpret_cast<void **>(&data)), &dataLength);

  for (unsigned i = 0; i < relocations.size(); ++i) {
    auto &reloc = relocations[i];
    unsigned relocationValue = 0;
    if (getRelocationSymbolValue(context, reloc, isGraphicsPipeline, &relocationValue)) {
      assert(data != nullptr && dataLength >= reloc.reloc.offset);
      assert(reloc.reloc.type == R_AMDGPU_ABS32 && "can only handle R_AMDGPU_ABS32 typed relocations.");
      unsigned *targetDword = reinterpret_cast<unsigned *>(data + reloc.reloc.offset);
      if (reloc.reloc.useExplicitAddend) {
        *targetDword = relocationValue + reloc.reloc.addend;
      } else {
        *targetDword += relocationValue;
      }
    } else {
      llvm_unreachable("Unknown relocation entry.");
    }
  }
}

// =====================================================================================================================
// Link the relocatable ELF readers into a pipeline ELF.
//
// @param relocatableElfs : An array of 3 relocatable ELF objects: {vertext, fragment, fetch}.
// @param context : Acquired context
template <class Elf>
Result ElfWriter<Elf>::linkGraphicsRelocatableElf(const ArrayRef<ElfReader<Elf> *> &relocatableElfs, Context *context) {
  assert(relocatableElfs.size() == 3 && "Can only handle VsPs pipeline with a fetch shader for now.");

  // The alignment requirements for the text section.
  const uint32_t vertexShaderAlignment = 0x10;
  const uint32_t fragmentShaderAlignment = 0x100;

  reinitialize();

  // Get the main data for the header, the parts that change will be updated when writing to buffer.
  m_header = relocatableElfs[0]->getHeader();

  // Copy the contents of the string table.  We only merge the vertex and fragment shaders.
  ElfSectionBuffer<typename Elf::SectionHeader> *vertexShaderStringTable = nullptr;
  relocatableElfs[0]->getSectionDataBySectionIndex(relocatableElfs[0]->getStrtabSecIdx(), &vertexShaderStringTable);
  ElfSectionBuffer<typename Elf::SectionHeader> *fragmentShaderStringTable = nullptr;
  relocatableElfs[1]->getSectionDataBySectionIndex(relocatableElfs[1]->getStrtabSecIdx(), &fragmentShaderStringTable);

  mergeSection(vertexShaderStringTable, vertexShaderStringTable->secHead.sh_size, nullptr, fragmentShaderStringTable, 0,
               nullptr, &m_sections[m_strtabSecIdx]);

  // Merge text sections
  ElfSectionBuffer<typename Elf::SectionHeader> *vertexShaderTextSection = nullptr;
  relocatableElfs[0]->getTextSectionData(&vertexShaderTextSection);
  ElfSectionBuffer<typename Elf::SectionHeader> *fragmentShaderTextSection = nullptr;
  relocatableElfs[1]->getTextSectionData(&fragmentShaderTextSection);
  ElfSectionBuffer<typename Elf::SectionHeader> *fetchShaderTextSection = nullptr;
  relocatableElfs[2]->getTextSectionData(&fetchShaderTextSection);

  // First merge the fetch shader and the vertex shader, and then merge with the fragment shader.
  ElfSectionBuffer<typename Elf::SectionHeader> fullVertexShaderTextSection;
  mergeSection(fetchShaderTextSection, alignTo(fetchShaderTextSection->secHead.sh_size, vertexShaderAlignment), nullptr,
               vertexShaderTextSection, 0, nullptr, &fullVertexShaderTextSection);
  mergeSection(&fullVertexShaderTextSection,
               alignTo(fullVertexShaderTextSection.secHead.sh_size, fragmentShaderAlignment), nullptr,
               fragmentShaderTextSection, 0, nullptr, &m_sections[m_textSecIdx]);
  delete[] fullVertexShaderTextSection.data;

  // We do not copy the fetch shader's tring table, so we need to make sure we use the string table offset for the name
  // from the vertex shader.
  m_sections[m_textSecIdx].secHead.sh_name = vertexShaderTextSection->secHead.sh_name;

  // Build the symbol table.  First set the symbol table section header.
  ElfSectionBuffer<typename Elf::SectionHeader> *symbolTableSection = nullptr;
  relocatableElfs[0]->getSectionDataBySectionIndex(relocatableElfs[0]->getSymSecIdx(), &symbolTableSection);
  m_sections[m_symSecIdx].secHead = symbolTableSection->secHead;

  std::vector<RelocationEntry> relocations;

  // Insert a dummy symbol. The ELF spec requires the symbol table to begin with a dummy symbol.
  getSymbol("")->secIdx = 0;

  // Now get the symbols that belong in the symbol table.  No symbols from the fetch shader are needed.
  unsigned offset = alignTo(fetchShaderTextSection->secHead.sh_size, vertexShaderAlignment);
  for (unsigned i = 0; i < 2; ++i) {
    const ElfReader<Elf> *elf = relocatableElfs[i];
    unsigned relocElfTextSectionId = elf->GetSectionIndex(TextName);
    std::vector<ElfSymbol> symbols;
    elf->GetSymbolsBySectionIndex(relocElfTextSectionId, symbols);
    for (auto sym : symbols) {
      if (strncmp(sym.pSymName, "BB", 2) == 0) {
        continue;
      }
      ElfSymbol *newSym = getSymbol(sym.pSymName);
      newSym->secIdx = m_textSecIdx;
      newSym->secName = nullptr;
      newSym->value = sym.value + offset;
      newSym->size = sym.size;
      newSym->info = sym.info;
    }

    // Copy and adjust all relocations.
    auto relocationCount = elf->getRelocationCount();
    for (unsigned i = 0; i < relocationCount; ++i) {
      ElfReloc relocation = {};
      RelocationEntry relocEntry = {};
      elf->getRelocation(i, &relocation);
      relocEntry.reloc = relocation;
      relocEntry.reloc.offset += offset;
      ElfSymbol symbol = {};
      elf->getSymbol(relocation.symIdx, &symbol);
      relocEntry.name = symbol.pSymName;
      relocations.push_back(relocEntry);
    }

    // Update the offset for the next elf file.
    ElfSectionBuffer<typename Elf::SectionHeader> *textSection = nullptr;
    elf->getSectionDataBySectionIndex(relocElfTextSectionId, &textSection);
    offset = alignTo(offset + textSection->secHead.sh_size, fragmentShaderAlignment);
  }

  // Update the size and offset of the vertex shader
  const ElfReader<Elf> *pElf = relocatableElfs[2];
  ElfSectionBuffer<typename Elf::SectionHeader> *pTextSection = nullptr;
  unsigned relocElfTextSectionId = pElf->GetSectionIndex(TextName);
  pElf->getSectionDataBySectionIndex(relocElfTextSectionId, &pTextSection);

  ElfSymbol *vsShaderSym = getSymbol("_amdgpu_vs_main");
  ElfSymbol *psShaderSym = getSymbol("_amdgpu_ps_main");

  // The vertex shader will include the fetch shader, so it should always start at offset 0.
  vsShaderSym->value = 0;

  // It will finish no later than where the fragment shader starts, so this is a safe size.
  vsShaderSym->size = psShaderSym->value;

  // Apply relocations
  fixUpRelocations(this, relocations, context, true);

  // Set the .note section header
  ElfSectionBuffer<typename Elf::SectionHeader> *noteSection = nullptr;
  relocatableElfs[0]->getSectionDataBySectionIndex(relocatableElfs[0]->GetSectionIndex(NoteName), &noteSection);

  m_sections[m_noteSecIdx].secHead = noteSection->secHead;

  // Merge and update the .note data
  // The merged note info will be updated using data in the pipeline create info, but nothing needs to be done yet.
  ElfNote vertexShaderNote = relocatableElfs[0]->getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
  ElfNote fragmentShaderNote = relocatableElfs[1]->getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
  ElfNote fetchShaderNote = relocatableElfs[2]->getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);

  m_notes.push_back({});
  vertexShaderNote = mergeVertexRegisterNote(&vertexShaderNote, &fetchShaderNote);
  mergeMetaNote(context, &vertexShaderNote, &fragmentShaderNote, &m_notes.back());
  delete[] vertexShaderNote.data;

  // Merge other sections.  For now, none of the other sections are important, so we will not do anything.

  return Result::Success;
}

// =====================================================================================================================
// Link the compute shader relocatable ELF reader into a pipeline ELF.
//
// @param relocatableElf : Relocatable compute shader elf
// @param context : Acquired context
template <class Elf>
Result ElfWriter<Elf>::linkComputeRelocatableElf(const ElfReader<Elf> &relocatableElf, Context *context) {
  // Currently nothing to do, just copy the elf.
  copyFromReader(relocatableElf);

  // Apply relocations
  std::vector<RelocationEntry> relocations;
  for (unsigned i = 0; i < relocatableElf.getRelocationCount(); ++i) {
    ElfReloc relocation = {};
    relocatableElf.getRelocation(i, &relocation);
    RelocationEntry relocEntry = {};
    ElfSymbol symbol = {};
    relocatableElf.getSymbol(relocation.symIdx, &symbol);
    relocEntry.name = symbol.pSymName;
    relocEntry.reloc = relocation;
    relocations.push_back(relocEntry);
  }
  fixUpRelocations(this, relocations, context, false);

  // Update root descriptor register value in metadata note
  ElfNote metadataNote = getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
  ElfNote updatedNote;
  updateMetaNote(context, &metadataNote, &updatedNote);
  setNote(&updatedNote);

  return Result::Success;
}

// =====================================================================================================================
// Merge the metadata for the fetch shader and reloctable vertex shader to get the metadata for the resulting vertex
// shader.
//
// @param vertexShaderNote [in]: The ElfNote for the relocatable vertex shader.
// @param fetchShaderNote [in]: The ElfNote for the fetch shader.
template <class Elf>
ElfNote ElfWriter<Elf>::mergeVertexRegisterNote(ElfNote *vertextShaderNote, ElfNote *fetchShaderNote) {
  msgpack::Document destDocument;
  msgpack::Document srcDocument;

  auto success = destDocument.readFromBlob(
      StringRef(reinterpret_cast<const char *>(vertextShaderNote->data), vertextShaderNote->hdr.descSize), false);
  assert(success);

  success = srcDocument.readFromBlob(
      StringRef(reinterpret_cast<const char *>(fetchShaderNote->data), fetchShaderNote->hdr.descSize), false);
  assert(success);

  auto destPipeline =
      destDocument.getRoot().getMap(false)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(false)[0];
  auto srcPipeline =
      srcDocument.getRoot().getMap(false)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(false)[0];
  (void(success)); // unused

  auto destHwStages = destPipeline.getMap(false)[Util::Abi::PipelineMetadataKey::HardwareStages].getMap(false);
  auto srcHwStages = srcPipeline.getMap(false)[Util::Abi::PipelineMetadataKey::HardwareStages].getMap(false);
  auto pHwPsStageName = HwStageNames[static_cast<unsigned>(Util::Abi::HardwareStage::Vs)];
  auto destVs = destHwStages[pHwPsStageName].getMap(false);
  auto srcVs = srcHwStages[pHwPsStageName].getMap(false);

  // Update the register counts
  msgpack::DocNode &destSgprCount = destVs[".sgpr_count"];
  msgpack::DocNode &srcSgprCount = srcVs[".sgpr_count"];
  destVs[".sgpr_count"] = destDocument.getNode(std::max(destSgprCount.getUInt(), srcSgprCount.getUInt()));

  msgpack::DocNode &destVgprCount = destVs[".vgpr_count"];
  msgpack::DocNode &srcVgprCount = srcVs[".vgpr_count"];
  destVs[".vgpr_count"] = destDocument.getNode(std::max(destVgprCount.getUInt(), srcVgprCount.getUInt()));

  msgpack::DocNode &destRegisterInfoNode = destPipeline.getMap(false)[Util::Abi::PipelineMetadataKey::Registers];
  msgpack::MapDocNode &destRegisterInfoMap = destRegisterInfoNode.getMap(false);
  Pal::Gfx9::SPI_SHADER_PGM_RSRC1_VS destVSRegInfo;
  destVSRegInfo.u32All = destRegisterInfoMap[destDocument.getNode(Pal::Gfx9::mmSPI_SHADER_PGM_RSRC1_VS)].getUInt();

  msgpack::DocNode &srcRegisterInfoNode = srcPipeline.getMap(false)[Util::Abi::PipelineMetadataKey::Registers];
  msgpack::MapDocNode &srcRegisterInfoMap = srcRegisterInfoNode.getMap(false);
  Pal::Gfx9::SPI_SHADER_PGM_RSRC1_VS srcVSRegInfo;
  srcVSRegInfo.u32All = srcRegisterInfoMap[srcDocument.getNode(Pal::Gfx9::mmSPI_SHADER_PGM_RSRC1_VS)].getUInt();

  destVSRegInfo.bitfields.SGPRS = std::max(destVSRegInfo.bitfields.SGPRS, srcVSRegInfo.bitfields.SGPRS);
  destVSRegInfo.bitfields.VGPRS = std::max(destVSRegInfo.bitfields.VGPRS, srcVSRegInfo.bitfields.VGPRS);
  destRegisterInfoMap[destDocument.getNode(Pal::Gfx9::mmSPI_SHADER_PGM_RSRC1_VS)] =
      destDocument.getNode(destVSRegInfo.u32All);

  // Write the metadata back out
  std::string destBlob;
  destDocument.writeToBlob(destBlob);
  ElfNote newNote = *vertextShaderNote;

  auto data = new uint8_t[destBlob.size() + 4]; // 4 is for additional alignment spece
  memcpy(data, destBlob.data(), destBlob.size());
  newNote.hdr.descSize = destBlob.size();
  newNote.data = data;
  return newNote;
}

// =====================================================================================================================
// Reads the .note section of the gieven elf package to retrieve the vertex shader interface information.
//
// @param elfPackage : The elf package that contains the elf for the vertex shader.
// @param context : The acquired context.
void readInterfaceData(const ElfPackage *elfPackage, Context *context, GfxIpVersion gfxIp) {
  ElfReader<Elf64> reader(gfxIp);
  size_t codeSize = elfPackage->size_in_bytes();
  Result result = reader.ReadFromBuffer(elfPackage->data(), &codeSize);
  assert(result == Result::Success);
  (void(result)); // unused

  ElfNote note = {};
  note = reader.getNote(Util::Abi::PipelineAbiNoteType::PalMetadata);

  msgpack::Document document;
  auto success = document.readFromBlob(StringRef(reinterpret_cast<const char *>(note.data), note.hdr.descSize), false);
  assert(success);
  (void(success)); // unused

  msgpack::DocNode &rootNode = document.getRoot();

  msgpack::DocNode &pipelineArrayNode = rootNode.getMap(false)[Util::Abi::PalCodeObjectMetadataKey::Pipelines];
  msgpack::DocNode &pipelineInfoNode = pipelineArrayNode.getArray(false)[0];
  msgpack::DocNode &registerInfoNode = pipelineInfoNode.getMap(false)[Util::Abi::PipelineMetadataKey::Registers];
  msgpack::MapDocNode &registerInfoMap = registerInfoNode.getMap(false);

  msgpack::DocNode &vertexInputInfoNode = pipelineInfoNode.getMap(false)[".vertexInputTypes"];

  lgc::VsInterfaceData *vsInterfaceData = context->getLgcContext()->getVsInterfaceData();

  unsigned maxUserData = Pal::Gfx9::mmSPI_SHADER_USER_DATA_VS_0;
  for (auto registerInfo : registerInfoMap) {
    unsigned infoType = registerInfo.first.getUInt();
    if (infoType == Pal::Gfx9::mmSPI_SHADER_PGM_RSRC1_VS) {
      // Get the comp cnt
      Pal::Gfx9::SPI_SHADER_PGM_RSRC1_VS data;
      data.u32All = registerInfo.second.getUInt();
      vsInterfaceData->setVgprCompCnt(data.bitfields.VGPR_COMP_CNT);
    } else if (infoType >= Pal::Gfx9::mmSPI_SHADER_USER_DATA_VS_0 &&
               infoType <= Pal::Gfx9::mmSPI_SHADER_USER_DATA_VS_31) {
      if (infoType > maxUserData) {
        maxUserData = infoType;
      }

      switch (static_cast<Util::Abi::UserDataMapping>(registerInfo.second.getUInt())) {
      case Util::Abi::UserDataMapping::BaseVertex:
        vsInterfaceData->setBaseVertexRegister(infoType - Pal::Gfx9::mmSPI_SHADER_USER_DATA_VS_0);
        break;
      case Util::Abi::UserDataMapping::BaseInstance:
        vsInterfaceData->setBaseInstanceRegister(infoType - Pal::Gfx9::mmSPI_SHADER_USER_DATA_VS_0);
        break;
      case Util::Abi::UserDataMapping::VertexBufferTable:
        vsInterfaceData->setVertexBuffer(infoType - Pal::Gfx9::mmSPI_SHADER_USER_DATA_VS_0);
        break;
      default:
        break;
      }
    }
  }
  // Add 1 for the offset of the scratch memory.
  vsInterfaceData->setLastSgpr(maxUserData - Pal::Gfx9::mmSPI_SHADER_USER_DATA_VS_0 + 1);

  if (vertexInputInfoNode.getKind() == msgpack::Type::Nil) {
    return;
  }

  msgpack::MapDocNode &locationMap = vertexInputInfoNode.getMap(false);
  for (auto locationInfo : locationMap) {
    unsigned location = locationInfo.first.getUInt();

    msgpack::MapDocNode &componentMap = locationInfo.second.getMap(false);
    for (auto componentInfo : componentMap) {
      unsigned component = componentInfo.first.getUInt();
      msgpack::ArrayDocNode typeInfoMsg = componentInfo.second.getArray(false);
      lgc::VertexInputTypeInfo typeInfo;
      typeInfo.elementType = static_cast<lgc::BasicVertexInputType>(typeInfoMsg[0].getUInt());
      typeInfo.vectorSize = typeInfoMsg[1].getUInt();
      vsInterfaceData->setVertexInputType(location, component, typeInfo);
    }
  }
}

template class ElfWriter<Elf64>;

} // namespace Llpc
