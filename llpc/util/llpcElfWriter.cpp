/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcContext.h"
#include "llpcError.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include <algorithm>
#include <string.h>

#define DEBUG_TYPE "llpc-elf-writer"

using namespace llvm;
using namespace Vkgc;
using namespace Util;

namespace Llpc {
// The names of API shader stages used in PAL metadata, in ShaderStage order.
static const char *const ApiStageNames[] = {".task",     ".vertex", ".hull",  ".domain",
                                            ".geometry", "mesh",    ".pixel", ".compute"};

// The names of hardware shader stages used in PAL metadata, in Util::Abi::HardwareStage order.
static const char *const HwStageNames[] = {".ls", ".hs", ".es", ".gs", ".vs", ".ps", ".cs"};

// The suffix added to the rodata sections from the cached elf bin
static const char CachedRodataSectionSuffix[] = ".cached";
// The suffix added to the symbols of the rodata sections from the cached elf bin
static const char CachedRodataSymbolSuffix[] = "_cached";

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
    // NOTE: All disassemble section don't have any alignment requirement, so it happen only if we merge
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
// A workaround to support erase in llvm::msgpack::MapDocNode.
class MapDocNode : public msgpack::MapDocNode {
public:
  MapTy::iterator erase(MapTy::iterator where) { return Map->erase(where); }
};

// =====================================================================================================================
// Merges the map item pair from source map to destination map for llvm::msgpack::MapDocNode.
//
// @param [in/out] destMap : Destination map
// @param srcMap : Source map
// @param key : Key to check in source map
template <class Elf>
void ElfWriter<Elf>::mergeMapItem(msgpack::MapDocNode &destMap, msgpack::MapDocNode &srcMap, unsigned key) {
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
// @param context : Context related to ElfNote
// @param [in/out] document : The parsed message pack document of the metadata note.
static void updateRootDescriptorRegisters(Context *context, msgpack::Document &document) {
  auto pipeline = document.getRoot().getMap(true)[PalAbi::CodeObjectMetadataKey::Pipelines].getArray(true)[0];
  auto registers = pipeline.getMap(true)[PalAbi::PipelineMetadataKey::Registers].getMap(true);

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
          const ResourceMappingData *resourceMapping = nullptr;
          if (baseRegister == mmComputeUserData0) {
            auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
            resourceMapping = &pipelineInfo->resourceMapping;
          } else {
            auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
            resourceMapping = &pipelineInfo->resourceMapping;
          }
          unsigned set = regValue & DescSetMask;
          for (unsigned j = 0; j < resourceMapping->userDataNodeCount; ++j) {
            auto userDataNode = &resourceMapping->pUserDataNodes[j].node;
            if (userDataNode->type == ResourceMappingNodeType::DescriptorTableVaPtr &&
                set == userDataNode->tablePtr.pNext[0].srdRange.set) {
              // If it's descriptor user data, then update its offset to it.
              unsigned value = userDataNode->offsetInDwords;
              keyIt->second = registers.getDocument()->getNode(value);
              // Update userDataLimit if necessary
              unsigned userDataLimit = pipeline.getMap(true)[PalAbi::PipelineMetadataKey::UserDataLimit].getUInt();
              pipeline.getMap(true)[PalAbi::PipelineMetadataKey::UserDataLimit] =
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

  auto destPipeline = destDocument.getRoot().getMap(true)[PalAbi::CodeObjectMetadataKey::Pipelines].getArray(true)[0];
  auto srcPipeline = srcDocument.getRoot().getMap(true)[PalAbi::CodeObjectMetadataKey::Pipelines].getArray(true)[0];

  // Copy .num_interpolants
  auto srcNumIterpIt = srcPipeline.getMap(true).find(StringRef(PalAbi::PipelineMetadataKey::NumInterpolants));
  if (srcNumIterpIt != srcPipeline.getMap(true).end())
    destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::NumInterpolants] = srcNumIterpIt->second;

  // Copy .spill_threshold
  auto destSpillThreshold = destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::SpillThreshold].getUInt();
  auto srcSpillThreshold = srcPipeline.getMap(true)[PalAbi::PipelineMetadataKey::SpillThreshold].getUInt();
  destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::SpillThreshold] =
      destDocument.getNode(std::min(srcSpillThreshold, destSpillThreshold));

  // Copy .user_data_limit
  auto destUserDataLimit = destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::UserDataLimit].getUInt();
  auto srcUserDataLimit = srcPipeline.getMap(true)[PalAbi::PipelineMetadataKey::UserDataLimit].getUInt();
  destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::UserDataLimit] =
      destDocument.getNode(std::max(destUserDataLimit, srcUserDataLimit));

  // Copy whole .ps hw stage
  auto destHwStages = destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::HardwareStages].getMap(true);
  auto srcHwStages = srcPipeline.getMap(true)[PalAbi::PipelineMetadataKey::HardwareStages].getMap(true);
  auto hwPsStageName = HwStageNames[static_cast<unsigned>(Util::Abi::HardwareStage::Ps)];
  destHwStages[hwPsStageName] = srcHwStages[hwPsStageName];

  // Copy whole .pixel shader
  auto destShaders = destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::Shaders].getMap(true);
  auto srcShaders = srcPipeline.getMap(true)[PalAbi::PipelineMetadataKey::Shaders].getMap(true);
  destShaders[ApiStageNames[ShaderStageFragment]] = srcShaders[ApiStageNames[ShaderStageFragment]];

  // Update pipeline hash
  auto pipelineHash = destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
  pipelineHash[0] = destDocument.getNode(pContext->getPipelineHashCode());
  pipelineHash[1] = destDocument.getNode(pContext->getPipelineHashCode());

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 723
  // Update .ps_sample_mask
  // NOTE: We need to erase the node if the cached ELF has it but we actually don't need it.
  auto srcPsSampleMask = srcPipeline.getMap(true).find(StringRef(PalAbi::PipelineMetadataKey::PsSampleMask));
  auto destPsSampleMask = destPipeline.getMap(true).find(StringRef(PalAbi::PipelineMetadataKey::PsSampleMask));
  if (srcPsSampleMask != srcPipeline.getMap(true).end())
    destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::PsSampleMask] = srcPsSampleMask->second;
  else if (destPsSampleMask != destPipeline.getMap(true).end())
    destPipeline.getMap(true).erase(destPsSampleMask);
#endif

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
  };

  // Merge fragment shader related registers. For each of the registers listed above, plus the input
  // control registers and the user data registers, copy the value from srcRegisters to destRegisters.
  // Where the register is set in destRegisters but not srcRegisters, clear it.
  auto destRegisters = destPipeline.getMap(true)[PalAbi::PipelineMetadataKey::Registers].getMap(true);
  auto srcRegisters = srcPipeline.getMap(true)[PalAbi::PipelineMetadataKey::Registers].getMap(true);

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
template <class Elf> ElfNote ElfWriter<Elf>::getNote(uint32_t noteType) {
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
  memset(data, 0, std::max(noteSize, noteHeaderSize));
  noteSection->data = data;
  noteSection->secHead.sh_size = noteSize;

  for (auto &note : m_notes) {
    memcpy(data, &note.hdr, noteHeaderSize);
    data += noteHeaderSize;
    memcpy(data, &note.hdr.name, note.hdr.nameSize);
    data += alignTo(note.hdr.nameSize, sizeof(unsigned));
    memcpy(data, note.data, note.hdr.descSize);
    data += alignTo(note.hdr.descSize, sizeof(unsigned));
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
    if (sizeBytes > 0)
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
// @returns : Success if the section was found, NotFound otherwise
template <class Elf>
Result ElfWriter<Elf>::getSectionDataBySectionIndex(unsigned secIdx, const SectionBuffer **ppSectionData) const {
  Result result = Result::NotFound;
  if (secIdx < m_sections.size()) {
    *ppSectionData = &m_sections[secIdx];
    result = Result::Success;
  }
  return result;
}

// =====================================================================================================================
// Update descriptor offset to USER_DATA in metaNote.
//
// @param pContext : Context related to ElfNote
// @param pNote : Note section to update
// @param [out] pNewNote : New note section
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
// Get the number of relocs in the reloc section
//
// @param relocSection : Reloc sectionN
// @returns : Number of the relocs
template <class Elf> size_t ElfWriter<Elf>::numRelocs(const SectionBuffer *relocSection) {
  if (relocSection->secHead.sh_size == 0)
    return 0;

  return static_cast<unsigned>(relocSection->secHead.sh_size / relocSection->secHead.sh_entsize);
}

// =====================================================================================================================
// Get the start pos of relocs used by PS in the reloc section
//
// @param relocSection : Reloc section
// @param psIsaOffset : Ps ISA offset in the elf bin
// @returns : Start pos of the relocs used by PS in the reloc section
template <class Elf> size_t ElfWriter<Elf>::getRelocPsStartPos(const SectionBuffer *relocSection, size_t psIsaOffset) {
  size_t relocCount = numRelocs(relocSection);
  if (relocCount == 0)
    return 0;

  size_t psStartPos = 0;
  auto relocs = reinterpret_cast<typename Elf::Reloc *>(const_cast<uint8_t *>(relocSection->data));
  for (; psStartPos < relocCount; ++psStartPos)
    if (relocs[psStartPos].r_offset >= psIsaOffset)
      break;

  return psStartPos;
}

// =====================================================================================================================
// Create a new empty section based on the input section
//
// @param name : Name of the new section, the name should be point to the part of the elf bin
// @param inputSection : The input section
// @returns : New section
template <class Elf>
typename ElfWriter<Elf>::SectionBuffer ElfWriter<Elf>::createNewSection(const char *sectionName,
                                                                        SectionBuffer *inputSection) {
  SectionBuffer newSection = {};
  newSection.secHead = inputSection->secHead;

  // Add the section name to .strtab section.
  const unsigned strTabSize = m_sections[m_strtabSecIdx].secHead.sh_size;
  const unsigned secNameSize = strlen(const_cast<char *>(sectionName)) + 1;

  newSection.secHead.sh_name = strTabSize;
  newSection.name = const_cast<char *>(sectionName);

  newSection.secHead.sh_link = m_symSecIdx;
  newSection.secHead.sh_size = 0;

  // The memory will be freed in ElfWrith destructor.
  auto strTabData = new uint8_t[strTabSize + secNameSize];
  memcpy(strTabData, m_sections[m_strtabSecIdx].data, strTabSize);
  memcpy(strTabData + strTabSize, sectionName, secNameSize);

  delete[] m_sections[m_strtabSecIdx].data;

  m_sections[m_strtabSecIdx].data = strTabData;
  m_sections[m_strtabSecIdx].secHead.sh_size = strTabSize + secNameSize;

  return newSection;
}

// =====================================================================================================================
// Add the reloc related symbols and sections based on the input reloc
//
// @param reader : The ElfReader to the input fragment elf bin
// @param inputReloc : The input reloc
template <class Elf> void ElfWriter<Elf>::addRelocSymbols(const ElfReader<Elf> &reader, ElfReloc inputReloc) {
  ElfSymbol relocSym = {};
  reader.getSymbol(inputReloc.r_symbol, &relocSym);

  // Create a section named origSectionName.cached.
  // If the section does not exist, the memory will be freed in ElfWrith destructor.
  const size_t inputSecNameLen = strlen(relocSym.secName);
  const size_t newSecNameSize = inputSecNameLen + strlen(CachedRodataSectionSuffix) + 1;
  char *secName = new char[newSecNameSize];
  memset(secName, 0, newSecNameSize);
  memcpy(secName, relocSym.secName, inputSecNameLen);
  memcpy(secName + inputSecNameLen, CachedRodataSectionSuffix, newSecNameSize - inputSecNameLen);

  for (auto section : m_sections) {
    if (strcmp(section.name, secName) == 0) {
      delete[] secName;
      return;
    }
  }

  // If the section does not exist, create a new section and update the related symbols.
  SectionBuffer *inputRodataSection = nullptr;
  std::vector<ElfSymbol> inputRodataSymbols;
  auto inputRodataSecIndex = reader.GetSectionIndex(relocSym.secName);
  Result result = reader.getSectionDataBySectionIndex(inputRodataSecIndex, &inputRodataSection);
  assert(result == Result::Success || result == Result::ErrorInvalidValue);
  (void)result;
  reader.GetSymbolsBySectionIndex(inputRodataSecIndex, inputRodataSymbols);

  SectionBuffer newSection = createNewSection(secName, inputRodataSection);

  // Update the data from the input section.
  // The memory will be freed in ElfWrith destructor.
  auto secData = new uint8_t[inputRodataSection->secHead.sh_size];
  memcpy(secData, inputRodataSection->data, inputRodataSection->secHead.sh_size);
  newSection.data = secData;
  newSection.secHead.sh_size = inputRodataSection->secHead.sh_size;

  m_sections.push_back(newSection);

  // Update symbols.
  for (auto rodataSymbol : inputRodataSymbols) {
    ElfSymbol symbol = {};
    // Create a new symbol named origSymbolName_cached.
    // The memory will be freed in ElfWrith destructor.
    const size_t inputSymbolNameLen = strlen(rodataSymbol.pSymName);
    const size_t newSymSize = inputSymbolNameLen + strlen(CachedRodataSymbolSuffix) + 1;
    char *newSymName = new char[newSymSize];
    memset(newSymName, 0, newSymSize);
    memcpy(newSymName, rodataSymbol.pSymName, inputSymbolNameLen);
    memcpy(newSymName + inputSymbolNameLen, CachedRodataSymbolSuffix, newSymSize - inputSymbolNameLen);
    symbol.secIdx = m_sections.size() - 1;
    symbol.secName = secName;
    symbol.size = rodataSymbol.size;
    symbol.pSymName = newSymName;
    symbol.value = rodataSymbol.value;
    symbol.info.all = rodataSymbol.info.all;
    symbol.nameOffset = InvalidValue;
    m_symbols.push_back(symbol);
  }
}

// =====================================================================================================================
// Get the reloc symbol index based on the input reloc symbol
//
// @param inputSymbolName : The input reloc symbol name
// @returns : Reloc symbol index
template <class Elf> uint32_t ElfWriter<Elf>::getRelocSymbolIndex(const char *inputSymbolName) {
  const size_t inputSymbolNameLen = strlen(inputSymbolName);
  const size_t newSymSize = inputSymbolNameLen + strlen(CachedRodataSymbolSuffix) + 1;
  char *newSymName = new char[newSymSize];
  memset(newSymName, 0, newSymSize);
  memcpy(newSymName, inputSymbolName, inputSymbolNameLen);
  memcpy(newSymName + inputSymbolNameLen, CachedRodataSymbolSuffix, newSymSize - inputSymbolNameLen);

  uint32_t index = 0;
  for (auto symbol : m_symbols) {
    if (symbol.secIdx == InvalidValue)
      continue;
    if (symbol.nameOffset == InvalidValue && strcmp(newSymName, symbol.pSymName) == 0)
      break;
    ++index;
  }

  delete[] newSymName;

  return index;
}

// =====================================================================================================================
// Merge base reloc section and input reloc section into merged section
//
// @param reader : The ElfReader to the input fragment elf bin
// @param section1 : The first reloc section buffer to merge
// @param section1RelocsCount : Relocs count of the first section
// @param section2 : The second reloc section buffer to merge
// @param section2RelocsOffset : Relocs count offset of the second section
// @param diffOfPsOffset : The delta of ps Isa offset of the two sections
// @returns : New section
template <class Elf>
typename ElfWriter<Elf>::SectionBuffer
ElfWriter<Elf>::mergeRelocSection(const ElfReader<Elf> &reader, const SectionBuffer *section1,
                                  size_t section1RelocsCount, const SectionBuffer *section2,
                                  size_t section2RelocsOffset, int64_t diffOfPsOffset) {
  SectionBuffer newSection = {};

  newSection = *section1;

  // The second section is null and the relocs count of the first section is zero,
  // no data need to be merged, return a null section.
  if (!section2 && (section1RelocsCount == 0)) {
    newSection.data = nullptr;
    newSection.secHead.sh_size = 0;
    return newSection;
  }

  size_t section1Size = section1RelocsCount * section1->secHead.sh_entsize;

  size_t newSectionSize = section1Size;
  size_t relocsCount = section1RelocsCount;

  if (section2) {
    size_t section2Offset = section2RelocsOffset * section2->secHead.sh_entsize;
    newSectionSize += (section2->secHead.sh_size - section2Offset);
    relocsCount += (numRelocs(section2) - section2RelocsOffset);
  }

  // The memory will be freed in ElfWrith destructor.
  auto mergedData = new uint8_t[newSectionSize];

  newSection.data = mergedData;
  newSection.secHead.sh_size = newSectionSize;

  auto data = mergedData;

  if (section1Size > 0) {
    memcpy(data, section1->data, section1Size);
    data += section1Size;
  }

  if (section2) {
    auto section2Relocs = reinterpret_cast<typename Elf::Reloc *>(const_cast<uint8_t *>(section2->data));
    size_t section2RelocsCount = numRelocs(section2);
    size_t relocSize = section2->secHead.sh_entsize;

    for (auto i = section2RelocsOffset; i < section2RelocsCount; ++i) {
      addRelocSymbols(reader, section2Relocs[i]);

      // Merge the relocs data.
      typename Elf::Reloc reloc = {};
      reloc.r_offset = section2Relocs[i].r_offset + diffOfPsOffset;
      reloc.r_type = section2Relocs[i].r_type;
      ElfSymbol section2RelocSymbol = {};
      reader.getSymbol(section2Relocs[i].r_symbol, &section2RelocSymbol);
      reloc.r_symbol = getRelocSymbolIndex(section2RelocSymbol.pSymName);
      memcpy(data, &reloc, relocSize);
      data += relocSize;
    }
  }

  return newSection;
}

// =====================================================================================================================
// Process the reloc section
//
// @param reader : The ElfReader to input fragment elf bin
// @param nonFragmentPsIsaOffset : The offset of PS ISA in nonFragment elf bin
// @param fragmentPsIsaOffset : The offset of PS ISA in input fragment elf bin
template <class Elf>
void ElfWriter<Elf>::processRelocSection(const ElfReader<Elf> &reader, size_t nonFragmentPsIsaOffset,
                                         size_t fragmentPsIsaOffset) {
  SectionBuffer *fragmentRelocSection = nullptr;
  const SectionBuffer *nonFragmentRelocSection = nullptr;

  auto fragmentRelocSecIndex = reader.GetSectionIndex(RelocName);
  auto nonFragmentRelocSecIndex = GetSectionIndex(RelocName);

  Result result = reader.getSectionDataBySectionIndex(fragmentRelocSecIndex, &fragmentRelocSection);
  assert(result == Result::Success || result == Result::ErrorInvalidValue);
  (void)result;
  result = getSectionDataBySectionIndex(nonFragmentRelocSecIndex, &nonFragmentRelocSection);
  assert(result == Result::Success || result == Result::NotFound);
  (void)result;

  bool isFragmentRelocUsedInPs = false;

  // The start pos of the relocs which used by PS.
  size_t fragmentRelocPsStartPos = 0;

  if (fragmentRelocSection) {
    size_t fragmentRelocCounts = numRelocs(fragmentRelocSection);
    fragmentRelocPsStartPos = getRelocPsStartPos(fragmentRelocSection, fragmentPsIsaOffset);
    isFragmentRelocUsedInPs = (fragmentRelocPsStartPos < fragmentRelocCounts);
  }

  if (!nonFragmentRelocSection && isFragmentRelocUsedInPs) {
    // The input fragment elf bin has relocs used in PS, but nonFragment elf bin does not have reloc section,
    // then a reloc section need to be created for nonFragment elf bin.
    SectionBuffer newRelocSection = createNewSection(RelocName, fragmentRelocSection);

    m_sections.push_back(newRelocSection);
    // Update the reloc section index.
    m_relocSecIdx = m_sections.size() - 1;
    m_map[RelocName] = m_relocSecIdx;
    nonFragmentRelocSecIndex = m_relocSecIdx;
    mustSucceed(getSectionDataBySectionIndex(nonFragmentRelocSecIndex, &nonFragmentRelocSection));
  }

  // Merge reloc section.
  if (nonFragmentRelocSection) {
    size_t nonFragmentRelocPsStartPos = getRelocPsStartPos(nonFragmentRelocSection, nonFragmentPsIsaOffset);

    // If the input fragment elf bin does not have relocs used by PS,
    // no need to merge any reloc from the input fragment elf bin.
    SectionBuffer *tmpFragmentRelocSection = isFragmentRelocUsedInPs ? fragmentRelocSection : nullptr;

    int64_t diffOfPsOffset = nonFragmentPsIsaOffset - fragmentPsIsaOffset;

    SectionBuffer newSection = mergeRelocSection(reader, nonFragmentRelocSection, nonFragmentRelocPsStartPos,
                                                 tmpFragmentRelocSection, fragmentRelocPsStartPos, diffOfPsOffset);

    setSection(nonFragmentRelocSecIndex, &newSection);
  }
}

// =====================================================================================================================
// Retrieves the section data for the specified section name, if it exists.
//
// @param [in] pName : Name of the section to look for
// @param [out] ppData : Pointer to section data
// @param [out] pDataLength : Size of the section data
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
// Update all associated symbols by section index.
//
// @param secIdx : Section index
// @param [out] secSymbols : ELF symbols
template <class Elf>
void ElfWriter<Elf>::updateSymbolsBySectionIndex(unsigned secIdx, std::vector<ElfSymbol> &secSymbols) {
  auto newEnd =
      std::remove_if(m_symbols.begin(), m_symbols.end(), [&secIdx](ElfSymbol &sym) { return sym.secIdx == secIdx; });
  m_symbols.erase(newEnd, m_symbols.end());
  for (auto &secSymbol : secSymbols)
    m_symbols.push_back(secSymbol);
}

// =====================================================================================================================
// Get text section shader data

// @param name: Shader stage entry-point names, such as "_amdgpu_vs_main","_amdgpu_gs_main", "_amdgpu_ps_main" etc
// @param [out] data: Data corresponding to GPU ISA code of this shader stage
// @param [out] dataLength: Length of the data
template <class Elf>
Result ElfWriter<Elf>::getSectionTextShader(const char *name, const void **data, size_t *dataLength) {
  auto textSectIdx = GetSectionIndex(".text");
  const ElfSectionBuffer<Elf64::SectionHeader> *textSection = nullptr;
  mustSucceed(getSectionDataBySectionIndex(textSectIdx, &textSection));
  std::vector<ElfSymbol *> symbols;
  GetSymbolsBySectionIndex(textSectIdx, symbols);
  for (auto symbol : symbols) {
    if (strcmp(symbol->pSymName, name) == 0) {
      *data = voidPtrInc(textSection->data, symbol->value);
      *dataLength = symbol->size;
      break;
    }
  }
  return Result::Success;
}

// =====================================================================================================================
// Update descriptor root offset in ELF binary
//
// @param pContext : Pipeline context
// @param [out] pPipelineElf : Final ELF binary
template <class Elf> void ElfWriter<Elf>::updateElfBinary(Context *pContext, ElfPackage *pPipelineElf) {
  // Merge PAL metadata
  ElfNote metaNote = {};
  metaNote = getNote(Util::Abi::MetadataNoteType);

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
  mustSucceed(reader.ReadFromBuffer(pFragmentElf->pCode, &fragmentCodesize));

  // Merge GPU ISA code
  const ElfSectionBuffer<Elf64::SectionHeader> *nonFragmentTextSection = nullptr;
  ElfSectionBuffer<Elf64::SectionHeader> *fragmentTextSection = nullptr;
  std::vector<ElfSymbol> fragmentSymbols;
  std::vector<ElfSymbol *> nonFragmentSymbols;

  auto fragmentTextSecIndex = reader.GetSectionIndex(TextName);
  auto nonFragmentSecIndex = GetSectionIndex(TextName);
  Result result = reader.getSectionDataBySectionIndex(fragmentTextSecIndex, &fragmentTextSection);
  assert(result == Result::Success || result == Result::ErrorInvalidValue);
  (void)result;
  reader.GetSymbolsBySectionIndex(fragmentTextSecIndex, fragmentSymbols);

  mustSucceed(getSectionDataBySectionIndex(nonFragmentSecIndex, &nonFragmentTextSection));
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

  // Merge ISA disassembly.
  auto fragmentDisassemblySecIndex = reader.GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
  auto nonFragmentDisassemblySecIndex = GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
  if (fragmentDisassemblySecIndex != InvalidValue && nonFragmentDisassemblySecIndex != InvalidValue) {
    ElfSectionBuffer<Elf64::SectionHeader> *fragmentDisassemblySection = nullptr;
    const ElfSectionBuffer<Elf64::SectionHeader> *nonFragmentDisassemblySection = nullptr;
    mustSucceed(reader.getSectionDataBySectionIndex(fragmentDisassemblySecIndex, &fragmentDisassemblySection));
    mustSucceed(getSectionDataBySectionIndex(nonFragmentDisassemblySecIndex, &nonFragmentDisassemblySection));

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

  // Merge LLVM IR disassembly.
  auto fragmentLlvmIrSecIndex = reader.GetSectionIndex(Util::Abi::AmdGpuCommentLlvmIrName);
  auto nonFragmentLlvmIrSecIndex = GetSectionIndex(Util::Abi::AmdGpuCommentLlvmIrName);
  if (fragmentLlvmIrSecIndex != InvalidValue && nonFragmentLlvmIrSecIndex != InvalidValue) {
    ElfSectionBuffer<Elf64::SectionHeader> *fragmentLlvmIrSection = nullptr;
    const ElfSectionBuffer<Elf64::SectionHeader> *nonFragmentLlvmIrSection = nullptr;
    mustSucceed(reader.getSectionDataBySectionIndex(fragmentLlvmIrSecIndex, &fragmentLlvmIrSection));
    mustSucceed(getSectionDataBySectionIndex(nonFragmentLlvmIrSecIndex, &nonFragmentLlvmIrSection));

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
  nonFragmentMetaNote = getNote(Util::Abi::MetadataNoteType);

  assert(nonFragmentMetaNote.data);
  ElfNote fragmentMetaNote = {};
  ElfNote newMetaNote = {};
  fragmentMetaNote = reader.getNote(Util::Abi::MetadataNoteType);
  mergeMetaNote(pContext, &nonFragmentMetaNote, &fragmentMetaNote, &newMetaNote);
  setNote(&newMetaNote);

  // Process reloc Section.
  processRelocSection(reader, isaOffset, fragmentIsaSymbol->value);

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
  m_map[StrTabName] = m_strtabSecIdx;

  m_textSecIdx = m_sections.size();
  m_sections.push_back({});
  m_map[TextName] = m_textSecIdx;

  m_symSecIdx = m_sections.size();
  m_sections.push_back({});
  m_map[SymTabName] = m_symSecIdx;

  m_noteSecIdx = m_sections.size();
  m_sections.push_back({});
  m_map[NoteName] = m_noteSecIdx;

  m_relocSecIdx = m_sections.size();
  m_sections.push_back({});
  m_map[RelocName] = m_relocSecIdx;
}

template class ElfWriter<Elf64>;

} // namespace Llpc
