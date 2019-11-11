/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcElfReader.cpp
 * @brief LLPC source file: contains implementation of LLPC ELF reading utilities.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-elf-reader"

#include <algorithm>
#include <string.h>
#include "llpcElfReader.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
template<class Elf>
ElfReader<Elf>::ElfReader(
    GfxIpVersion gfxIp) // Graphics IP version info
    :
    m_gfxIp(gfxIp),
    m_header(),
    m_symSecIdx(InvalidValue),
    m_relocSecIdx(InvalidValue),
    m_strtabSecIdx(InvalidValue),
    m_textSecIdx(InvalidValue)
{
}

// =====================================================================================================================
template<class Elf>
ElfReader<Elf>::~ElfReader()
{
    for (auto pSection : m_sections)
    {
        delete pSection;
    }
    m_sections.clear();
}

// =====================================================================================================================
// Reads ELF data in from the given buffer into the context.
//
// ELF data is stored in the buffer like so:
//
// + ELF header
// + Section Header String Table
//
// + Section Buffer (b0) [NULL]
// + Section Buffer (b1) [.shstrtab]
// + ...            (b#) [...]
//
// + Section Header (h0) [NULL]
// + Section Header (h1) [.shstrtab]
// + ...            (h#) [...]
template<class Elf>
Result ElfReader<Elf>::ReadFromBuffer(
    const void* pBuffer,   // [in] Input ELF data buffer
    size_t*     pBufSize)  // [out] Size of the given read buffer (determined from the ELF header)
{
    LLPC_ASSERT(pBuffer != nullptr);

    Result result = Result::Success;

    const uint8_t* pData = static_cast<const uint8_t*>(pBuffer);

    // ELF header is always located at the beginning of the file
    auto pHeader = static_cast<const typename Elf::FormatHeader*>(pBuffer);

    // If the identification info isn't the magic number, this isn't a valid file.
    result = (pHeader->e_ident32[EI_MAG0] == ElfMagic) ?  Result::Success : Result::ErrorInvalidValue;

    if (result == Result::Success)
    {
        result = (pHeader->e_machine == EM_AMDGPU) ? Result::Success : Result::ErrorInvalidValue;
    }

    if (result == Result::Success)
    {
        m_header = *pHeader;
        size_t readSize = sizeof(typename Elf::FormatHeader);

        // Section header location information.
        const uint32_t sectionHeaderOffset = static_cast<uint32_t>(pHeader->e_shoff);
        const uint32_t sectionHeaderNum    = pHeader->e_shnum;
        const uint32_t sectionHeaderSize   = pHeader->e_shentsize;

        const uint32_t sectionStrTableHeaderOffset = sectionHeaderOffset + (pHeader->e_shstrndx * sectionHeaderSize);
        auto pSectionStrTableHeader = reinterpret_cast<const typename Elf::SectionHeader*>(pData + sectionStrTableHeaderOffset);
        const uint32_t sectionStrTableOffset = static_cast<uint32_t>(pSectionStrTableHeader->sh_offset);

        for (uint32_t section = 0; section < sectionHeaderNum; section++)
        {
            // Where the header is located for this section
            const uint32_t sectionOffset = sectionHeaderOffset + (section * sectionHeaderSize);
            auto pSectionHeader = reinterpret_cast<const typename Elf::SectionHeader*>(pData + sectionOffset);
            readSize += sizeof(typename Elf::SectionHeader);

            // Where the name is located for this section
            const uint32_t sectionNameOffset = sectionStrTableOffset + pSectionHeader->sh_name;
            const char* pSectionName = reinterpret_cast<const char*>(pData + sectionNameOffset);

            // Where the data is located for this section
            const uint32_t sectionDataOffset = static_cast<uint32_t>(pSectionHeader->sh_offset);
            auto pBuf = new SectionBuffer;

            result = (pBuf != nullptr) ? Result::Success : Result::ErrorOutOfMemory;

            if (result == Result::Success)
            {
                pBuf->secHead = *pSectionHeader;
                pBuf->pName   = pSectionName;
                pBuf->pData   = (pData + sectionDataOffset);

                readSize += static_cast<size_t>(pSectionHeader->sh_size);

                m_sections.push_back(pBuf);
                m_map[pSectionName] = section;
            }
        }

        *pBufSize = readSize;
    }

    // Get section index
    m_symSecIdx    = GetSectionIndex(SymTabName);
    m_relocSecIdx  = GetSectionIndex(RelocName);
    m_strtabSecIdx = GetSectionIndex(StrTabName);
    m_textSecIdx   = GetSectionIndex(TextName);

    return result;
}

// =====================================================================================================================
// Retrieves the section data for the specified section name, if it exists.
template<class Elf>
Result ElfReader<Elf>::GetSectionData(
    const char*  pName,       // [in] Name of the section to look for
    const void** pData,       // [out] Pointer to section data
    size_t*      pDataLength  // [out] Size of the section data
    ) const
{
    Result result = Result::ErrorInvalidValue;

    auto pEntry = m_map.find(pName);

    if (pEntry != m_map.end())
    {
        *pData = m_sections[pEntry->second]->pData;
        *pDataLength = static_cast<size_t>(m_sections[pEntry->second]->secHead.sh_size);
        result = Result::Success;
    }

    return result;
}

// =====================================================================================================================
// Gets the count of symbols in the symbol table section.
template<class Elf>
uint32_t ElfReader<Elf>::GetSymbolCount() const
{
    uint32_t symCount = 0;
    if (m_symSecIdx >= 0)
    {
        auto& pSection = m_sections[m_symSecIdx];
        symCount = static_cast<uint32_t>(pSection->secHead.sh_size / pSection->secHead.sh_entsize);
    }
    return symCount;
}

// =====================================================================================================================
// Gets info of the symbol in the symbol table section according to the specified index.
template<class Elf>
void ElfReader<Elf>::GetSymbol(
    uint32_t   idx,       // Symbol index
    ElfSymbol* pSymbol)   // [out] Info of the symbol
{
    auto& pSection = m_sections[m_symSecIdx];
    const char* pStrTab = reinterpret_cast<const char*>(m_sections[m_strtabSecIdx]->pData);

    auto symbols = reinterpret_cast<const typename Elf::Symbol*>(pSection->pData);
    pSymbol->secIdx   = symbols[idx].st_shndx;
    pSymbol->pSecName = m_sections[pSymbol->secIdx]->pName;
    pSymbol->pSymName = pStrTab + symbols[idx].st_name;
    pSymbol->size     = symbols[idx].st_size;
    pSymbol->value    = symbols[idx].st_value;
    pSymbol->info.all = symbols[idx].st_info.all;
}

// =====================================================================================================================
// Gets the count of relocations in the relocation section.
template<class Elf>
uint32_t ElfReader<Elf>::GetRelocationCount()
{
    uint32_t relocCount = 0;
    if (m_relocSecIdx >= 0)
    {
        auto& pSection = m_sections[m_relocSecIdx];
        relocCount = static_cast<uint32_t>(pSection->secHead.sh_size / pSection->secHead.sh_entsize);
    }
    return relocCount;
}

// =====================================================================================================================
// Gets info of the relocation in the relocation section according to the specified index.
template<class Elf>
void ElfReader<Elf>::GetRelocation(
    uint32_t  idx,      // Relocation index
    ElfReloc* pReloc)   // [out] Info of the relocation
{
    auto& pSection = m_sections[m_relocSecIdx];

    auto relocs = reinterpret_cast<const typename Elf::Reloc*>(pSection->pData);
    pReloc->offset = relocs[idx].r_offset;
    pReloc->symIdx = relocs[idx].r_symbol;
}

// =====================================================================================================================
// Gets the count of Elf section.
template<class Elf>
uint32_t ElfReader<Elf>::GetSectionCount()
{
    return static_cast<uint32_t>(m_sections.size());
}

// =====================================================================================================================
// Gets section data by section index.
template<class Elf>
Result ElfReader<Elf>::GetSectionDataBySectionIndex(
    uint32_t           secIdx,          // Section index
    SectionBuffer**    ppSectionData    // [out] Section data
    ) const
{
    Result result = Result::ErrorInvalidValue;
    if (secIdx < m_sections.size())
    {
        *ppSectionData = m_sections[secIdx];
        result = Result::Success;
    }
    return result;
}

// =====================================================================================================================
// Gets section data by sorting index (ordered map).
template<class Elf>
Result ElfReader<Elf>::GetSectionDataBySortingIndex(
    uint32_t           sortIdx,         // Sorting index
    uint32_t*          pSecIdx,         // [out] Section index
    SectionBuffer**    ppSectionData    // [out] Section data
    ) const
{
    Result result = Result::ErrorInvalidValue;
    if (sortIdx < m_sections.size())
    {
        auto it = m_map.begin();
        for (uint32_t i = 0; i < sortIdx; ++i)
        {
            ++it;
        }
        *pSecIdx = it->second;
        *ppSectionData = m_sections[it->second];
        result = Result::Success;
    }
    return result;
}

// =====================================================================================================================
// Gets all associated symbols by section index.
template<class Elf>
void ElfReader<Elf>::GetSymbolsBySectionIndex(
    uint32_t                secIdx,         // Section index
    std::vector<ElfSymbol>& secSymbols      // [out] ELF symbols
    ) const
{
    if ((secIdx < m_sections.size()) && (m_symSecIdx >= 0))
    {
        auto& pSection = m_sections[m_symSecIdx];
        const char* pStrTab = reinterpret_cast<const char*>(m_sections[m_strtabSecIdx]->pData);

        auto symbols = reinterpret_cast<const typename Elf::Symbol*>(pSection->pData);
        uint32_t symCount = GetSymbolCount();
        ElfSymbol symbol = {};

        for (uint32_t idx = 0; idx < symCount; ++idx)
        {
            if (symbols[idx].st_shndx == secIdx)
            {
                symbol.secIdx   = symbols[idx].st_shndx;
                symbol.pSecName = m_sections[symbol.secIdx]->pName;
                symbol.pSymName = pStrTab + symbols[idx].st_name;
                symbol.size     = symbols[idx].st_size;
                symbol.value    = symbols[idx].st_value;
                symbol.info.all = symbols[idx].st_info.all;

                secSymbols.push_back(symbol);
            }
        }

        std::sort(secSymbols.begin(), secSymbols.end(),
             [](const ElfSymbol& a, const ElfSymbol& b)
             {
                 return a.value < b.value;
             });
    }
}

// =====================================================================================================================
// Checks whether the input name is a valid symbol.
template<class Elf>
bool ElfReader<Elf>::IsValidSymbol(
    const char* pSymbolName)  // [in] Symbol name
{
    auto& pSection = m_sections[m_symSecIdx];
    const char* pStrTab = reinterpret_cast<const char*>(m_sections[m_strtabSecIdx]->pData);

    auto symbols = reinterpret_cast<const typename Elf::Symbol*>(pSection->pData);
    uint32_t symCount = GetSymbolCount();
    bool findSymbol = false;
    for (uint32_t idx = 0; idx < symCount; ++idx)
    {
        auto pName = pStrTab + symbols[idx].st_name;
        if (strcmp(pName, pSymbolName) == 0)
        {
            findSymbol = true;
            break;
        }
    }
    return findSymbol;
}

// =====================================================================================================================
// Gets note according to note type
template<class Elf>
ElfNote ElfReader<Elf>::GetNote(
    Util::Abi::PipelineAbiNoteType noteType // Note type
    ) const
{
    uint32_t noteSecIdx = m_map.at(NoteName);
    LLPC_ASSERT(noteSecIdx > 0);

    auto pNoteSection = m_sections[noteSecIdx];
    ElfNote noteNode = {};
    const uint32_t noteHeaderSize = sizeof(NoteHeader) - 8;

    size_t offset = 0;
    while (offset < pNoteSection->secHead.sh_size)
    {
        const NoteHeader* pNote = reinterpret_cast<const NoteHeader*>(pNoteSection->pData + offset);
        const uint32_t noteNameSize = Pow2Align(pNote->nameSize, 4);
        if (pNote->type == noteType)
        {
            memcpy(&noteNode.hdr, pNote, sizeof(NoteHeader));
            noteNode.pData = pNoteSection->pData + offset + noteHeaderSize + noteNameSize;
            break;
        }
        offset += noteHeaderSize + noteNameSize + Pow2Align(pNote->descSize, 4);
    }

    return noteNode;
}

// =====================================================================================================================
// Initialize MsgPack document and related visitor iterators
template<class Elf>
void ElfReader<Elf>::InitMsgPackDocument(
    const void* pBuffer,       // [in] Message buffer
    uint32_t    sizeInBytes)   // Buffer size in bytes
{
    m_document.readFromBlob(StringRef(reinterpret_cast<const char*>(pBuffer), sizeInBytes), false);

    auto pRoot = &m_document.getRoot();
    LLPC_ASSERT(pRoot->isMap());

    m_iteratorStack.clear();
    MsgPackIterator iter = { };
    iter.pNode = &(pRoot->getMap(true));
    iter.status = MsgPackIteratorMapBegin;
    m_iteratorStack.push_back(iter);

    m_msgPackMapLevel = 0;
}

// =====================================================================================================================
// Advances the MsgPack context to the next item token and return true if success.
template<class Elf>
bool ElfReader<Elf>::GetNextMsgNode()
{
    if (m_iteratorStack.empty())
    {
        return false;
    }

    MsgPackIterator curIter = m_iteratorStack.back();
    bool skipPostCheck = false;
    if (curIter.status == MsgPackIteratorMapBegin)
    {
        auto pMap = &(curIter.pNode->getMap(true));
        curIter.mapIt = pMap->begin();
        curIter.mapEnd = pMap->end();
        m_msgPackMapLevel++;
        curIter.status = MsgPackIteratorMapPair;
        m_iteratorStack.push_back(curIter);
        skipPostCheck = true;
    }
    else if (curIter.status == MsgPackIteratorMapPair)
    {
        LLPC_ASSERT((curIter.mapIt->first.isMap() == false) && (curIter.mapIt->first.isArray() == false));
        curIter.status = MsgPackIteratorMapKey;
        m_iteratorStack.push_back(curIter);
    }
    else if (curIter.status == MsgPackIteratorMapKey)
    {
        if (curIter.mapIt->second.isMap())
        {
            curIter.status = MsgPackIteratorMapBegin;
            curIter.pNode = &(curIter.mapIt->second.getMap(true));
        }
        else if (curIter.mapIt->second.isArray())
        {
            curIter.status = MsgPackIteratorArray;
            auto pArray = &(curIter.mapIt->second.getArray(true));
            curIter.arrayIt = pArray->begin();
            curIter.arrayEnd = pArray->end();
            curIter.pNode = pArray;
        }
        else
        {
            curIter.status = MsgPackIteratorMapValue;
        }
        m_iteratorStack.back() = curIter;
        skipPostCheck = true;
    }
    else if (curIter.status == MsgPackIteratorArray)
    {
        curIter.arrayIt = curIter.pNode->getArray(true).begin();
        curIter.arrayEnd = curIter.pNode->getArray(true).end();
        if (curIter.arrayIt->isMap())
        {
            curIter.status = MsgPackIteratorMapBegin;
            curIter.pNode = &(curIter.arrayIt->getMap(true));
        }
        else if (curIter.arrayIt->isArray())
        {
            curIter.status = MsgPackIteratorArray;
            auto pArray = &(curIter.arrayIt->getArray(true));
            curIter.arrayIt = pArray->begin();
            curIter.arrayEnd = pArray->end();
            curIter.pNode = pArray;
        }
        else
        {
            curIter.status = MsgPackIteratorArrayValue;
        }
        m_iteratorStack.push_back(curIter);
        skipPostCheck = true;
    }
    else if (curIter.status == MsgPackIteratorMapValue)
    {
        m_iteratorStack.pop_back();
    }
    else if (curIter.status == MsgPackIteratorArrayValue)
    {
        m_iteratorStack.pop_back();
    }
    else if (curIter.status == MsgPackIteratorMapEnd)
    {
        m_iteratorStack.pop_back();
        m_iteratorStack.pop_back();
        m_msgPackMapLevel--;
    }
    else if (curIter.status == MsgPackIteratorArrayEnd)
    {
        m_iteratorStack.pop_back();
        m_iteratorStack.pop_back();
    }

    // Post check for end visit map or array element
    if ((m_iteratorStack.size() > 0) && (skipPostCheck == false))
    {
        auto pNextIter = &m_iteratorStack.back();
        if (pNextIter->status == MsgPackIteratorMapPair)
        {
            pNextIter->mapIt++;
            if (pNextIter->mapIt == pNextIter->mapEnd)
            {
                pNextIter->status = MsgPackIteratorMapEnd;
            }
        }
        else if (pNextIter->status == MsgPackIteratorArray)
        {
            pNextIter->arrayIt++;
            curIter = *pNextIter;
            if (curIter.arrayIt == curIter.arrayEnd)
            {
                curIter.status = MsgPackIteratorArrayEnd;
            }
            else if (curIter.arrayIt->isMap())
            {
                curIter.status = MsgPackIteratorMapBegin;
                curIter.pNode = &(curIter.arrayIt->getMap(true));
            }
            else if (curIter.arrayIt->isArray())
            {
                curIter.status = MsgPackIteratorArray;
                auto pArray = &(curIter.arrayIt->getArray(true));
                curIter.arrayIt = pArray->begin();
                curIter.arrayEnd = pArray->end();
                curIter.pNode = pArray;
            }
            else
            {
                curIter.status = MsgPackIteratorArrayValue;
            }
            m_iteratorStack.push_back(curIter);
        }
    }

    return m_iteratorStack.size() > 0;
}

// =====================================================================================================================
// Gets MsgPack node.
template<class Elf>
const llvm::msgpack::DocNode* ElfReader<Elf>::GetMsgNode() const
{
    LLPC_ASSERT(m_iteratorStack.size() > 0);
    auto pCurIter = &(m_iteratorStack.back());
    if (pCurIter->status == MsgPackIteratorArrayValue)
    {
        return &(*pCurIter->arrayIt);
    }
    else if (pCurIter->status == MsgPackIteratorMapValue)
    {
        return &(pCurIter->mapIt->second);
    }
    else if (pCurIter->status == MsgPackIteratorMapKey)
    {
        return &(pCurIter->mapIt->first);
    }
    else
    {
        return pCurIter->pNode;
    }
}

// =====================================================================================================================
// Gets the map level of current message item.
template<class Elf>
uint32_t ElfReader<Elf>::GetMsgMapLevel() const
{
    return m_msgPackMapLevel;
}

// =====================================================================================================================
// Gets the status of message packer iterator.
template<class Elf>
MsgPackIteratorStatus ElfReader<Elf>::GetMsgIteratorStatus() const
{
    return m_iteratorStack.back().status;
}

// Explicit instantiations for ELF utilities
template class ElfReader<Elf64>;

} // Llpc
