/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ElfLinker.cpp
 * @brief LLPC source file: Implementation class for linking unlinked shader/part-pipeline ELFs into pipeline ELF
 ***********************************************************************************************************************
 */
#include "lgc/ElfLinker.h"
#include "ColorExportShader.h"
#include "ElfLinkerImpl.h"
#include "GlueShader.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-elf-linker"

using namespace llvm;

namespace lgc {

using namespace Util;

// =====================================================================================================================
// Create ELF linker given PipelineState and ELFs to link
ElfLinker *createElfLinkerImpl(PipelineState *pipelineState, ArrayRef<MemoryBufferRef> elfs) {
  return new ElfLinkerImpl(pipelineState, elfs);
}

// =====================================================================================================================
// Constructor given PipelineState and ELFs to link
//
// @param pipelineState : PipelineState object
// @param elfs : Array of unlinked ELF modules to link
ElfLinkerImpl::ElfLinkerImpl(PipelineState *pipelineState, ArrayRef<MemoryBufferRef> elfs)
    : m_pipelineState(pipelineState) {
  m_pipelineState->clearPalMetadata();

  // Add ELF inputs supplied here.
  for (MemoryBufferRef elf : elfs)
    addInputElf(elf, /*addAtStart=*/false);
}

// =====================================================================================================================
// Destructor
ElfLinkerImpl::~ElfLinkerImpl() {
}

// =====================================================================================================================
// Add another input ELF to the link.
void ElfLinkerImpl::addInputElf(MemoryBufferRef inputElf, bool addAtStart) {
  assert(!m_doneInputs && "Cannot use ElfLinker::addInputElf after other ElfLinker calls");
  ElfInput elfInput = {cantFail(object::ObjectFile::createELFObjectFile(inputElf))};
  // Populate output ELF header if this is the first one to be added.
  if (m_elfInputs.empty())
    memcpy(&m_ehdr, inputElf.getBuffer().data(), sizeof(ELF::Elf64_Ehdr));
  // Add the ELF.
  readIsaName(*elfInput.objectFile);
  mergePalMetadataFromElf(*elfInput.objectFile, false);
  m_currentElfInput = m_elfInputs.insert(addAtStart ? m_elfInputs.begin() : m_elfInputs.end(), std::move(elfInput));
}

// =====================================================================================================================
// Check whether we have FS input mappings, and thus whether we're doing part-pipeline compilation of the
// pre-FS part of the pipeline.
bool ElfLinkerImpl::haveFsInputMappings() {
  return m_pipelineState->getPalMetadata()->haveFsInputMappings();
}

// =====================================================================================================================
// Get a representation of the fragment shader input mappings from the PAL metadata of ELF input(s) added so far.
StringRef ElfLinkerImpl::getFsInputMappings() {
  return m_pipelineState->getPalMetadata()->getFsInputMappings();
}

// =====================================================================================================================
// Processing when all inputs are done.
void ElfLinkerImpl::doneInputs() {
  if (m_doneInputs)
    return;
  m_doneInputs = true;

  // Create any needed glue shaders.
  createGlueShaders();
}

// =====================================================================================================================
// Create a GlueShader object for each glue shader needed for this link. This does not actually create
// the glue shaders themselves, just the GlueShader objects that represent them.
void ElfLinkerImpl::createGlueShaders() {
  if (m_pipelineState->isGraphics() && !this->m_pipelineState->getShaderStageMask().contains(ShaderStage::Fragment)) {
    m_glueShaders.push_back(GlueShader::createNullFragmentShader(m_pipelineState));
  }

  // Create a color export shader if we need one.
  SmallVector<ColorExportInfo, 8> exports;
  m_pipelineState->getPalMetadata()->getColorExportInfo(exports);
  if (!exports.empty()) {
    m_glueShaders.push_back(GlueShader::createColorExportShader(m_pipelineState, exports));
  }
}

// =====================================================================================================================
// Get information on the glue code that will be needed for the link. It is an implementation detail how many
// chunks of glue there might be and what they are for, but, for information, they will be some subset of:
// - A CS prolog
// - A VS prolog ("fetch shader")
// - A vertex-processing epilog ("parameter export shader")
// - An FS epilog ("color export shader")
//
// Returns an array (possibly 0 length) with an entry for each chunk of glue code, where an entry
// is a StringRef that the client can hash for its cache lookup. If it gets a cache hit, it should provide the
// found blob to ElfLinker::addGlue. If it does not get a cache hit, the client can call ElfLinker::compileGlue to
// retrieve the compiled glue code to store in the cache.
ArrayRef<StringRef> ElfLinkerImpl::getGlueInfo() {
  doneInputs();
  if (m_glueShaders.empty())
    return {};
  if (m_glueStrings.empty()) {
    // Get the array of strings for glue shaders.
    for (auto &glueShader : m_glueShaders)
      m_glueStrings.push_back(glueShader->getString());
  }
  return m_glueStrings;
}

// =====================================================================================================================
// Create color export shader
//
// @param exports : Fragment export info
// @param enableKill : Whether this fragment shader has kill enabled.
// @param zFmt : depth-export-format
StringRef ElfLinkerImpl::createColorExportShader(ArrayRef<ColorExportInfo> exports, bool enableKill) {
  assert(m_glueShaders.empty());
  m_glueShaders.push_back(GlueShader::createColorExportShader(m_pipelineState, exports));
  ColorExportShader *copyColorShader = static_cast<ColorExportShader *>(m_glueShaders[0].get());
  if (enableKill)
    copyColorShader->enableKill();
  m_doneInputs = true;

  return copyColorShader->getString();
}

// =====================================================================================================================
// Add a blob for a particular chunk of glue code, typically retrieved from a cache.
//
// @param glueIndex : Index into the array that was returned by getGlueInfo()
// @param blob : Blob for the glue code
void ElfLinkerImpl::addGlue(unsigned glueIndex, StringRef blob) {
  doneInputs();
  m_glueShaders[glueIndex]->setElfBlob(blob);
}

// =====================================================================================================================
// Compile a particular chunk of glue code and retrieve its blob. The returned blob remains valid until the first
// of calling link() or the ElfLinker's parent Pipeline being destroyed. It is optional to call this; any chunk
// of glue code that has not had one of addGlue() or compileGlue() done by the time link() is called will be
// internally compiled. The client only needs to call this if it wants to cache the glue code's blob.
//
// @param glueIndex : Index into the array that was returned by getGlueInfo()
// @returns : The blob. A zero-length blob indicates that a recoverable error occurred, and link() will also return
//           and empty ELF blob.
StringRef ElfLinkerImpl::compileGlue(unsigned glueIndex) {
  doneInputs();
  return m_glueShaders[glueIndex]->getElfBlob();
}

// =====================================================================================================================
// Link the unlinked shader/part-pipeline ELFs and the compiled glue code into a pipeline ELF.
// Three ways this can exit:
// 1. On success, returns true.
// 2. Returns false on failure due to something in the shaders or pipeline state making separate
//    compilation and linking impossible. The client typically then does a whole-pipeline
//    compilation instead. The client can call Pipeline::getLastError() to get a textual representation of the
//    error, for use in logging or in error reporting in a command-line utility.
// 3. Other failures cause exit by report_fatal_error. The client can that catch by setting a diagnostic handler
//    with LLVMContext::setDiagnosticHandler, although the usefulness of that is limited, as no attempt is
//    made by LLVM to avoid memory leaks.
//
// @param [out] outStream : Stream to write linked ELF to
// @returns : True for success, false if something about the pipeline state stops linking
bool ElfLinkerImpl::link(raw_pwrite_stream &outStream) {
  // The call to doneInputs creates any needed glue shaders, but we only need to do it here for unlinked shaders.
  if (m_pipelineState->isUnlinked())
    doneInputs();

  // Insert glue shaders (if any).
  if (!insertGlueShaders())
    return false;

  // Initialize symbol table and string table
  m_symbols.push_back({});
  m_strings = std::string("", 1);
  m_stringMap[""] = 0;
  // Pre-create four fixed sections at the start:
  // 0: unused (per ELF spec)
  // 1: string table
  // 2: symbol table
  // 3: .text
  // 4: .note
  // 5: .rel.text
  m_outputSections.push_back(OutputSection(this, "", ELF::SHT_NULL));
  m_outputSections.push_back(OutputSection(this, ".strtab", ELF::SHT_STRTAB));
  m_outputSections.push_back(OutputSection(this, ".symtab", ELF::SHT_SYMTAB));
  unsigned textSectionIdx = m_outputSections.size();
  m_outputSections.push_back(OutputSection(this, ".text"));
  unsigned noteSectionIdx = m_outputSections.size();
  m_outputSections.push_back(OutputSection(this, ".note", ELF::SHT_NOTE));
  // The creation of the relocation section is delayed below so we can create it only if there is at least one
  // relocation.
  bool relSectionCreated = false;
  bool relaSectionCreated = false;

  // Allocate input sections to output sections.
  for (auto &elfInput : m_elfInputs) {
    for (const object::SectionRef &section : elfInput.objectFile->sections()) {
      unsigned sectType = object::ELFSectionRef(section).getType();
      if (sectType == ELF::SHT_REL) {
        if (!relSectionCreated && !section.relocations().empty()) {
          m_outputSections.push_back(OutputSection(this, ".rel.text", ELF::SHT_REL));
          relSectionCreated = true;
        }
      } else if (sectType == ELF::SHT_RELA) {
        if (!relaSectionCreated && !section.relocations().empty()) {
          m_outputSections.push_back(OutputSection(this, ".rela.text", ELF::SHT_RELA));
          relaSectionCreated = true;
        }
      } else if (sectType == ELF::SHT_PROGBITS) {
        // Put same-named sections together (excluding symbol table, string table, reloc sections).
        StringRef name = cantFail(section.getName());
        bool reduceAlign = false;
        if (elfInput.reduceAlign != "")
          reduceAlign = name == elfInput.reduceAlign;
        for (unsigned idx = 1;; ++idx) {
          if (idx == m_outputSections.size()) {
            m_outputSections.push_back(OutputSection(this));
            m_outputSections[idx].addInputSection(elfInput, section, reduceAlign);
            break;
          }
          if (name == m_outputSections[idx].getName()) {
            m_outputSections[idx].addInputSection(elfInput, section, reduceAlign);
            break;
          }
        }
      }
    }
  }

  // Construct uninitialized section table, and write partly-initialized ELF header and uninitialized
  // section table as a placeholder.
  assert(outStream.tell() == 0);
  SmallVector<ELF::Elf64_Shdr, 8> shdrs(m_outputSections.size());
  m_ehdr.e_shoff = sizeof(m_ehdr);
  m_ehdr.e_shnum = m_outputSections.size();
  outStream << StringRef(reinterpret_cast<const char *>(&m_ehdr), sizeof(m_ehdr));
  outStream << StringRef(reinterpret_cast<const char *>(shdrs.data()), sizeof(ELF::Elf64_Shdr) * shdrs.size());

  // Allow each output section to fix its layout. Also ensure that its name is in the string table.
  for (OutputSection &outputSection : m_outputSections) {
    outputSection.layout();
    getStringIndex(outputSection.getName());
  }

  // Find public symbols in the input ELFs, and add them to the output ELF.
  for (auto &elfInput : m_elfInputs) {
    for (object::SymbolRef symRef : elfInput.objectFile->symbols()) {
      object::ELFSymbolRef elfSymRef(symRef);
      StringRef name = cantFail(elfSymRef.getName());
      if (name == "llvmir" && findSymbol(getStringIndex(name)) != 0)
        continue;
      if (elfSymRef.getBinding() == ELF::STB_GLOBAL) {
        object::section_iterator containingSect = cantFail(elfSymRef.getSection());
        if (containingSect != elfInput.objectFile->section_end()) {
          auto outputIndices = findInputSection(elfInput, *containingSect);
          if (outputIndices.first != UINT_MAX)
            m_outputSections[outputIndices.first].addSymbol(elfSymRef, outputIndices.second);
        }
      }
    }
  }

  // Update the size of the symbols that had code appended to them.
  // Note that we currently cannot have the same shader get both an epilogue and a prologue.  However, if this does
  // happen the epilogue will have to come first in m_glueShaders.  This way the size of the epilogue will be added to
  // the size of the main shader, and then the updated size will be added to the size of the prologue to get the whole
  // shader.
  for (auto &glueShader : m_glueShaders) {
    auto glueNameStringIdx = getStringIndex(glueShader->getGlueShaderName());
    auto glueShaderSym = findSymbol(glueNameStringIdx);
    assert(glueShaderSym != 0);

    auto mainNameStringIdx = getStringIndex(glueShader->getMainShaderName());
    auto mainSym = findSymbol(mainNameStringIdx);
    assert(mainSym != 0);

    if (glueShader->isProlog()) {
      m_symbols[glueShaderSym].st_size =
          (m_symbols[mainSym].st_value + m_symbols[mainSym].st_size) - m_symbols[glueShaderSym].st_value;
    } else {
      m_symbols[mainSym].st_size =
          (m_symbols[glueShaderSym].st_value + m_symbols[glueShaderSym].st_size) - m_symbols[mainSym].st_value;
    }
  }

  // Add relocations that cannot be applied at this stage.
  for (auto &elfInput : m_elfInputs) {
    for (const object::SectionRef section : elfInput.objectFile->sections()) {
      unsigned sectType = object::ELFSectionRef(section).getType();
      if (sectType == ELF::SHT_REL || sectType == ELF::SHT_RELA) {
        for (object::RelocationRef reloc : section.relocations()) {
          unsigned targetSectionIdx = UINT_MAX;
          unsigned targetIdxInSection = UINT_MAX;
          std::tie(targetSectionIdx, targetIdxInSection) =
              findInputSection(elfInput, *cantFail(section.getRelocatedSection()));
          if (targetSectionIdx != UINT_MAX) {
            (void)(textSectionIdx);
            assert(targetSectionIdx == textSectionIdx && "We assume all relocations are applied to the text section");
            object::SectionRef relocSection = *cantFail(reloc.getSymbol()->getSection());
            unsigned relocSectionId = UINT_MAX;
            unsigned relocIdxInSection = UINT_MAX;
            std::tie(relocSectionId, relocIdxInSection) = findInputSection(elfInput, relocSection);
            uint64_t relocSectionOffset = m_outputSections[relocSectionId].getOutputOffset(relocIdxInSection);
            uint64_t targetSectionOffset = m_outputSections[targetSectionIdx].getOutputOffset(targetIdxInSection);
            StringRef id = sys::path::filename(elfInput.objectFile->getFileName());
            m_outputSections[relocSectionId].addRelocation(reloc, id, relocSectionOffset, targetSectionOffset,
                                                           sectType);
          }
        }
      }
    }
  }

  // Output each section, and let it set its section table entry.
  // Ensure each section is aligned in the file by the minimum of 4 and its address alignment requirement.
  // I am not sure if that is actually required by the ELF standard, but vkgcPipelineDumper.cpp relies on
  // it when dumping .note records.
  // The .note section will be emitted later.  We must wait until after processing the relocation to have all of the
  // metadata needed for the .note section.
  for (unsigned sectionIndex = 0; sectionIndex != shdrs.size(); ++sectionIndex) {
    if (sectionIndex == noteSectionIdx)
      continue;
    OutputSection &outputSection = m_outputSections[sectionIndex];
    Align align = std::min(outputSection.getAlignment(), Align(4));
    outStream << StringRef("\0\0\0", 3).slice(0, offsetToAlignment(outStream.tell(), align));
    shdrs[sectionIndex].sh_offset = outStream.tell();
    outputSection.write(outStream, &shdrs[sectionIndex]);
  }

  OutputSection &noteOutputSection = m_outputSections[noteSectionIdx];
  Align align = std::min(noteOutputSection.getAlignment(), Align(4));

  // Write ISA name into the .note section.
  writeIsaName(align);

  // Write the PAL metadata out into the .note section.  The relocations can change the metadata, so we cannot write the
  // PAL metadata any earlier.
  writePalMetadata(align);

  // Output the note section now that the metadata has been finalized.
  outStream << StringRef("\0\0\0", 3).slice(0, offsetToAlignment(outStream.tell(), align));
  shdrs[noteSectionIdx].sh_offset = outStream.tell();
  noteOutputSection.write(outStream, &shdrs[noteSectionIdx]);

  // Go back and write the now-complete ELF header and section table.
  outStream.pwrite(reinterpret_cast<const char *>(&m_ehdr), sizeof(m_ehdr), 0);
  outStream.pwrite(reinterpret_cast<const char *>(shdrs.data()), sizeof(ELF::Elf64_Shdr) * shdrs.size(),
                   sizeof(m_ehdr));

  return m_pipelineState->getLastError() == "";
}

// =====================================================================================================================
// Get string index in output ELF, adding to string table if necessary
unsigned ElfLinkerImpl::getStringIndex(StringRef string) {
  if (string == "")
    return 0;
  auto &stringMapEntry = m_stringMap[string];
  if (!stringMapEntry) {
    stringMapEntry = m_strings.size();
    m_strings += string;
    m_strings += '\0';
  }
  return stringMapEntry;
}

// =====================================================================================================================
// Get string index in output ELF.  Returns 0 if not found.
unsigned ElfLinkerImpl::findStringIndex(StringRef string) {
  return m_stringMap.lookup(string);
}

// =====================================================================================================================
// Find symbol in output ELF
//
// @param nameIndex : Index of symbol name in string table
// @returns : Index in symbol table, or 0 if not found
unsigned ElfLinkerImpl::findSymbol(unsigned nameIndex) {
  for (auto &sym : getSymbols()) {
    if (sym.st_name == nameIndex)
      return &sym - getSymbols().data();
  }
  return 0;
}

// =====================================================================================================================
// Find symbol in output ELF
//
// @param name: name of the symbol to find.
// @returns : Index in symbol table, or 0 if not found
unsigned ElfLinkerImpl::findSymbol(StringRef name) {
  unsigned nameIndex = findStringIndex(name);
  return findSymbol(nameIndex);
}

// =====================================================================================================================
// Find where an input section contributes to an output section
//
// @param elfInput : ElfInput object for the ELF input
// @param section : Section from that input
// @returns : {outputSectionIdx,withinIdx} pair, both elements UINT_MAX if no contribution to an output section
std::pair<unsigned, unsigned> ElfLinkerImpl::findInputSection(ElfInput &elfInput, object::SectionRef section) {
  unsigned idx = section.getIndex();
  if (idx >= elfInput.sectionMap.size())
    return {UINT_MAX, UINT_MAX};
  return elfInput.sectionMap[idx];
}

// =====================================================================================================================
// Read PAL metadata from an ELF file and merge it in to the PAL metadata that we already have
//
// @param objectFile : The ELF input
void ElfLinkerImpl::mergePalMetadataFromElf(object::ObjectFile &objectFile, bool isGlueCode) {
  for (const object::SectionRef &section : objectFile.sections()) {
    object::ELFSectionRef elfSection(section);
    if (elfSection.getType() == ELF::SHT_NOTE) {
      // This is a .note section. Find the PAL metadata note and merge it into the PalMetadata object
      // in the PipelineState.
      Error err = ErrorSuccess();
      auto &elfFile = cast<object::ELFObjectFile<object::ELF64LE>>(&objectFile)->getELFFile();
      auto shdr = cantFail(elfFile.getSection(elfSection.getIndex()));
      for (auto note : elfFile.notes(*shdr, err)) {
        if (note.getName() == Util::Abi::AmdGpuArchName && note.getType() == ELF::NT_AMDGPU_METADATA) {
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 460558
          // Old version of the code
          ArrayRef<uint8_t> desc = note.getDesc();
#else
          // New version of the code (also handles unknown version, which we treat as latest)
          ArrayRef<uint8_t> desc = note.getDesc(shdr->sh_addralign);
#endif
          m_pipelineState->mergePalMetadataFromBlob(StringRef(reinterpret_cast<const char *>(desc.data()), desc.size()),
                                                    isGlueCode);
        }
      }
    }
  }
}

// =====================================================================================================================
// Read ISA name string from an ELF file if not already done
//
// @param objectFile : The ELF input
void ElfLinkerImpl::readIsaName(object::ObjectFile &objectFile) {
  if (!m_isaName.empty())
    return;
  for (const object::SectionRef &section : objectFile.sections()) {
    object::ELFSectionRef elfSection(section);
    if (elfSection.getType() == ELF::SHT_NOTE) {
      Error err = ErrorSuccess();
      auto &elfFile = cast<object::ELFObjectFile<object::ELF64LE>>(&objectFile)->getELFFile();
      auto shdr = cantFail(elfFile.getSection(elfSection.getIndex()));
      for (auto note : elfFile.notes(*shdr, err)) {
        if (note.getName() == Util::Abi::AmdGpuVendorName && note.getType() == ELF::NT_AMD_HSA_ISA_NAME) {
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 460558
          // Old version of the code
          ArrayRef<uint8_t> desc = note.getDesc();
#else
          // New version of the code (also handles unknown version, which we treat as latest)
          ArrayRef<uint8_t> desc = note.getDesc(shdr->sh_addralign);
#endif
          m_isaName = StringRef(reinterpret_cast<const char *>(desc.data()), desc.size());
          return;
        }
      }
    }
  }
}

// =====================================================================================================================
// Write ISA name into the .note section.
void ElfLinkerImpl::writeIsaName(Align align) {
  StringRef noteName = Util::Abi::AmdGpuVendorName;
  typedef object::Elf_Nhdr_Impl<object::ELF64LE> NoteHeader;
  NoteHeader noteHeader;
  noteHeader.n_namesz = noteName.size() + 1;
  noteHeader.n_descsz = m_isaName.size();
  noteHeader.n_type = ELF::NT_AMD_HSA_ISA_NAME;
  m_notes.append(reinterpret_cast<const char *>(&noteHeader), sizeof(noteHeader));
  // Write the note name, followed by 1-4 zero bytes to terminate and align.
  m_notes += noteName;
  m_notes.append(offsetToAlignment(m_notes.size(), align), '\0');
  // Write ISA name, followed by 0-3 zero bytes to align.
  m_notes += m_isaName;
  m_notes.append(offsetToAlignment(m_notes.size(), align), '\0');
}

// =====================================================================================================================
// Write the PAL metadata out into the .note section.
void ElfLinkerImpl::writePalMetadata(Align align) {
  // Fix up user data registers.
  PalMetadata *palMetadata = m_pipelineState->getPalMetadata();
  palMetadata->fixUpRegisters();
  for (auto &glueShader : m_glueShaders)
    glueShader->updatePalMetadata(*palMetadata);

  // Finalize the PAL metadata, writing pipeline state items into it.
  palMetadata->finalizePipeline(/*isWholePipeline=*/true);
  // Write the MsgPack document into a blob.
  std::string blob;
  palMetadata->getDocument()->writeToBlob(blob);
  // Write the note header.
  StringRef noteName = Util::Abi::AmdGpuArchName;
  typedef object::Elf_Nhdr_Impl<object::ELF64LE> NoteHeader;
  NoteHeader noteHeader;
  noteHeader.n_namesz = noteName.size() + 1;
  noteHeader.n_descsz = blob.size();
  noteHeader.n_type = ELF::NT_AMDGPU_METADATA;
  m_notes.append(reinterpret_cast<const char *>(&noteHeader), sizeof(noteHeader));
  // Write the note name, followed by 1-4 zero bytes to terminate and align.
  m_notes += noteName;
  m_notes.append(offsetToAlignment(m_notes.size(), align), '\0');
  // Write the blob, followed by 0-3 zero bytes to align.
  m_notes += blob;
  m_notes.append(offsetToAlignment(m_notes.size(), align), '\0');
}

// =====================================================================================================================
// Insert glue shaders (if any).
//
// @returns : False if a recoverable error occurred and was reported with setError().
bool ElfLinkerImpl::insertGlueShaders() {
  // Ensure glue code is compiled, and insert them as new input shaders.
  for (auto &glueShader : m_glueShaders) {
    // Compile the glue shader (if not already done), and parse the ELF.
    StringRef elfBlob = glueShader->getElfBlob();
    MemoryBufferRef elfBuffer(elfBlob, glueShader->getName());
    ElfInput glueElfInput = {cantFail(object::ObjectFile::createELFObjectFile(elfBuffer))};

    // Find the input ELF containing the main shader that the glue shader wants to attach to.
    StringRef mainName = glueShader->getMainShaderName();
    unsigned insertPos = UINT_MAX;

    if (mainName == glueShader->getGlueShaderName()) {
      // In this case, the glue shader is a stand alone shader.  The null fragment shader is an example.
      mergePalMetadataFromElf(*glueElfInput.objectFile, false);
      m_elfInputs.push_back(std::move(glueElfInput));
      return true;
    }

    for (unsigned idx = 0; idx != m_elfInputs.size(); ++idx) {
      ElfInput &elfInput = m_elfInputs[idx];
      for (auto sym : elfInput.objectFile->symbols()) {
        if (cantFail(sym.getName()) == mainName) {
          // Found it. Find other STT_FUNC symbols in the same text section so we can check the validity of
          // gluing the glue shader on.
          uint64_t symValue = cantFail(sym.getValue()), maxValue = symValue;
          auto section = cantFail(sym.getSection());
          auto type = cantFail(sym.getType());
          for (auto otherSym : elfInput.objectFile->symbols()) {
            if (cantFail(otherSym.getSection()) == section && cantFail(otherSym.getType()) == type)
              maxValue = std::max(maxValue, cantFail(otherSym.getValue()));
          }
          if (glueShader->isProlog()) {
            // For a prolog glue shader, we can only cope if the main shader is at the start of its text section.
            // We can reduce the alignment of the main shader from 0x100 to 0x40, but only if there are no
            // other shaders in its text section.
            if (symValue != 0) {
              getPipelineState()->setError("Shader " + mainName + " is not at the start of its text section");
              return false;
            }

            // You cannot reduce the alignment if the elfInput has more than one
            // shader.  Otherwise the other shaders could be misaligned.
            if (containsASingleShader(elfInput))
              elfInput.reduceAlign = cantFail(section->getName());
            insertPos = idx;
          } else {
            // For an epilog glue shader, we can only cope if the main shader is the last one in its text section.
            // Also we reduce the alignment of the glue shader from 0x100 to 0x40.
            if (symValue != maxValue) {
              getPipelineState()->setError("Shader " + mainName + " is not at the end of its text section");
              return false;
            }
            glueElfInput.reduceAlign = cantFail(section->getName());
            insertPos = idx + 1;
          }
          break;
        }
      }
    }

    // Merge PAL metadata from glue ELF.
    // Note that the merger callback in PalMetadata.cpp relies on the PAL metadata for the shader/part-pipeline
    // ELFs being read first, and the glue shaders being merged in afterwards.
    mergePalMetadataFromElf(*glueElfInput.objectFile, true);

    // Insert the glue shader in the appropriate place in the list of ELFs.
    assert(insertPos != UINT_MAX && "Main shader not found for glue shader");
    m_elfInputs.insert(m_elfInputs.begin() + insertPos, std::move(glueElfInput));
  }
  return true;
}

// =====================================================================================================================
// Returns true of the given elf contains just 1 shader.
bool ElfLinkerImpl::containsASingleShader(ElfInput &elf) {
  bool foundAFunction = false;
  for (auto sym : elf.objectFile->symbols()) {
    if (cantFail(sym.getType()) != llvm::object::SymbolRef::Type::ST_Function)
      continue;
    if (foundAFunction)
      return false;
    foundAFunction = true;
  }
  return true;
}

// =====================================================================================================================
// Add an input section to this output section
//
// @param elfInput : ELF input that the section comes from
// @param inputSectionRef : Input section to add to this output section
// @param reduceAlign : Reduce the alignment of the section (for gluing code together)
void OutputSection::addInputSection(ElfInput &elfInput, object::SectionRef inputSectionRef, bool reduceAlign) {
  // Add the input section.
  InputSection inputSection(inputSectionRef);

  m_inputSections.push_back(inputSection);

  // Remember reduceAlign request.
  if (reduceAlign)
    setReduceAlign(m_inputSections.back());
  // Add an entry to the ElfInput's sectionMap, so we can get from an input section to where it contributes
  // to an output section.
  unsigned idx = inputSectionRef.getIndex();
  if (idx >= elfInput.sectionMap.size())
    elfInput.sectionMap.resize(idx + 1, {UINT_MAX, UINT_MAX});
  elfInput.sectionMap[idx] = {getIndex(), m_inputSections.size() - 1};
}

// =====================================================================================================================
// Get the name of this output section
StringRef OutputSection::getName() {
  if (!m_name.empty())
    return m_name;
  if (m_inputSections.empty())
    return "";
  return cantFail(m_inputSections[0].sectionRef.getName());
}

// =====================================================================================================================
// Get the index of this output section
unsigned OutputSection::getIndex() {
  return this - m_linker->getOutputSections().data();
}

// =====================================================================================================================
// Set the layout of this output section, allowing for alignment required by input sections.
// Also copy global symbols for each input section to the output ELF's symbol table.
// This is done as an initial separate step so that in the future we could support a reloc in one output section
// referring to a symbol in a different output section. But we do not currently support that.
void OutputSection::layout() {
  uint64_t size = 0;
  for (InputSection &inputSection : m_inputSections) {
    if (object::ELFSectionRef(inputSection.sectionRef).getFlags() & ELF::SHF_EXECINSTR) {
      // Remove GFX10 s_end_code padding by removing any suffix of the section that is not inside a function symbol.
      inputSection.size = 0;
      for (auto &sym : inputSection.sectionRef.getObject()->symbols()) {
        if (cantFail(sym.getSection()) == inputSection.sectionRef &&
            cantFail(sym.getType()) == object::SymbolRef::ST_Function) {
          inputSection.size =
              std::max(inputSection.size, cantFail(sym.getValue()) + object::ELFSymbolRef(sym).getSize());
        }
      }
      if (inputSection.size == 0) {
        // No function symbols found. We'd better restore the size to the size of the whole section.
        inputSection.size = inputSection.sectionRef.getSize();
      }
    }

    // Gain alignment as required for the next input section.
    Align alignment = getAlignment(inputSection);
    m_alignment = std::max(m_alignment, alignment);
    size = alignTo(size, alignment);
    // Store the start offset for the section.
    inputSection.offset = size;
    // Add on the size for this section.
    size += inputSection.size;
  }
  if (m_type == ELF::SHT_NOTE)
    m_alignment = Align(4);
}

// =====================================================================================================================
// Get alignment for an input section. This takes into account the reduceAlign flag, reducing the alignment
// from 0x100 to 0x40 when gluing code together.
//
// @param inputSection : InputSection
Align OutputSection::getAlignment(const InputSection &inputSection) {
  Align alignment = Align(inputSection.sectionRef.getAlignment());
  // Check if alignment is reduced for this section
  // for gluing code together.
  if (alignment > 0x40 && getReduceAlign(inputSection))
    alignment = Align(0x40);
  return alignment;
}

// =====================================================================================================================
// Add a symbol to the output symbol table
//
// @param elfSymRef : The symbol from an input ELF
// @param inputSectIdx : Index of input section within this output section that the symbol refers to
void OutputSection::addSymbol(const object::ELFSymbolRef &elfSymRef, unsigned inputSectIdx) {
  const InputSection &inputSection = m_inputSections[inputSectIdx];
  StringRef name = cantFail(elfSymRef.getName());
  ELF::Elf64_Sym newSym = {};
  newSym.st_name = m_linker->getStringIndex(name);
  newSym.setBinding(elfSymRef.getBinding());
  newSym.setType(elfSymRef.getELFType());
  newSym.st_shndx = getIndex();
  newSym.st_value = cantFail(elfSymRef.getValue()) + inputSection.offset;
  newSym.st_size = elfSymRef.getSize();
  if (m_linker->findSymbol(newSym.st_name) != 0)
    report_fatal_error("Duplicate symbol '" + name + "'");
  m_linker->getSymbols().push_back(newSym);
}

// Add a relocation to the output elf
void OutputSection::addRelocation(object::ELFRelocationRef relocRef, StringRef id, unsigned int relocSectionOffset,
                                  unsigned int targetSectionOffset, unsigned sectType) {
  object::ELFSymbolRef relocSymRef(*relocRef.getSymbol());
  std::string rodataSymName = cantFail(relocSymRef.getName()).str();
  rodataSymName += ".";
  rodataSymName += id;
  unsigned rodataSymIdx = m_linker->findSymbol(rodataSymName);
  if (rodataSymIdx == 0) {
    // Create the ".rodata" symbol
    ELF::Elf64_Sym newSym = {};
    newSym.st_name = m_linker->getStringIndex(rodataSymName);
    newSym.setBinding(ELF::STB_LOCAL);
    newSym.setType(ELF::STT_OBJECT);
    newSym.st_shndx = getIndex();
    newSym.st_value = relocSectionOffset + cantFail(relocSymRef.getValue());
    newSym.st_size = relocSymRef.getSize();
    rodataSymIdx = m_linker->getSymbols().size();
    m_linker->getSymbols().push_back(newSym);
  }
  if (sectType == ELF::SHT_REL) {
    ELF::Elf64_Rel newReloc = {};
    newReloc.setSymbolAndType(rodataSymIdx, relocRef.getType());
    newReloc.r_offset = targetSectionOffset + relocRef.getOffset();
    m_linker->getRelocations().push_back(newReloc);
  } else {
    assert(sectType == ELF::SHT_RELA);
    ELF::Elf64_Rela newReloc = {};
    newReloc.setSymbolAndType(rodataSymIdx, relocRef.getType());
    newReloc.r_offset = targetSectionOffset + relocRef.getOffset();
    newReloc.r_addend = cantFail(relocRef.getAddend());
    m_linker->getRelocationsA().push_back(newReloc);
  }
}

// =====================================================================================================================
// Write the output section
//
// @param [in/out] outStream : Stream to write the section to
// @param [in/out] shdr : ELF section header to write to (but not sh_offset)
void OutputSection::write(raw_pwrite_stream &outStream, ELF::Elf64_Shdr *shdr) {
  shdr->sh_name = m_linker->getStringIndex(getName());
  m_offset = outStream.tell();

  if (m_type == ELF::SHT_STRTAB) {
    StringRef strings = m_linker->getStrings();
    shdr->sh_type = m_type;
    shdr->sh_size = strings.size();
    m_linker->setStringTableIndex(getIndex());
    outStream << strings;
    return;
  }

  if (m_type == ELF::SHT_SYMTAB) {
    ArrayRef<ELF::Elf64_Sym> symbols = m_linker->getSymbols();
    shdr->sh_type = m_type;
    shdr->sh_size = symbols.size() * sizeof(ELF::Elf64_Sym);
    shdr->sh_entsize = sizeof(ELF::Elf64_Sym);
    shdr->sh_link = 1; // Section index of string table
    outStream << StringRef(reinterpret_cast<const char *>(symbols.data()), symbols.size() * sizeof(ELF::Elf64_Sym));
    return;
  }

  if (m_type == ELF::SHT_NOTE) {
    StringRef notes = m_linker->getNotes();
    shdr->sh_type = m_type;
    shdr->sh_size = notes.size();
    outStream << notes;
    return;
  }

  if (m_type == ELF::SHT_REL) {
    ArrayRef<ELF::Elf64_Rel> relocations = m_linker->getRelocations();
    shdr->sh_type = m_type;
    shdr->sh_size = relocations.size() * sizeof(ELF::Elf64_Rel);
    shdr->sh_entsize = sizeof(ELF::Elf64_Rel);
    shdr->sh_link = 2; // Section index of symbol table
    shdr->sh_info = 3; // Section index of the .text section
    outStream << StringRef(reinterpret_cast<const char *>(relocations.data()),
                           relocations.size() * sizeof(ELF::Elf64_Rel));
    return;
  }

  if (m_type == ELF::SHT_RELA) {
    ArrayRef<ELF::Elf64_Rela> relocations = m_linker->getRelocationsA();
    shdr->sh_type = m_type;
    shdr->sh_size = relocations.size() * sizeof(ELF::Elf64_Rela);
    shdr->sh_entsize = sizeof(ELF::Elf64_Rela);
    shdr->sh_link = 2; // Section index of symbol table
    shdr->sh_info = 3; // Section index of the .text section
    outStream << StringRef(reinterpret_cast<const char *>(relocations.data()),
                           relocations.size() * sizeof(ELF::Elf64_Rela));
    return;
  }

  if (m_inputSections.empty())
    return;

  // This section has contributions from input sections. Get the type and flags from the first input section.
  shdr->sh_type = object::ELFSectionRef(m_inputSections[0].sectionRef).getType();
  shdr->sh_flags = object::ELFSectionRef(m_inputSections[0].sectionRef).getFlags();

  // Set up the pattern we will use for alignment padding.
  const size_t paddingUnit = 16;
  const char *padding = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
  const char *endPadding = nullptr;
  if (shdr->sh_flags & ELF::SHF_EXECINSTR) {
    padding = "\0\0\x80\xBF\0\0\x80\xBF\0\0\x80\xBF\0\0\x80\xBF";    // s_nop
    endPadding = "\0\0\x9F\xBF\0\0\x9F\xBF\0\0\x9F\xBF\0\0\x9F\xBF"; // s_code_end
  }

  // Output the contributions from the input sections.
  uint64_t size = 0;
  for (InputSection &inputSection : m_inputSections) {
    assert(m_alignment >= getAlignment(inputSection));
    // Gain alignment as required for the next input section.
    uint64_t alignmentGap = offsetToAlignment(size, getAlignment(inputSection));
    while (alignmentGap != 0) {
      size_t thisSize = std::min(alignmentGap, paddingUnit - (size & (paddingUnit - 1)));
      outStream << StringRef(&padding[size & (paddingUnit - 1)], thisSize);
      alignmentGap -= thisSize;
      size += thisSize;
    }

    // Write the input section
    StringRef contents = cantFail(inputSection.sectionRef.getContents());
    uint64_t contentSize = inputSection.size;
    outStream << contents.slice(0, contentSize);
    size += contentSize;
  }

  if (endPadding) {
    // On GFX10 in .text, also add padding at the end of the section: align to an instruction cache line
    // boundary, then add another 3 cache lines worth of padding.
    uint64_t cacheLineSize = 64;
    if (m_linker->getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 11)
      cacheLineSize = 128;

    uint64_t alignmentGap = (-size & (cacheLineSize - 1)) + 3 * cacheLineSize;
    while (alignmentGap != 0) {
      size_t thisSize = std::min(alignmentGap, paddingUnit - (size & (paddingUnit - 1)));
      outStream << StringRef(&endPadding[size & (paddingUnit - 1)], thisSize);
      alignmentGap -= thisSize;
      size += thisSize;
    }
  }

  shdr->sh_size = size;
  shdr->sh_addralign = m_alignment.value();
}

} // namespace lgc
