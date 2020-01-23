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
 * @file  llpcElfWriter.h
 * @brief LLPC header file: contains declaration of LLPC ELF writing utilities.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcElfReader.h"

 // Forward declaration
namespace llvm { namespace msgpack { class MapDocNode; } }

namespace Llpc
{

// Forward declaration
class Context;

// =====================================================================================================================
// Represents a writer for storing data to an ELF buffer.
//
// NOTE: It is a limited implementation, it is designed for merging two ELF binaries which generated by LLVM back-end.
template<class Elf>
class ElfWriter
{
public:
    typedef ElfSectionBuffer<typename Elf::SectionHeader> SectionBuffer;

    ElfWriter(GfxIpVersion gfxIp);

    ~ElfWriter();

    static void MergeSection(const SectionBuffer* pSection1,
                             size_t               section1Size,
                             const char*          pPrefixString1,
                             const SectionBuffer* pSection2,
                             size_t               section2Offset,
                             const char*          pPrefixString2,
                             SectionBuffer*       pNewSection);

    static void MergeMetaNote(Context*       pContext,
                              const ElfNote* pNote1,
                              const ElfNote* pNote2,
                              ElfNote*       pNewNote);

    Result ReadFromBuffer(const void* pBuffer, size_t bufSize);
    Result CopyFromReader(const ElfReader<Elf>& reader);

    void MergeElfBinary(Context*          pContext,
                        const BinaryData* pFragmentElf,
                        ElfPackage*       pPipelineElf);

    // Gets the section index for the specified section name.
    int32_t GetSectionIndex(const char* pName) const
    {
        auto pEntry = m_map.find(pName);
        return (pEntry != m_map.end()) ? pEntry->second : InvalidValue;
    }

    void SetSection(uint32_t secIndex, SectionBuffer* pSection);

    ElfSymbol* GetSymbol(const char* pSymbolName);

    ElfNote GetNote(Util::Abi::PipelineAbiNoteType noteType);

    void SetNote(ElfNote* pNote);

    Result GetSectionDataBySectionIndex(uint32_t secIdx, const SectionBuffer** ppSectionData) const;

    void GetSymbolsBySectionIndex(uint32_t secIdx, std::vector<ElfSymbol*>& secSymbols);

    void WriteToBuffer(ElfPackage* pElf);

    Result LinkGraphicsRelocatableElf(const llvm::ArrayRef<ElfReader<Elf>*>& relocatableElfs, Context *pContext);

    Result LinkComputeRelocatableElf(const ElfReader<Elf>& relocatableElf, Context* pContext);
private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(ElfWriter);

    static void MergeMapItem(llvm::msgpack::MapDocNode& destMap, llvm::msgpack::MapDocNode& srcMap, uint32_t key);

    size_t GetRequiredBufferSizeBytes();

    void CalcSectionHeaderOffset();

    void AssembleNotes();

    void AssembleSymbols();

    void Reinitialize();

    // -----------------------------------------------------------------------------------------------------------------

    GfxIpVersion                      m_gfxIp;    // Graphics IP version info (used by ELF dump only)
    typename Elf::FormatHeader        m_header;   // ELF header
    std::map<std::string, uint32_t>   m_map;      // Map between section name and section index

    std::vector<SectionBuffer>        m_sections;    // List of section data and headers
    std::vector<ElfNote>              m_notes;       // List of Elf notes
    std::vector<ElfSymbol>            m_symbols;     // List of Elf symbols

    int32_t m_textSecIdx;       // Section index of .text section
    int32_t m_noteSecIdx;       // Section index of .note section
    int32_t m_symSecIdx;        // Section index of symbol table section
    int32_t m_strtabSecIdx;     // Section index of string table section
};

} // Llpc
