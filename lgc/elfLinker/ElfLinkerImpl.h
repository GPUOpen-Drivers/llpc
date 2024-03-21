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
/**
 ***********************************************************************************************************************
 * @file  ElfLinkerImpl.h
 * @brief LLPC header file: The class implements linking unlinked shader/part-pipeline ELFs into pipeline ELF
 ***********************************************************************************************************************
 */
#pragma once

#include "GlueShader.h"
#include "lgc/ElfLinker.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Object/ELFObjectFile.h"
#include <unordered_map>

struct ShaderDbgOutput;

namespace lgc {

class PipelineState;
class ElfLinkerImpl;

// =====================================================================================================================
// An ELF input to the linker
struct ElfInput {
  std::unique_ptr<llvm::object::ObjectFile> objectFile;
  llvm::SmallVector<std::pair<unsigned, unsigned>, 4> sectionMap;
  llvm::StringRef reduceAlign; // If non-empty, the name of a text section to reduce the alignment to 0x40
};

// =====================================================================================================================
// A single input section
struct InputSection {
  InputSection(llvm::object::SectionRef sectionRef) : sectionRef(sectionRef), size(sectionRef.getSize()) {}
  llvm::object::SectionRef sectionRef; // Section from the input ELF
  size_t offset = 0;                   // Offset within the output ELF section
  uint64_t size;                       // Size, possibly after removing s_end_code padding
};

// =====================================================================================================================
// A single output section
class OutputSection {
public:
  // Constructor given name and optional SHT_* section type
  OutputSection(ElfLinkerImpl *linker, llvm::StringRef name = "", unsigned type = 0)
      : m_linker(linker), m_name(name), m_type(type) {}

  // Add an input section
  void addInputSection(ElfInput &elfInput, llvm::object::SectionRef inputSectionRef, bool reduceAlign = false);

  // Get name of output section
  llvm::StringRef getName();

  // Get the section index in the output file
  unsigned getIndex();

  // Set the layout of this output section, allowing for alignment required by input sections.
  void layout();

  // Add a symbol to the output symbol table
  void addSymbol(const llvm::object::ELFSymbolRef &elfSymRef, unsigned inputSectIdx);

  // Add a relocation to the output elf
  void addRelocation(llvm::object::ELFRelocationRef relocRef, llvm::StringRef id, unsigned int relocSectionOffset,
                     unsigned int targetSectionOffset, unsigned sectType);

  // Get the output file offset of a particular input section in the output section
  uint64_t getOutputOffset(unsigned inputIdx) { return m_offset + m_inputSections[inputIdx].offset; }

  // Get the overall alignment requirement, after calling layout().
  llvm::Align getAlignment() const { return m_alignment; }

  // Write the output section
  void write(llvm::raw_pwrite_stream &outStream, llvm::ELF::Elf64_Shdr *shdr);

private:
  // Flag that we want to reduce alignment on the given input section, for gluing code together.
  void setReduceAlign(const InputSection &inputSection) {
    m_reduceAlign |= 1ULL << (&inputSection - &m_inputSections[0]);
  }

  // See if the given input section has the reduce align flag set.
  bool getReduceAlign(const InputSection &inputSection) const {
    return (m_reduceAlign >> (&inputSection - &m_inputSections[0])) & 1;
  }

  // Get alignment for an input section. This takes into account the reduceAlign flag.
  llvm::Align getAlignment(const InputSection &inputSection);

  ElfLinkerImpl *m_linker;
  llvm::StringRef m_name;                             // Section name
  unsigned m_type;                                    // Section type (SHT_* value)
  uint64_t m_offset = 0;                              // File offset of this output section
  llvm::SmallVector<InputSection, 4> m_inputSections; // Input sections contributing to this output section
  llvm::Align m_alignment;                            // Overall alignment required for the section
  unsigned m_reduceAlign = 0;                         // Bitmap of input sections to reduce alignment for
};

// =====================================================================================================================
// Internal implementation of the LGC interface for ELF linking.
class ElfLinkerImpl final : public lgc::ElfLinker {
public:
  // Constructor given PipelineState and ELFs to link
  ElfLinkerImpl(lgc::PipelineState *pipelineState, llvm::ArrayRef<llvm::MemoryBufferRef> elfs);

  // Destructor
  ~ElfLinkerImpl() override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Implementations of ElfLinker methods exposed to the front-end

  // Add another input ELF to the link, in addition to the ones that were added when the ElfLinker was constructed.
  // The default behavior of adding extra ones at the start of the list instead of the end is just so you
  // get the same order of code (VS then FS) when doing a part-pipeline compile as when doing a whole pipeline
  // compile, to make it easier to test by diff.
  void addInputElf(llvm::MemoryBufferRef inputElf) override final { addInputElf(inputElf, /*addAtStart=*/true); }
  void addInputElf(llvm::MemoryBufferRef inputElf, bool addAtStart);

  // Check whether we have FS input mappings, and thus whether we're doing part-pipeline compilation of the
  // pre-FS part of the pipeline.
  bool haveFsInputMappings() override final;

  // Get a representation of the fragment shader input mappings from the PAL metadata of ELF input(s) added so far.
  // This is used by the caller in a part-pipeline compilation scheme to include the FS input mappings in the
  // hash for the non-FS part of the pipeline.
  llvm::StringRef getFsInputMappings() override final;

  // Get information on the glue code that will be needed for the link
  llvm::ArrayRef<llvm::StringRef> getGlueInfo() override final;

  // Explicitly build color export shader
  llvm::StringRef createColorExportShader(llvm::ArrayRef<lgc::ColorExportInfo> exports, bool enableKill) override final;

  // Add a blob for a particular chunk of glue code, typically retrieved from a cache
  void addGlue(unsigned glueIndex, llvm::StringRef blob) override final;

  // Compile a particular chunk of glue code and retrieve its blob
  llvm::StringRef compileGlue(unsigned glueIndex) override final;

  // Link the unlinked shader/part-pipeline ELFs and the compiled glue code into a pipeline ELF
  bool link(llvm::raw_pwrite_stream &outStream) override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Accessors

  lgc::PipelineState *getPipelineState() const { return m_pipelineState; }
  llvm::ArrayRef<OutputSection> getOutputSections() { return m_outputSections; }
  llvm::StringRef getStrings() { return m_strings; }
  llvm::SmallVectorImpl<llvm::ELF::Elf64_Sym> &getSymbols() { return m_symbols; }
  llvm::SmallVectorImpl<llvm::ELF::Elf64_Rel> &getRelocations() { return m_relocations; }
  llvm::SmallVectorImpl<llvm::ELF::Elf64_Rela> &getRelocationsA() { return m_relocationsA; }
  void setStringTableIndex(unsigned index) { m_ehdr.e_shstrndx = index; }
  llvm::StringRef getNotes() { return m_notes; }

  // Get string index in output ELF, adding to string table if necessary
  unsigned getStringIndex(llvm::StringRef string);

  // Get string index in output ELF.  Returns 0 if not found.
  unsigned findStringIndex(llvm::StringRef string);

  // Find symbol in output ELF. Returns 0 if not found.
  unsigned findSymbol(unsigned nameIndex);
  unsigned findSymbol(llvm::StringRef name);

private:
  // Processing when all inputs are done.
  void doneInputs();

  // Find where an input section contributes to an output section
  std::pair<unsigned, unsigned> findInputSection(ElfInput &elfInput, llvm::object::SectionRef section);

  // Read PAL metadata from an ELF file and merge it in to the PAL metadata that we already have
  void mergePalMetadataFromElf(llvm::object::ObjectFile &objectFile, bool isGlueCode);

  // Read ISA name string from an ELF file if not already done
  void readIsaName(llvm::object::ObjectFile &objectFile);

  // Write ISA name into the .note section.
  void writeIsaName(llvm::Align align);

  // Write the PAL metadata out into the .note section.
  void writePalMetadata(llvm::Align align);

  // Create a GlueShader object for each glue shader needed for this link.
  void createGlueShaders();

  // Insert glue shaders (if any).
  bool insertGlueShaders();

  // Returns true of the given elf contains just 1 shader.
  bool containsASingleShader(ElfInput &elf);

  lgc::PipelineState *m_pipelineState;                                  // PipelineState object
  llvm::SmallVector<ElfInput, 5> m_elfInputs;                           // ELF objects to link
  ElfInput *m_currentElfInput = nullptr;                                // Currently inserted ELF object
  llvm::SmallVector<std::unique_ptr<lgc::GlueShader>, 4> m_glueShaders; // Glue shaders needed for link
  llvm::SmallVector<llvm::StringRef, 5> m_glueStrings;                  // Strings to return for glue shader cache keys
  llvm::ELF::Elf64_Ehdr m_ehdr;                                         // Output ELF header, copied from first input
  llvm::SmallVector<OutputSection, 4> m_outputSections;                 // Output sections
  llvm::SmallVector<llvm::ELF::Elf64_Sym, 8> m_symbols;                 // Symbol table
  llvm::SmallVector<llvm::ELF::Elf64_Rel, 8> m_relocations;             // Relocations
  llvm::SmallVector<llvm::ELF::Elf64_Rela, 8> m_relocationsA;           // Relocations with explicit addend
  llvm::StringMap<unsigned> m_symbolMap;                                // Map from name to symbol index
  std::string m_strings;                                                // Strings for string table
  llvm::StringMap<unsigned> m_stringMap;                                // Map from string to string table index
  std::string m_notes;                                                  // Notes to go in .note section
  bool m_doneInputs = false;                                            // Set when caller has done adding inputs
  llvm::StringRef m_isaName;                                            // ISA name to include in the .note section
};

} // namespace lgc
