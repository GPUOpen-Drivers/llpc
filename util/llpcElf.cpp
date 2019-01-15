/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcElf.cpp
 * @brief LLPC source file: contains implementation of LLPC ELF utilities.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-elf"

#include <algorithm>
#include <string.h>
#include "llpcElf.h"

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
    m_strtabSecIdx(InvalidValue)
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
            auto pBuf =  new ElfReadSectionBuffer<typename Elf::SectionHeader>;

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
uint32_t ElfReader<Elf>::GetSymbolCount()
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
    ElfSectionBuffer** ppSectionData    // [out] Section data
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
    ElfSectionBuffer** ppSectionData    // [out] Section data
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
    std::vector<ElfSymbol>& secSymbols)     // [out] ELF symbols
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

                secSymbols.push_back(symbol);
            }
        }

        sort(secSymbols.begin(), secSymbols.end(),
             [](const ElfSymbol& a, const ElfSymbol& b)
             {
                 return a.value < b.value;
             });
    }
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 432
// =====================================================================================================================
// Initialize MsgPack context and related visitor iterators
template<class Elf>
void ElfReader<Elf>::InitMsgPack(
    const void* pBuffer,       // [in] Message buffer
    uint32_t    sizeInBytes)   // Buffer size in bytes
{
    memset(&m_msgPackContext, 0, sizeof(m_msgPackContext));
    cw_unpack_context_init(&m_msgPackContext, pBuffer, sizeInBytes, nullptr, nullptr);

    m_iteratorStack.clear();
    MsgPackIterator iter = { };
    iter.status = MsgPackIteratorNone;
    iter.index = 0;
    iter.size = 0;
    m_iteratorStack.push_back(iter);

    m_msgPackMapLevel = 0;
}

// =====================================================================================================================
// Advances the MsgPack context to the next item token and return true if success.
template<class Elf>
bool ElfReader<Elf>::GetNextMsgItem()
{
    cw_unpack_next(&m_msgPackContext);
    return m_msgPackContext.return_code == CWP_RC_OK;
}

// =====================================================================================================================
// Gets MsgPack item.
template<class Elf>
const cwpack_item* ElfReader<Elf>::GetMsgItem() const
{
    return &m_msgPackContext.item;
}

// =====================================================================================================================
// Gets the status of message packer iterator.
template<class Elf>
MsgPackIteratorStatus ElfReader<Elf>::GetMsgIteratorStatus() const
{
    return m_iteratorStack.back().status;
}

// =====================================================================================================================
// Gets the map level of current message item.
template<class Elf>
uint32_t ElfReader<Elf>::GetMsgMapLevel() const
{
    return m_msgPackMapLevel;
}

// =====================================================================================================================
// Gets the map level of current message item.
template<class Elf>
void ElfReader<Elf>::UpdateMsgPackStatus(
    std::function<void(MsgPackIteratorStatus)> callback) // Callback function for array or map finish
{
    auto pCurIter = &m_iteratorStack.back();

    if ((m_msgPackContext.item.type == CWP_ITEM_MAP) || (m_msgPackContext.item.type == CWP_ITEM_ARRAY))
    {
        // Begin a new map or array
        MsgPackIterator iter = { };

        iter.status = (m_msgPackContext.item.type == CWP_ITEM_MAP) ? MsgPackIteratorMapKey : MsgPackIteratorArray;
        iter.index = 0;
        iter.size = m_msgPackContext.item.as.map.size;
        m_iteratorStack.push_back(iter);

        if (m_msgPackContext.item.type == CWP_ITEM_MAP)
        {
            ++m_msgPackMapLevel;
        }
    }
    else if ((GetMsgIteratorStatus() == MsgPackIteratorMapValue) || (GetMsgIteratorStatus() == MsgPackIteratorArray))
    {
        // Visit a map value or array item
        ++pCurIter->index;
        while ((pCurIter->index == pCurIter->size) && (m_iteratorStack.size() > 1))
        {
            if (pCurIter->status == MsgPackIteratorMapValue)
            {
                callback(pCurIter->status);
                --m_msgPackMapLevel;
            }
            else
            {
                callback(pCurIter->status);
            }
            m_iteratorStack.pop_back();
            pCurIter = &m_iteratorStack.back();
            ++ pCurIter->index;
        }

        if (pCurIter->status == MsgPackIteratorMapValue)
        {
            pCurIter->status = MsgPackIteratorMapKey;
        }
    }
    else if (pCurIter->status == MsgPackIteratorMapKey)
    {
        // Visit a map key
        LLPC_ASSERT((m_msgPackContext.item.type == CWP_ITEM_STR) ||
                    (m_msgPackContext.item.type == CWP_ITEM_POSITIVE_INTEGER));
        pCurIter->status = MsgPackIteratorMapValue;
    }
}
#endif

// Explicit instantiations for ELF utilities
template class ElfReader<Elf64>;

} // Llpc
