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
#define DEBUG_TYPE "llpc-elf-writer"

#include <algorithm>
#include <string.h>
#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"

#include "llpcContext.h"
#include "llpcElfWriter.h"
#include "llpcGfx6Chip.h"
#include "llpcGfx9Chip.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
template<class Elf>
ElfWriter<Elf>::ElfWriter(
    GfxIpVersion gfxIp)         // Graphics IP version info
    :
    m_gfxIp(gfxIp),
    m_textSecIdx(InvalidValue),
    m_noteSecIdx(InvalidValue),
    m_symSecIdx(InvalidValue),
    m_strtabSecIdx(InvalidValue)
{
}

// =====================================================================================================================
template<class Elf>
ElfWriter<Elf>::~ElfWriter()
{
    for (auto& section : m_sections)
    {
        delete[] section.pData;
    }
    m_sections.clear();

    for (auto& note : m_notes)
    {
        delete[] note.pData;
    }
    m_notes.clear();

    for (auto& sym : m_symbols)
    {
        if (sym.nameOffset == InvalidValue)
        {
            delete[] sym.pSymName;
        }
    }
    m_symbols.clear();
}

// =====================================================================================================================
// Merge base section and input section into merged section
template<class Elf>
void ElfWriter<Elf>::MergeSection(
    const SectionBuffer*    pSection1,          // [in] The first section buffer to merge
    size_t                  section1Size,       // Byte size of the first section
    const char*             pPrefixString1,     // [in] Prefix string of the first section's contents
    const SectionBuffer*    pSection2,          // [in] The second section buffer to merge
    size_t                  section2Offset,     // Byte offset of the second section
    const char*             pPrefixString2,     // [in] Prefix string of the second section's contents
    SectionBuffer*          pNewSection)        // [out] Merged section
{
    std::string prefix1;
    std::string prefix2;

    // Build prefix1 if it is needed
    if (pPrefixString1 != nullptr)
    {
        if (strncmp(reinterpret_cast<const char*>(pSection1->pData),
                    pPrefixString1,
                    strlen(pPrefixString1)) != 0)
        {
            prefix1 = pPrefixString1;
            prefix1 += ":\n";
        }
    }

    // Build appendPrefixString if it is needed
    if (pPrefixString2 != nullptr)
    {
        if (strncmp(reinterpret_cast<const char*>(pSection2->pData + section2Offset),
                    pPrefixString2,
                    strlen(pPrefixString2)) != 0)
        {
            prefix2 = pPrefixString2;
            prefix2 += ":\n";
        }
    }

    // Build merged section
    size_t newSectionSize = section1Size +
                            (pSection2->secHead.sh_size - section2Offset) +
                            prefix1.length() + prefix2.length();

    auto pMergedData = new uint8_t[newSectionSize];
    *pNewSection = *pSection1;
    pNewSection->pData = pMergedData;
    pNewSection->secHead.sh_size = newSectionSize;

    auto pData = pMergedData;

    // Copy prefix1
    if (prefix1.length() > 0)
    {
        memcpy(pData, prefix1.data(), prefix1.length());
        pData += prefix1.length();
    }

    // Copy base section content
    auto baseCopySize = std::min(section1Size, static_cast<size_t>(pSection1->secHead.sh_size));
    memcpy(pData, pSection1->pData, baseCopySize);
    pData += baseCopySize;

    // Fill alignment data with NOP instruction to match backend's behavior
    if (baseCopySize < section1Size)
    {
        // NOTE: All disassemble section don't have any alignmeent requirement, so it happen only if we merge
        // .text section.
        constexpr uint32_t Nop = 0xBF800000;
        uint32_t* pDataDw = reinterpret_cast<uint32_t*>(pData);
        for (uint32_t i = 0; i < (section1Size - baseCopySize) / sizeof(uint32_t); ++i)
        {
            pDataDw[i] = Nop;
        }
        pData += (section1Size - baseCopySize);
    }

    // Copy append prefix
    if (prefix2.length() > 0)
    {
        memcpy(pData, prefix2.data(), prefix2.length());
        pData += prefix2.length();
    }

    // Copy append section content
    memcpy(pData,
           pSection2->pData + section2Offset,
           pSection2->secHead.sh_size - section2Offset);
}

// =====================================================================================================================
// A woarkaround to support erase in llvm::msgpack::MapDocNode.
class MapDocNode : public llvm::msgpack::MapDocNode
{
public:
    MapTy::iterator erase(MapTy::iterator _Where)
    {
        return Map->erase(_Where);
    }
};

// =====================================================================================================================
// Merges the map item pair from source map to destination map for llvm::msgpack::MapDocNode.
template<class Elf>
void ElfWriter<Elf>::MergeMapItem(
    llvm::msgpack::MapDocNode& destMap,    // [in,out] Destination map
    llvm::msgpack::MapDocNode& srcMap,     // [in] Source map
    uint32_t                    key)       // Key to check in source map
{
    auto srcKeyNode = srcMap.getDocument()->getNode(key);
    auto srcIt = srcMap.find(srcKeyNode);
    if (srcIt != srcMap.end())
    {
        LLPC_ASSERT(srcIt->first.getUInt() == key);
        destMap[destMap.getDocument()->getNode(key)] = srcIt->second;
    }
    else
    {
        auto destKeyNode = destMap.getDocument()->getNode(key);
        auto destIt = destMap.find(destKeyNode);
        if (destIt != destMap.end())
        {
            LLPC_ASSERT(destIt->first.getUInt() == key);
            static_cast<MapDocNode&>(destMap).erase(destIt);
        }
    }
}

// =====================================================================================================================
// Merges fragment shader related info for meta notes.
template<class Elf>
void ElfWriter<Elf>::MergeMetaNote(
    Context*       pContext,       // [in] The first note section to merge
    const ElfNote* pNote1,         // [in] The second note section to merge (contain fragment shader info)
    const ElfNote* pNote2,         // [in] Note section contains fragment shader info
    ElfNote*       pNewNote)       // [out] Merged note section
{
    msgpack::Document destDocument;
    msgpack::Document srcDocument;

    auto success = destDocument.readFromBlob(StringRef(reinterpret_cast<const char*>(pNote1->pData),
                                                       pNote1->hdr.descSize),
                                             false);
    LLPC_ASSERT(success);

    success = srcDocument.readFromBlob(StringRef(reinterpret_cast<const char*>(pNote2->pData),
                                                 pNote2->hdr.descSize),
                                       false);
    LLPC_ASSERT(success);
    LLPC_UNUSED(success);

    auto destPipeline = destDocument.getRoot().
        getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0];
    auto srcPipeline = srcDocument.getRoot().
        getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0];

    // Copy .num_interpolants
    auto srcNumIterpIt = srcPipeline.getMap(true).find(StringRef(Util::Abi::PipelineMetadataKey::NumInterpolants));
    if (srcNumIterpIt != srcPipeline.getMap(true).end())
    {
        destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::NumInterpolants] = srcNumIterpIt->second;
    }

    // Copy .spill_threshold
    auto destSpillThreshold = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::SpillThreshold].getUInt();
    auto srcSpillThreshold = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::SpillThreshold].getUInt();
    destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::SpillThreshold] =
        destDocument.getNode(std::min(srcSpillThreshold, destSpillThreshold));

    // Copy .user_data_limit
    auto destUserDataLimit = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit].getUInt();
    auto srcUserDataLimit = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit].getUInt();
    destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::UserDataLimit] =
        destDocument.getNode(std::max(destUserDataLimit, srcUserDataLimit));

    // Copy whole .ps hw stage
    auto destHwStages = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::HardwareStages].getMap(true);
    auto srcHwStages = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::HardwareStages].getMap(true);
    auto pHwPsStageName = HwStageNames[static_cast<uint32_t>(Util::Abi::HardwareStage::Ps)];
    destHwStages[pHwPsStageName] = srcHwStages[pHwPsStageName];

    // Copy whole .pixel shader
    auto destShaders = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Shaders].getMap(true);
    auto srcShaders = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Shaders].getMap(true);
    destShaders[ApiStageNames[ShaderStageFragment]] = srcShaders[ApiStageNames[ShaderStageFragment]];

    // Update pipeline hash
    auto pipelineHash = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
    pipelineHash[0] = destDocument.getNode(pContext->GetPiplineHashCode());
    pipelineHash[1] = destDocument.getNode(pContext->GetPiplineHashCode());

    // Merge fragment shader related registers
    auto destRegisters = destPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Registers].getMap(true);
    auto srcRegisters = srcPipeline.getMap(true)[Util::Abi::PipelineMetadataKey::Registers].getMap(true);

    auto gfxIp = pContext->GetGfxIpVersion();
    Gfx6::PsRegConfig gfx6PsConfig;
    Gfx9::PsRegConfig gfx9PsConfig(gfxIp);
    Util::Abi::PalMetadataNoteEntry* pRegEntry = nullptr;
    uint32_t regCount = 0;
    uint32_t psInputCntlBase = 0;
    uint32_t psUserDataBase = 0;
    uint32_t psUserDataCount = 0;
    if (gfxIp.major < 9)
    {
        pRegEntry = reinterpret_cast<Util::Abi::PalMetadataNoteEntry*>(&gfx6PsConfig);
        psInputCntlBase = gfx6PsConfig.GetPsInputCntlStart();
        psUserDataBase = gfx6PsConfig.GetPsUserDataStart();
        psUserDataCount = 16;
        regCount = sizeof(gfx6PsConfig) / sizeof(Util::Abi::PalMetadataNoteEntry);
    }
    else
    {
        pRegEntry = reinterpret_cast<Util::Abi::PalMetadataNoteEntry*>(&gfx9PsConfig);
        psInputCntlBase = Gfx9::mmSPI_PS_INPUT_CNTL_0;
        psUserDataBase = Gfx9::mmSPI_SHADER_USER_DATA_PS_0;
        psUserDataCount = 32;
        regCount = sizeof(gfx9PsConfig) / sizeof(Util::Abi::PalMetadataNoteEntry);
    }

    for (uint32_t i = 0; i < regCount; ++i)
    {
        if (pRegEntry[i].key != InvalidMetadataKey)
        {
            MergeMapItem(destRegisters, srcRegisters, pRegEntry[i].key);
        }
    }

    constexpr uint32_t PsInputCntlCount = 32;
    for (uint32_t i = 0; i < PsInputCntlCount; ++i)
    {
        MergeMapItem(destRegisters, srcRegisters, psInputCntlBase + i);
    }

    for (uint32_t i = 0; i < psUserDataCount; ++i)
    {
        MergeMapItem(destRegisters, srcRegisters, psUserDataBase + i);
    }

    std::string destBlob;
    destDocument.writeToBlob(destBlob);
    *pNewNote = *pNote1;
    auto pData = new uint8_t[destBlob.size() + 4]; // 4 is for additional alignment spece
    memcpy(pData, destBlob.data(), destBlob.size());
    pNewNote->hdr.descSize = destBlob.size();
    pNewNote->pData = pData;
}

// =====================================================================================================================
// Gets symbol according to symbol name, and creates a new one if it doesn't exist.
template<class Elf>
ElfSymbol* ElfWriter<Elf>::GetSymbol(
    const char* pSymbolName)   // [in] Symbol name
{
    for (auto& symbol : m_symbols)
    {
        if (strcmp(pSymbolName, symbol.pSymName) == 0)
        {
            return &symbol;
        }
    }

    // Create new symbol
    ElfSymbol newSymbol = {};
    char* pNewSymName = new char[strlen(pSymbolName) + 1];
    strcpy(pNewSymName, pSymbolName);

    newSymbol.pSymName = pNewSymName;
    newSymbol.nameOffset = InvalidValue;
    newSymbol.secIdx = InvalidValue;
    newSymbol.info.type = STT_FUNC;
    newSymbol.info.binding = STB_LOCAL;
    m_symbols.push_back(newSymbol);

    return &m_symbols.back();
}

// =====================================================================================================================
// Gets note according to noteType.
template<class Elf>
ElfNote ElfWriter<Elf>::GetNote(
    Util::Abi::PipelineAbiNoteType noteType)  // Note type
{
    for (auto& note : m_notes)
    {
        if (note.hdr.type == noteType)
        {
            return note;
        }
    }

    ElfNote note = {};
    return note;
}

// =====================================================================================================================
// Replaces exist note with input note according to input note type.
template<class Elf>
void ElfWriter<Elf>::SetNote(
    ElfNote* pNote)   // [in] Input note
{
    for (auto& note : m_notes)
    {
        if (note.hdr.type == pNote->hdr.type)
        {
            LLPC_ASSERT(note.pData != pNote->pData);
            delete[] note.pData;
            note = *pNote;
            return;
        }
    }

    LLPC_NEVER_CALLED();
}

// =====================================================================================================================
// Replace exist section with input section according to section index
template<class Elf>
void ElfWriter<Elf>::SetSection(
    uint32_t          secIndex,   //  Section index
    SectionBuffer*    pSection)   // [in] Input section
{
    LLPC_ASSERT(secIndex < m_sections.size());
    LLPC_ASSERT(pSection->pName == m_sections[secIndex].pName);
    LLPC_ASSERT(pSection->pData != m_sections[secIndex].pData);

    delete[] m_sections[secIndex].pData;
    m_sections[secIndex] = *pSection;
}

// =====================================================================================================================
// Determines the size needed for a memory buffer to store this ELF.
template<class Elf>
size_t ElfWriter<Elf>::GetRequiredBufferSizeBytes()
{
    // Update offsets and size values
    CalcSectionHeaderOffset();

    size_t totalBytes = sizeof(typename Elf::FormatHeader);

    // Iterate through the section list
    for (auto& section : m_sections)
    {
        totalBytes += Pow2Align(section.secHead.sh_size, sizeof(uint32_t));
    }

    totalBytes += m_header.e_shentsize * m_header.e_shnum;
    totalBytes += m_header.e_phentsize * m_header.e_phnum;

    return totalBytes;
}

// =====================================================================================================================
// Assembles ELF notes and add them to .note section
template<class Elf>
void ElfWriter<Elf>::AssembleNotes()
{
    if (m_noteSecIdx == InvalidValue)
    {
        return;
    }
    auto pNoteSection = &m_sections[m_noteSecIdx];
    const uint32_t noteHeaderSize = sizeof(NoteHeader) - 8;
    uint32_t noteSize = 0;
    for (auto& note : m_notes)
    {
        const uint32_t noteNameSize = Pow2Align(note.hdr.nameSize, sizeof(uint32_t));
        noteSize += noteHeaderSize + noteNameSize + Pow2Align(note.hdr.descSize, sizeof(uint32_t));
    }

    delete[] pNoteSection->pData;
    uint8_t* pData = new uint8_t[std::max(noteSize, noteHeaderSize)];
    LLPC_ASSERT(pData != nullptr);
    pNoteSection->pData = pData;
    pNoteSection->secHead.sh_size = noteSize;

    for (auto& note : m_notes)
    {
        memcpy(pData, &note.hdr, noteHeaderSize);
        pData += noteHeaderSize;
        const uint32_t noteNameSize = Pow2Align(note.hdr.nameSize, sizeof(uint32_t));
        memcpy(pData, &note.hdr.name, noteNameSize);
        pData += noteNameSize;
        const uint32_t noteDescSize = Pow2Align(note.hdr.descSize, sizeof(uint32_t));
        memcpy(pData, note.pData, noteDescSize);
        pData += noteDescSize;
    }

    LLPC_ASSERT(pNoteSection->secHead.sh_size == static_cast<uint32_t>(pData - pNoteSection->pData));
}

// =====================================================================================================================
// Assembles ELF symbols and symbol info to .symtab section
template<class Elf>
void ElfWriter<Elf>::AssembleSymbols()
{
    if (m_symSecIdx == InvalidValue)
    {
        return;
    }
    auto pStrTabSection = &m_sections[m_strtabSecIdx];
    auto newStrTabSize = 0;
    uint32_t symbolCount = 0;
    for (auto& symbol : m_symbols)
    {
        if (symbol.nameOffset == InvalidValue)
        {
            newStrTabSize += strlen(symbol.pSymName) + 1;
        }

        if (symbol.secIdx != InvalidValue)
        {
            symbolCount++;
        }
    }

    if (newStrTabSize > 0)
    {
        uint32_t strTabOffset = pStrTabSection->secHead.sh_size;
        auto pStrTabBuffer = new uint8_t[pStrTabSection->secHead.sh_size + newStrTabSize];
        memcpy(pStrTabBuffer, pStrTabSection->pData, pStrTabSection->secHead.sh_size);
        delete[] pStrTabSection->pData;

        pStrTabSection->pData = pStrTabBuffer;
        pStrTabSection->secHead.sh_size += newStrTabSize;

        for (auto& symbol : m_symbols)
        {
            if (symbol.nameOffset == InvalidValue)
            {
                auto symNameSize = strlen(symbol.pSymName) + 1;
                memcpy(pStrTabBuffer + strTabOffset, symbol.pSymName, symNameSize);
                symbol.nameOffset = strTabOffset;
                delete[] symbol.pSymName;
                symbol.pSymName = reinterpret_cast<const char*>(pStrTabBuffer + strTabOffset);
                strTabOffset += symNameSize;
            }
        }
    }

    auto symSectionSize = sizeof(typename Elf::Symbol) * symbolCount;
    auto pSymbolSection = &m_sections[m_symSecIdx];

    if (pSymbolSection->pData == nullptr)
    {
        pSymbolSection->pData = new uint8_t[symSectionSize];
    }
    else if (symSectionSize > pSymbolSection->secHead.sh_size)
    {
        delete[] pSymbolSection->pData;
        pSymbolSection->pData = new uint8_t[symSectionSize];
    }
    pSymbolSection->secHead.sh_size = symSectionSize;

    auto pSymbol = reinterpret_cast<typename Elf::Symbol*>(const_cast<uint8_t*>(pSymbolSection->pData));
    for (auto& symbol : m_symbols)
    {
        if (symbol.secIdx != InvalidValue)
        {
            pSymbol->st_name = symbol.nameOffset;
            pSymbol->st_info.all = symbol.info.all;
            pSymbol->st_other = 0;
            pSymbol->st_shndx = symbol.secIdx;
            pSymbol->st_value = symbol.value;
            pSymbol->st_size = symbol.size;
            ++pSymbol;
        }
    }

    LLPC_ASSERT(pSymbolSection->secHead.sh_size ==
                static_cast<uint32_t>(reinterpret_cast<uint8_t*>(pSymbol) - pSymbolSection->pData));
}

// =====================================================================================================================
// Determines the offset of the section header table by totaling the sizes of each binary chunk written to the ELF file,
// accounting for alignment.
template<class Elf>
void ElfWriter<Elf>::CalcSectionHeaderOffset()
{
    uint32_t sharedHdrOffset = 0;

    const uint32_t elfHdrSize = sizeof(typename Elf::FormatHeader);
    const uint32_t pHdrSize = sizeof(typename Elf::Phdr);

    sharedHdrOffset += elfHdrSize;
    sharedHdrOffset += m_header.e_phnum * pHdrSize;

    for (auto& section : m_sections)
    {
        const uint32_t secSzBytes = Pow2Align(section.secHead.sh_size, sizeof(uint32_t));
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
template<class Elf>
void ElfWriter<Elf>::WriteToBuffer(
    ElfPackage* pElf)   // [in] Output buffer to write ELF data
{
    LLPC_ASSERT(pElf != nullptr);

    // Update offsets and size values
    AssembleNotes();
    AssembleSymbols();

    const size_t reqSize = GetRequiredBufferSizeBytes();
    pElf->resize(reqSize);
    auto pData = pElf->data();
    memset(pData, 0, reqSize);

    char* pBuffer = static_cast<char*>(pData);

    // ELF header comes first
    const uint32_t elfHdrSize = sizeof(typename Elf::FormatHeader);
    memcpy(pBuffer, &m_header, elfHdrSize);
    pBuffer += elfHdrSize;

    LLPC_ASSERT(m_header.e_phnum == 0);

    // Write each section buffer
    for (auto& section : m_sections)
    {
        section.secHead.sh_offset = static_cast<uint32_t>(pBuffer - pData);
        const uint32_t sizeBytes = section.secHead.sh_size;
        memcpy(pBuffer, section.pData, sizeBytes);
        pBuffer += Pow2Align(sizeBytes, sizeof(uint32_t));
    }

    const uint32_t secHdrSize = sizeof(typename Elf::SectionHeader);
    for (auto& section : m_sections)
    {
        memcpy(pBuffer, &section.secHead, secHdrSize);
        pBuffer += secHdrSize;
    }

    LLPC_ASSERT((pBuffer - pData) == reqSize);
}

// =====================================================================================================================
// Copies ELF content from a ElfReader.
template<class Elf>
Result ElfWriter<Elf>::CopyFromReader(
    const ElfReader<Elf>& reader)   // The ElfReader to copy from.
{
    Result result = Result::Success;
    m_header = reader.m_header;
    m_sections.resize(reader.m_sections.size());
    for (size_t i = 0; i < reader.m_sections.size(); ++i)
    {
        auto pSection = reader.m_sections[i];
        m_sections[i].secHead = pSection->secHead;
        m_sections[i].pName = pSection->pName;
        auto pData = new uint8_t[pSection->secHead.sh_size + 1];
        memcpy(pData, pSection->pData, pSection->secHead.sh_size);
        pData[pSection->secHead.sh_size] = 0;
        m_sections[i].pData = pData;
    }

    m_map = reader.m_map;
    LLPC_ASSERT(m_header.e_phnum == 0);

    m_noteSecIdx = m_map[NoteName];
    m_textSecIdx = m_map[TextName];
    m_symSecIdx = m_map[SymTabName];
    m_strtabSecIdx = m_map[StrTabName];
    LLPC_ASSERT(m_noteSecIdx > 0);
    LLPC_ASSERT(m_textSecIdx > 0);
    LLPC_ASSERT(m_symSecIdx > 0);
    LLPC_ASSERT(m_strtabSecIdx > 0);

    auto pNoteSection = &m_sections[m_noteSecIdx];
    const uint32_t noteHeaderSize = sizeof(NoteHeader) - 8;
    size_t offset = 0;
    while (offset < pNoteSection->secHead.sh_size)
    {
        const NoteHeader* pNote = reinterpret_cast<const NoteHeader*>(pNoteSection->pData + offset);
        const uint32_t noteNameSize = Pow2Align(pNote->nameSize, 4);
        ElfNote noteNode;
        memcpy(&noteNode.hdr, pNote, noteHeaderSize);
        memcpy(noteNode.hdr.name, pNote->name, noteNameSize);

        const uint32_t noteDescSize = Pow2Align(pNote->descSize, 4);
        auto pData = new uint8_t[noteDescSize];
        memcpy(pData, pNoteSection->pData + offset + noteHeaderSize + noteNameSize, noteDescSize);
        noteNode.pData = pData;

        offset += noteHeaderSize + noteNameSize + noteDescSize;
        m_notes.push_back(noteNode);
    }

    auto pSymSection = &m_sections[m_symSecIdx];
    const char* pStrTab = reinterpret_cast<const char*>(m_sections[m_strtabSecIdx].pData);

    auto symbols = reinterpret_cast<const typename Elf::Symbol*>(pSymSection->pData);
    auto symCount = static_cast<uint32_t>(pSymSection->secHead.sh_size / pSymSection->secHead.sh_entsize);

    for (uint32_t idx = 0; idx < symCount; ++idx)
    {
        ElfSymbol symbol = {};
        symbol.secIdx = symbols[idx].st_shndx;
        symbol.pSecName = m_sections[symbol.secIdx].pName;
        symbol.pSymName = pStrTab + symbols[idx].st_name;
        symbol.size = symbols[idx].st_size;
        symbol.value = symbols[idx].st_value;
        symbol.info.all = symbols[idx].st_info.all;
        symbol.nameOffset = symbols[idx].st_name;
        m_symbols.push_back(symbol);
    }

    std::sort(m_symbols.begin(), m_symbols.end(),
        [](const ElfSymbol & a, const ElfSymbol & b)
        {
            return (a.secIdx < b.secIdx) || ((a.secIdx == b.secIdx) && (a.value < b.value));
        });
    return result;
}

// =====================================================================================================================
// Reads ELF content from a buffer.
template<class Elf>
Result ElfWriter<Elf>::ReadFromBuffer(
    const void* pBuffer,    // [in] Buffer to read data from
    size_t      bufSize)    // Size of the buffer
{
    ElfReader<Elf> reader(m_gfxIp);
    auto result = reader.ReadFromBuffer(pBuffer, &bufSize);
    if (result != Llpc::Result::Success)
    {
        return result;
    }
    return CopyFromReader(reader);
}

// =====================================================================================================================
// Gets section data by section index.
template<class Elf>
Result ElfWriter<Elf>::GetSectionDataBySectionIndex(
    uint32_t                 secIdx,          // Section index
    const SectionBuffer**    ppSectionData    // [out] Section data
    ) const
{
    Result result = Result::ErrorInvalidValue;
    if (secIdx < m_sections.size())
    {
        *ppSectionData = &m_sections[secIdx];
        result = Result::Success;
    }
    return result;
}

// =====================================================================================================================
// Gets all associated symbols by section index.
template<class Elf>
void ElfWriter<Elf>::GetSymbolsBySectionIndex(
    uint32_t                secIdx,         // Section index
    std::vector<ElfSymbol*>& secSymbols)    // [out] ELF symbols
{
    uint32_t symCount = m_symbols.size();

    for (uint32_t idx = 0; idx < symCount; ++idx)
    {
        if (m_symbols[idx].secIdx == secIdx)
        {
            secSymbols.push_back(&m_symbols[idx]);
        }
    }
}

// =====================================================================================================================
// Merge ELF binary of fragment shader and ELF binary of non-fragment shaders into single ELF binary
template<class Elf>
void ElfWriter<Elf>::MergeElfBinary(
    Context*          pContext,        // [in] Pipeline context
    const BinaryData* pFragmentElf,    // [in] ELF binary of fragment shader
    ElfPackage*       pPipelineElf)    // [out] Final ELF binary
{
    auto FragmentIsaSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsMainEntry)];
    auto FragmentIntrlTblSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsShdrIntrlTblPtr)];
    auto FragmentDisassemblySymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsDisassembly)];
    auto FragmentIntrlDataSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsShdrIntrlData)];
    auto FragmentAmdIlSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsAmdIl)];

    ElfReader<Elf64> reader(m_gfxIp);

    auto fragmentCodesize = pFragmentElf->codeSize;
    auto result = reader.ReadFromBuffer(pFragmentElf->pCode, &fragmentCodesize);
    LLPC_ASSERT(result == Result::Success);
    LLPC_UNUSED(result);

    // Merge GPU ISA code
    const ElfSectionBuffer<Elf64::SectionHeader>* pNonFragmentTextSection = nullptr;
    ElfSectionBuffer<Elf64::SectionHeader>* pFragmentTextSection = nullptr;
    std::vector<ElfSymbol> fragmentSymbols;
    std::vector<ElfSymbol*> nonFragmentSymbols;

    auto fragmentTextSecIndex = reader.GetSectionIndex(TextName);
    auto nonFragmentSecIndex = GetSectionIndex(TextName);
    reader.GetSectionDataBySectionIndex(fragmentTextSecIndex, &pFragmentTextSection);
    reader.GetSymbolsBySectionIndex(fragmentTextSecIndex, fragmentSymbols);

    GetSectionDataBySectionIndex(nonFragmentSecIndex, &pNonFragmentTextSection);
    GetSymbolsBySectionIndex(nonFragmentSecIndex, nonFragmentSymbols);
    ElfSymbol* pFragmentIsaSymbol = nullptr;
    ElfSymbol* pNonFragmentIsaSymbol = nullptr;
    std::string firstIsaSymbolName;

    for (auto pSymbol : nonFragmentSymbols)
    {
        if (firstIsaSymbolName.empty())
        {
            // NOTE: Entry name of the first shader stage is missed in disassembly section, we have to add it back
            // when merge disassembly sections.
            if (strncmp(pSymbol->pSymName, "_amdgpu_", strlen("_amdgpu_")) == 0)
            {
                firstIsaSymbolName = pSymbol->pSymName;
            }
        }

        if (strcmp(pSymbol->pSymName, FragmentIsaSymbolName) == 0)
        {
            pNonFragmentIsaSymbol = pSymbol;
        }

        if (pNonFragmentIsaSymbol == nullptr)
        {
            continue;
        }

        // Reset all symbols after _amdgpu_ps_main
        pSymbol->secIdx = InvalidValue;
    }

    size_t isaOffset = (pNonFragmentIsaSymbol == nullptr) ?
                       Pow2Align(pNonFragmentTextSection->secHead.sh_size, 0x100) :
                       pNonFragmentIsaSymbol->value;
    for (auto& fragmentSymbol : fragmentSymbols)
    {
        if (strcmp(fragmentSymbol.pSymName, FragmentIsaSymbolName) == 0)
        {
            // Modify ISA code
            pFragmentIsaSymbol = &fragmentSymbol;
            ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
            MergeSection(pNonFragmentTextSection,
                                isaOffset,
                                nullptr,
                                pFragmentTextSection,
                                pFragmentIsaSymbol->value,
                                nullptr,
                                &newSection);
            SetSection(nonFragmentSecIndex, &newSection);
        }

        if (pFragmentIsaSymbol == nullptr)
        {
            continue;
        }

        // Update fragment shader related symbols
        ElfSymbol* pSymbol = nullptr;
        pSymbol = GetSymbol(fragmentSymbol.pSymName);
        pSymbol->secIdx = nonFragmentSecIndex;
        pSymbol->pSecName = nullptr;
        pSymbol->value = isaOffset + fragmentSymbol.value - pFragmentIsaSymbol->value;
        pSymbol->size = fragmentSymbol.size;
    }

    // LLPC doesn't use per pipeline internal table, and LLVM backend doesn't add symbols for disassembly info.
    LLPC_ASSERT((reader.IsValidSymbol(FragmentIntrlTblSymbolName) == false) &&
                (reader.IsValidSymbol(FragmentDisassemblySymbolName) == false) &&
                (reader.IsValidSymbol(FragmentIntrlDataSymbolName) == false) &&
                (reader.IsValidSymbol(FragmentAmdIlSymbolName) == false));
    LLPC_UNUSED(FragmentIntrlTblSymbolName);
    LLPC_UNUSED(FragmentDisassemblySymbolName);
    LLPC_UNUSED(FragmentIntrlDataSymbolName);
    LLPC_UNUSED(FragmentAmdIlSymbolName);

    // Merge ISA disassemble
    auto fragmentDisassemblySecIndex = reader.GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
    auto nonFragmentDisassemblySecIndex = GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
    ElfSectionBuffer<Elf64::SectionHeader>* pFragmentDisassemblySection = nullptr;
    const ElfSectionBuffer<Elf64::SectionHeader>* pNonFragmentDisassemblySection = nullptr;
    reader.GetSectionDataBySectionIndex(fragmentDisassemblySecIndex, &pFragmentDisassemblySection);
    GetSectionDataBySectionIndex(nonFragmentDisassemblySecIndex, &pNonFragmentDisassemblySection);
    if (pNonFragmentDisassemblySection != nullptr)
    {
        LLPC_ASSERT(pFragmentDisassemblySection != nullptr);
        // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
        // text search will be incorrect. It is only needed for ElfReader, ElfWriter always append a null terminator
        // for all section data.
        auto pFragmentDisassemblySectionEnd = pFragmentDisassemblySection->pData +
                                              pFragmentDisassemblySection->secHead.sh_size - 1;
        uint8_t lastChar = *pFragmentDisassemblySectionEnd;
        const_cast<uint8_t*>(pFragmentDisassemblySectionEnd)[0] = '\0';
        auto pFragmentDisassembly = strstr(reinterpret_cast<const char*>(pFragmentDisassemblySection->pData),
                                          FragmentIsaSymbolName);
        const_cast<uint8_t*>(pFragmentDisassemblySectionEnd)[0] = lastChar;

        auto fragmentDisassemblyOffset =
            (pFragmentDisassembly == nullptr) ?
            0 :
            (pFragmentDisassembly - reinterpret_cast<const char*>(pFragmentDisassemblySection->pData));

        auto pDisassemblyEnd = strstr(reinterpret_cast<const char*>(pNonFragmentDisassemblySection->pData),
                                     FragmentIsaSymbolName);
        auto disassemblySize = (pDisassemblyEnd == nullptr) ?
                              pNonFragmentDisassemblySection->secHead.sh_size :
                              pDisassemblyEnd - reinterpret_cast<const char*>(pNonFragmentDisassemblySection->pData);

        ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
        MergeSection(pNonFragmentDisassemblySection,
                     disassemblySize,
                     firstIsaSymbolName.c_str(),
                     pFragmentDisassemblySection,
                     fragmentDisassemblyOffset,
                     FragmentIsaSymbolName,
                     &newSection);
        SetSection(nonFragmentDisassemblySecIndex, &newSection);
    }

    // Merge LLVM IR disassemble
    const std::string LlvmIrSectionName = std::string(Util::Abi::AmdGpuCommentLlvmIrName);
    ElfSectionBuffer<Elf64::SectionHeader>* pFragmentLlvmIrSection = nullptr;
    const ElfSectionBuffer<Elf64::SectionHeader>* pNonFragmentLlvmIrSection = nullptr;

    auto fragmentLlvmIrSecIndex = reader.GetSectionIndex(LlvmIrSectionName.c_str());
    auto nonFragmentLlvmIrSecIndex = GetSectionIndex(LlvmIrSectionName.c_str());
    reader.GetSectionDataBySectionIndex(fragmentLlvmIrSecIndex, &pFragmentLlvmIrSection);
    GetSectionDataBySectionIndex(nonFragmentLlvmIrSecIndex, &pNonFragmentLlvmIrSection);

    if (pNonFragmentLlvmIrSection != nullptr)
    {
        LLPC_ASSERT(pFragmentLlvmIrSection != nullptr);

        // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
        // text search will be incorrect. It is only needed for ElfReader, ElfWriter always append a null terminator
        // for all section data.
        auto pFragmentLlvmIrSectionEnd = pFragmentLlvmIrSection->pData +
                                         pFragmentLlvmIrSection->secHead.sh_size - 1;
        uint8_t lastChar = *pFragmentLlvmIrSectionEnd;
        const_cast<uint8_t*>(pFragmentLlvmIrSectionEnd)[0] = '\0';
        auto pFragmentLlvmIrStart = strstr(reinterpret_cast<const char*>(pFragmentLlvmIrSection->pData),
                                           FragmentIsaSymbolName);
        const_cast<uint8_t*>(pFragmentLlvmIrSectionEnd)[0] = lastChar;

        auto fragmentLlvmIrOffset =
            (pFragmentLlvmIrStart == nullptr) ?
            0 :
            (pFragmentLlvmIrStart - reinterpret_cast<const char*>(pFragmentLlvmIrSection->pData));

        auto pLlvmIrEnd = strstr(reinterpret_cast<const char*>(pNonFragmentLlvmIrSection->pData),
                                 FragmentIsaSymbolName);
        auto llvmIrSize = (pLlvmIrEnd == nullptr) ?
                          pNonFragmentLlvmIrSection->secHead.sh_size :
                          pLlvmIrEnd - reinterpret_cast<const char*>(pNonFragmentLlvmIrSection->pData);

        ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
        MergeSection(pNonFragmentLlvmIrSection,
                     llvmIrSize,
                     firstIsaSymbolName.c_str(),
                     pFragmentLlvmIrSection,
                     fragmentLlvmIrOffset,
                     FragmentIsaSymbolName,
                     &newSection);
        SetSection(nonFragmentLlvmIrSecIndex, &newSection);
    }

    // Merge PAL metadata
    ElfNote nonFragmentMetaNote = {};
    nonFragmentMetaNote = GetNote(Util::Abi::PipelineAbiNoteType::PalMetadata);

    LLPC_ASSERT(nonFragmentMetaNote.pData != nullptr);
    ElfNote fragmentMetaNote = {};
    ElfNote newMetaNote = {};
    fragmentMetaNote = reader.GetNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
    MergeMetaNote(pContext, &nonFragmentMetaNote, &fragmentMetaNote, &newMetaNote);
    SetNote(&newMetaNote);

    WriteToBuffer(pPipelineElf);
}

// =====================================================================================================================
// Reset the contents to an empty ELF file.
template<class Elf>
void ElfWriter<Elf>::Reinitialize()
{
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
}

// =====================================================================================================================
// Link the relocatable ELF readers into a pipeline ELF.
template<class Elf>
Result ElfWriter<Elf>::LinkGraphicsRelocatableElf(
    const ArrayRef<ElfReader<Elf>*>& relocatableElfs, // An array of relocatable ELF objects
    Context* pContext)                                // [in] Acquired context
{
    Reinitialize();
    LLPC_ASSERT(relocatableElfs.size() == 2 && "Can only handle VsPs Shaders for now.");

    // Get the main data for the header, the parts that change will be updated when writing to buffer.
    m_header = relocatableElfs[0]->m_header;

    // Copy the contents of the string table
    ElfSectionBuffer<typename Elf::SectionHeader>* pStringTable1 = nullptr;
    relocatableElfs[0]->GetSectionDataBySectionIndex(relocatableElfs[0]->m_strtabSecIdx, &pStringTable1);

    ElfSectionBuffer<typename Elf::SectionHeader>* pStringTable2 = nullptr;
    relocatableElfs[1]->GetSectionDataBySectionIndex(relocatableElfs[1]->m_strtabSecIdx, &pStringTable2);

    MergeSection(pStringTable1,
                 pStringTable1->secHead.sh_size,
                 nullptr,
                 pStringTable2,
                 0,
                 nullptr,
                 &m_sections[m_strtabSecIdx]);

    // Merge text sections
    ElfSectionBuffer<typename Elf::SectionHeader>* pTextSection1 = nullptr;
    relocatableElfs[0]->GetTextSectionData(&pTextSection1);
    ElfSectionBuffer<typename Elf::SectionHeader> *pTextSection2 = nullptr;
    relocatableElfs[1]->GetTextSectionData(&pTextSection2);

    MergeSection(pTextSection1,
                 Pow2Align(pTextSection1->secHead.sh_size, 0x100),
                 nullptr,
                 pTextSection2,
                 0,
                 nullptr,
                 &m_sections[m_textSecIdx]);

    // Build the symbol table.  First set the symbol table section header.
    ElfSectionBuffer<typename Elf::SectionHeader>* pSymbolTableSection = nullptr;
    relocatableElfs[0]->GetSectionDataBySectionIndex(relocatableElfs[0]->m_symSecIdx, &pSymbolTableSection);
    m_sections[m_symSecIdx].secHead = pSymbolTableSection->secHead;

    // Now get the symbols that belong in the symbol table.
    uint32_t offset = 0;
    for (const ElfReader<Elf>* pElf : relocatableElfs)
    {
        uint32_t relocElfTextSectionId = pElf->GetSectionIndex(TextName);
        std::vector<ElfSymbol> symbols;
        pElf->GetSymbolsBySectionIndex(relocElfTextSectionId, symbols);
        for (auto sym : symbols)
        {
            // When relocations start being used, we will have to filter out symbols related to relocations.
            // We should be able to filter the BB* symbols as well.
            ElfSymbol *pNewSym = GetSymbol(sym.pSymName);
            pNewSym->secIdx = m_textSecIdx;
            pNewSym->pSecName = nullptr;
            pNewSym->value = sym.value + offset;
            pNewSym->size = sym.size;
            pNewSym->info = sym.info;
        }

        // Update the offset for the next elf file.
        ElfSectionBuffer<typename Elf::SectionHeader>* pTextSection = nullptr;
        pElf->GetSectionDataBySectionIndex(relocElfTextSectionId, &pTextSection);
        offset += Pow2Align(pTextSection->secHead.sh_size, 0x100);
    }

    // Apply relocations
    // There are no relocations to apply yet.

    // Set the .note section header
    ElfSectionBuffer<typename Elf::SectionHeader>* pNoteSection = nullptr;
    relocatableElfs[0]->GetSectionDataBySectionIndex(relocatableElfs[0]->GetSectionIndex(NoteName), &pNoteSection);
    m_sections[m_noteSecIdx].secHead = pNoteSection->secHead;

    // Merge and update the .note data
    // The merged note info will be updated using data in the pipeline create info, but nothing needs to be done yet.
    ElfNote noteInfo1 = relocatableElfs[0]->GetNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
    ElfNote noteInfo2 = relocatableElfs[1]->GetNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
    m_notes.push_back({});
    MergeMetaNote(pContext, &noteInfo1, &noteInfo2, &m_notes.back());

    // Merge other sections.  For now, none of the other sections are important, so we will not do anything.

    return Result::Success;
}

// =====================================================================================================================
// Link the compute shader relocatable ELF reader into a pipeline ELF.
template<class Elf>
Result ElfWriter<Elf>::LinkComputeRelocatableElf(
    const ElfReader<Elf>& relocatableElf,   // [in] Relocatable compute shader elf
    Context* pContext)                      // [in] Acquired context
{
    // Currently nothing to do, just copy the elf.
    CopyFromReader(relocatableElf);

    // Apply relocations
    // There are no relocations to apply yet.

    return Result::Success;
}

template class ElfWriter<Elf64>;

} // Llpc
