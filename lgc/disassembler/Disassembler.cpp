/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Disassembler.cpp
 * @brief LGC disassembler library
 ***********************************************************************************************************************
 */
#include "lgc/Disassembler.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/AMDGPUMetadata.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"

using namespace llvm;
using namespace object;

namespace lgc {
// Get name of PAL metadata register, or "" if not known
const char *getPalMetadataRegName(unsigned regNumber);
} // namespace lgc

namespace {

// Class for the object file disassembler.
class ObjDisassembler {
  MemoryBufferRef m_data;
  std::unique_ptr<ELFObjectFileBase> m_objFile;
  raw_ostream &m_ostream;
  std::string m_tripleName;
  const Target *m_target = nullptr;
  std::unique_ptr<MCRegisterInfo> m_mcRegInfo;
  std::unique_ptr<MCSubtargetInfo> m_subtargetInfo;
  std::unique_ptr<MCStreamer> m_streamer;
  std::unique_ptr<MCDisassembler> m_instDisassembler;
  MCContext *m_context = nullptr;
  std::vector<object::RelocationRef> relocs;

public:
  static void disassembleObject(MemoryBufferRef data, raw_ostream &ostream) {
    ObjDisassembler objDis(data, ostream);
    objDis.run();
  }

private:
  ObjDisassembler(MemoryBufferRef data, raw_ostream &ostream) : m_data(data), m_ostream(ostream) {}

  void run();
  void processSection(ELFSectionRef sectionRef);
  void gatherSectionSymbols(ELFSectionRef sectionRef, SectionSymbolsTy &symbols,
                            std::vector<std::unique_ptr<std::string>> &synthesizedLabels);
  void gatherRelocs(ELFSectionRef sectionRef, std::vector<object::RelocationRef> &relocs);
  void tryDisassembleSection(ELFSectionRef sectionRef, unsigned sectType, unsigned sectFlags, bool outputting,
                             ArrayRef<SymbolInfoTy> symbols, ArrayRef<object::RelocationRef> relocs);
  void addBinaryEncodingComment(raw_ostream &stream, unsigned instAlignment, ArrayRef<uint8_t> instBytes);
  void outputData(bool outputting, uint64_t offset, StringRef data, ArrayRef<object::RelocationRef> &relocs);
  void outputRelocs(bool outputting, uint64_t offset, uint64_t size, ArrayRef<object::RelocationRef> &relocs);
  size_t decodeNote(StringRef data);

  support::endianness endian() { return m_objFile->isLittleEndian() ? support::little : support::big; }
};

} // anonymous namespace

// =====================================================================================================================
// Disassemble an ELF object into ostream. Does report_fatal_error on error.
//
// @param data : The object file contents
// @param ostream : The stream to disassemble into
void lgc::disassembleObject(MemoryBufferRef data, raw_ostream &ostream) {
  // Initialize targets and assembly printers/parsers.
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllDisassemblers();

  // Do the disassembly.
  ObjDisassembler::disassembleObject(data, ostream);
}

// =====================================================================================================================
// Run the object disassembler to disassemble the object. Does report_fatal_error on error.
void ObjDisassembler::run() {
  // Decode the object file.
  Expected<std::unique_ptr<ObjectFile>> expectedObjFile = ObjectFile::createELFObjectFile(m_data);
  if (!expectedObjFile)
    report_fatal_error(m_data.getBufferIdentifier() + ": Cannot decode ELF object file");
  if (!isa<ELFObjectFileBase>(&*expectedObjFile.get()))
    report_fatal_error(m_data.getBufferIdentifier() + ": Is not ELF object file");
  m_objFile.reset(cast<ELFObjectFileBase>(expectedObjFile.get().release()));

  // Figure out the target triple from the object file, and get features.
  Triple triple = m_objFile->makeTriple();
  SubtargetFeatures features = m_objFile->getFeatures();

  // Get the target specific parser.
  std::string error;
  m_tripleName = triple.getTriple();
  m_target = TargetRegistry::lookupTarget(m_tripleName, error);
  if (!m_target)
    report_fatal_error(m_objFile->getFileName() + ": '" + m_tripleName + "': " + error);

  // Get the CPU name.
  Optional<StringRef> mcpu = m_objFile->tryGetCPUName();
  if (!mcpu)
    report_fatal_error(m_objFile->getFileName() + ": Cannot get CPU name");

  // Output the required llvm-mc command as a comment.
  m_ostream << "// llvm-mc -triple=" << m_tripleName << " -mcpu=" << mcpu << "\n";

  // Set up other objects required for disassembly.
  std::unique_ptr<MCRegisterInfo> regInfo(m_target->createMCRegInfo(m_tripleName));
  if (!regInfo)
    report_fatal_error(m_data.getBufferIdentifier() + ": No register info for target");
  std::unique_ptr<MCAsmInfo> asmInfo(m_target->createMCAsmInfo(*regInfo, m_tripleName, MCTargetOptions()));
  if (!asmInfo)
    report_fatal_error(m_data.getBufferIdentifier() + ": No assembly info for target");
  m_subtargetInfo.reset(m_target->createMCSubtargetInfo(m_tripleName, *mcpu, features.getString()));
  if (!m_subtargetInfo)
    report_fatal_error(m_data.getBufferIdentifier() + ": No subtarget info for target");
  std::unique_ptr<MCInstrInfo> instrInfo(m_target->createMCInstrInfo());
  if (!instrInfo)
    report_fatal_error(m_data.getBufferIdentifier() + ": No instruction info for target");

  MCObjectFileInfo objFileInfo;
  MCContext context(triple, asmInfo.get(), regInfo.get(), &objFileInfo, m_subtargetInfo.get());
  m_context = &context;
  objFileInfo.initMCObjectFileInfo(context, false);

  m_instDisassembler.reset(m_target->createMCDisassembler(*m_subtargetInfo, *m_context));
  if (!m_instDisassembler)
    report_fatal_error(m_data.getBufferIdentifier() + ": No disassembler for target");
  MCInstPrinter *instPrinter(
      m_target->createMCInstPrinter(triple, asmInfo->getAssemblerDialect(), *asmInfo, *instrInfo, *regInfo));
  if (!instPrinter)
    report_fatal_error(m_data.getBufferIdentifier() + ": No instruction printer for target");

  auto fostream = std::make_unique<formatted_raw_ostream>(m_ostream);
  m_streamer.reset(
      m_target->createAsmStreamer(*m_context, std::move(fostream), true, false, instPrinter, nullptr, nullptr, false));

  // Process each section.
  for (ELFSectionRef sectionRef : m_objFile->sections())
    processSection(sectionRef);
}

// =====================================================================================================================
// Disassemble one section.
//
// @param sectionRef : The section to disassemble
void ObjDisassembler::processSection(ELFSectionRef sectionRef) {
  // Omit certain ELF sections.
  unsigned sectType = sectionRef.getType();
  if (sectType == ELF::SHT_NULL || sectType == ELF::SHT_STRTAB || sectType == ELF::SHT_SYMTAB ||
      sectType == ELF::SHT_REL || sectType == ELF::SHT_RELA)
    return;

  // Switch the streamer to the section.
  m_streamer->AddBlankLine();
  unsigned sectFlags = sectionRef.getFlags();
  MCSection *sect = m_context->getELFSection(cantFail(sectionRef.getName()), sectType, sectFlags);
  m_streamer->SwitchSection(sect);

  // Create an array of all symbols in this section. Also emit directives for symbol type and size,
  // adding a synthesized label for the end of the symbol.
  SectionSymbolsTy symbols;
  std::vector<std::unique_ptr<std::string>> synthesizedLabels;
  gatherSectionSymbols(sectionRef, symbols, synthesizedLabels);

  // Collect and sort the relocs for the section.
  std::vector<object::RelocationRef> relocs;
  gatherRelocs(sectionRef, relocs);

  // Disassemble the section multiple times until no new synthesized labels appear, then disassemble
  // one final time actually streaming the output. For non-code, just use a single outputting pass.
  // The loop terminates after that one final time, at the "Done final outputting pass" comment below.
  bool outputting = !(sectFlags & ELF::SHF_EXECINSTR);
  for (;;) {
    // One iteration of disassembling the section.
    // Sort the symbols. (Stable sort as there may be duplicate addresses.)
    stable_sort(symbols);

    // If AMDGPU, create a symbolizer, giving it the symbols.
    MCSymbolizer *symbolizerPtr = nullptr;
    if (m_objFile->getArch() == Triple::amdgcn) {
      std::unique_ptr<MCRelocationInfo> relInfo(m_target->createMCRelocationInfo(m_tripleName, *m_context));
      if (relInfo) {
        std::unique_ptr<MCSymbolizer> symbolizer(
            m_target->createMCSymbolizer(m_tripleName, nullptr, nullptr, &symbols, m_context, std::move(relInfo)));
        symbolizerPtr = &*symbolizer;
        m_instDisassembler->setSymbolizer(std::move(symbolizer));
      }
    }

    // Disassemble the section contents.
    tryDisassembleSection(sectionRef, sectType, sectFlags, outputting, symbols, relocs);
    if (outputting)
      break; // Done final outputting pass.

    // If we created a symbolizer, get the referenced addresses from it and synthesize labels, avoiding
    // duplicates. If there were no new referenced addresses, then we can do the final output in the next
    // pass (indicated by the setting of outputting).
    outputting = true;
    if (symbolizerPtr && !symbolizerPtr->getReferencedAddresses().empty()) {
      outputting = false;
      std::vector<uint64_t> referencedAddresses;
      referencedAddresses.insert(referencedAddresses.begin(), symbolizerPtr->getReferencedAddresses().begin(),
                                 symbolizerPtr->getReferencedAddresses().end());
      llvm::sort(referencedAddresses.begin(), referencedAddresses.end());
      uint64_t lastLabel = ~uint64_t(0);
      for (uint64_t labelAddr : referencedAddresses) {
        if (labelAddr == lastLabel)
          continue;
        lastLabel = labelAddr;
        synthesizedLabels.push_back({});
        synthesizedLabels.back().reset(new std::string((Twine("_L") + Twine::utohexstr(labelAddr)).str()));
        symbols.push_back(SymbolInfoTy(labelAddr, *synthesizedLabels.back(), ELF::STT_NOTYPE));
      }
    }
  }
}

// =====================================================================================================================
// Create an array of all symbols in the given section. Also emit directives for symbol type and size.
// The size is an expression endSym-sym where endSym is a synthesized label at the end of the function.
//
// @param sectionRef : The section being disassembled
// @param [out] symbols : Vector of symbols to populate
// @param [out] synthesizedLabels : Vector of names for synthesized labels
void ObjDisassembler::gatherSectionSymbols(ELFSectionRef sectionRef, SectionSymbolsTy &symbols,
                                           std::vector<std::unique_ptr<std::string>> &synthesizedLabels) {
  for (ELFSymbolRef symbolRef : m_objFile->symbols()) {
    if (cantFail(symbolRef.getSection()) != sectionRef)
      continue;
    symbols.push_back(
        SymbolInfoTy(cantFail(symbolRef.getValue()), cantFail(symbolRef.getName()), symbolRef.getELFType()));

    MCSymbol *sym = m_context->getOrCreateSymbol(symbols.back().Name);
    switch (symbols.back().Type) {
    case ELF::STT_FUNC:
      m_streamer->emitSymbolAttribute(sym, MCSA_ELF_TypeFunction);
      break;
    case ELF::STT_OBJECT:
      m_streamer->emitSymbolAttribute(sym, MCSA_ELF_TypeObject);
      break;
    }

    if (uint64_t size = symbolRef.getSize()) {
      if (symbols.back().Addr + size <= cantFail(sectionRef.getContents()).size()) {
        synthesizedLabels.push_back(std::make_unique<std::string>((Twine(symbols.back().Name) + "_symend").str()));
        StringRef endName = *synthesizedLabels.back();
        MCSymbol *endSym = m_context->getOrCreateSymbol(endName);
        symbols.push_back(SymbolInfoTy(symbols.back().Addr + size, endName, ELF::STT_NOTYPE));
        const MCExpr *sizeExpr = MCBinaryExpr::createSub(MCSymbolRefExpr::create(endSym, *m_context),
                                                         MCSymbolRefExpr::create(sym, *m_context), *m_context);
        m_streamer->emitELFSize(sym, sizeExpr);
      }
    }
  }
}

// =====================================================================================================================
// Collect and sort the relocs for the given section.
//
// @param sectionRef : The section being disassembled
// @param [out] relocs : Vector to gather the relocs in
void ObjDisassembler::gatherRelocs(ELFSectionRef sectionRef, std::vector<object::RelocationRef> &relocs) {
  for (auto &relSect : m_objFile->sections()) {
    auto relocatedSectOr = relSect.getRelocatedSection();
    if (relocatedSectOr && *relocatedSectOr == sectionRef) {
      for (auto &reloc : relSect.relocations())
        relocs.push_back(reloc);
    }
  }
  llvm::sort(relocs.begin(), relocs.end(), [](const object::RelocationRef &a, const object::RelocationRef &b) {
    return a.getOffset() < b.getOffset();
  });
}

// =====================================================================================================================
// Try disassembling one section, possibly not outputting to see if any new synthesized labels get added.
//
// @param sectionRef : The section to disassemble
// @param sectType : ELF section type, so we can see if it is SHT_NOTE
// @param sectFlags : ELF section flags, so we can see if it is SHF_EXECINSTR
// @param outputting : True to actually stream the disassembly output
// @param symbols : Sorted array of symbols in the section
// @param relocs : Sorted array of relocs in the section
void ObjDisassembler::tryDisassembleSection(ELFSectionRef sectionRef, unsigned sectType, unsigned sectFlags,
                                            bool outputting, ArrayRef<SymbolInfoTy> symbols,
                                            ArrayRef<object::RelocationRef> relocs) {

  bool isCode = sectFlags & ELF::SHF_EXECINSTR;
  bool isNote = sectType == ELF::SHT_NOTE;
  unsigned instAlignment = 1;
  if (isCode)
    instAlignment = m_context->getAsmInfo()->getMinInstAlignment();

  // Get the section contents, and disassemble until nothing left.
  StringRef contents = cantFail(sectionRef.getContents());
  size_t offset = 0, lastOffset = 0;
  auto nextSymbol = symbols.begin();

  for (;;) {
    size_t endOffset = contents.size();
    if (nextSymbol != symbols.end() && nextSymbol->Addr < endOffset)
      endOffset = nextSymbol->Addr;

    if (offset == endOffset) {
      // We're about to emit a symbol or finish the section.
      // If there is any remaining non-disassemblable data, output it.
      if (lastOffset != offset) {
        outputData(outputting, lastOffset, contents.slice(lastOffset, offset), relocs);
        lastOffset = offset;
      }

      if (nextSymbol != symbols.end() && nextSymbol->Addr == offset) {
        // Output a symbol or label here.
        if (outputting) {
          MCSymbol *sym = m_context->getOrCreateSymbol(nextSymbol->Name);
          if (sym->isUndefined())
            m_streamer->emitLabel(sym);
        }
        ++nextSymbol;
        continue;
      }

      if (offset == contents.size())
        break;
    }

    if (isNote) {
      // Special handling for an ELF .note record.
      size_t eaten = decodeNote(contents.slice(offset, endOffset));
      if (eaten) {
        offset += eaten;
        lastOffset = offset;
      } else
        isNote = false;
      continue;
    }

    // Try disassembling an instruction.
    uint64_t instSize = 0;
    MCInst inst;
    ArrayRef<uint8_t> code(reinterpret_cast<const uint8_t *>(contents.data()) + offset, endOffset - offset);
    std::string comment;
    raw_string_ostream commentStream(comment);
    MCDisassembler::DecodeStatus status = MCDisassembler::Fail;
    if (isCode && (offset & instAlignment - 1) == 0)
      status = m_instDisassembler->getInstruction(inst, instSize, code, offset, commentStream);

    if (status == MCDisassembler::Fail) {
      // No disassemblable instruction here. Try the next instruction unit.
      offset = std::min(size_t(alignTo(offset + 1, instAlignment)), endOffset);
      continue;
    }

    // Got a disassemblable instruction.
    // First output any non-disassemblable data up to this point.
    if (lastOffset != offset) {
      outputData(outputting, lastOffset, contents.slice(lastOffset, offset), relocs);
      lastOffset = offset;
    }
    // Output reloc.
    outputRelocs(outputting, offset, instSize, relocs);

    if (outputting) {
      // Output the binary encoding as a comment.
      if (status == MCDisassembler::SoftFail)
        m_streamer->AddComment("Illegal instruction encoding ");
      if (!comment.empty())
        commentStream << " ";
      commentStream << format("%06x:", offset);
      addBinaryEncodingComment(commentStream, instAlignment, code.slice(0, instSize));
      // Output the instruction to the streamer.
      if (!comment.empty())
        m_streamer->AddComment(comment);
      m_streamer->emitInstruction(inst, *m_subtargetInfo);
    }
    offset += instSize;
    lastOffset = offset;
  }
}

// =====================================================================================================================
// Add binary encoding of an instruction as a comment
//
// @param [in/out] stream : Stream to output to
// @param instAlignment : Alignment (instruction unit size in bytes)
// @param instBytes : Array of bytes forming the instruction
void ObjDisassembler::addBinaryEncodingComment(raw_ostream &stream, unsigned instAlignment,
                                               ArrayRef<uint8_t> instBytes) {
  assert((instBytes.size() & (instAlignment - 1)) == 0);
  for (size_t subOffset = 0; subOffset != instBytes.size(); ++subOffset) {
    // This puts a space before each group of instAlignment bytes, and swizzles by
    // instAlignment if little-endian. That has the effect of dumping words of size
    // instAlignment.
    if ((subOffset & (instAlignment - 1)) == 0)
      stream << " ";
    unsigned byte = instBytes[subOffset];
    if (endian() == support::little)
      byte = instBytes[subOffset ^ (instAlignment - 1)];
    stream << format("%02x", byte);
  }
}

// =====================================================================================================================
// Output data, including relocs in that data (bumping the relocs array)
//
// @param outputting : True if outputting
// @param offset : Offset in section
// @param data : Bytes of data
// @param [in/out] relocs : ArrayRef of relocs, bumped on output past relocs that have been consumed
void ObjDisassembler::outputData(bool outputting, uint64_t offset, StringRef data,
                                 ArrayRef<object::RelocationRef> &relocs) {
  while (!data.empty()) {
    if (!relocs.empty() && relocs[0].getOffset() == offset)
      outputRelocs(outputting, offset, 1, relocs);

    size_t size = data.size();
    if (!relocs.empty())
      size = std::min(size, size_t(relocs[0].getOffset() - offset));

    if (outputting) {
      // Check whether the data is mostly ASCII, possibly with a terminating 0.
      size_t asciiCount = 0;
      for (size_t i = 0; i != size; ++i) {
        if (data[i] >= ' ' && data[i] <= '~')
          ++asciiCount;
      }
      if (asciiCount * 10 >= size * 9)
        m_streamer->emitBytes(data);
      else
        m_streamer->emitBinaryData(data);
    }
    offset += size;
    data = data.drop_front(size);
  }
}

// =====================================================================================================================
// Output any relocs in the given code/data range (bumping the relocs array).
// It is assumed that offset is $ (the current pc).
//
// @param outputting : True if outputting
// @param offset : Offset in section
// @param size : Size of range to output relocs for
// @param [in/out] relocs : ArrayRef of relocs, bumped on output past relocs that have been consumed
void ObjDisassembler::outputRelocs(bool outputting, uint64_t offset, uint64_t size,
                                   ArrayRef<object::RelocationRef> &relocs) {
  while (!relocs.empty() && relocs[0].getOffset() < offset + size) {
    if (outputting) {
      // Start with a '$' reference.
      MCSymbol *hereSym = m_context->createTempSymbol();
      m_streamer->emitLabel(hereSym);
      const MCExpr *offsetExpr = MCSymbolRefExpr::create(hereSym, MCSymbolRefExpr::VK_None, *m_context);
      // Add on the offset if any.
      if (relocs[0].getOffset() != offset)
        offsetExpr = MCBinaryExpr::createAdd(
            offsetExpr, MCConstantExpr::create(relocs[0].getOffset() - offset, *m_context), *m_context);
      // Get other info and emit the .reloc.
      SmallString<10> relocName;
      relocs[0].getTypeName(relocName);
      const MCExpr *tgtExpr = nullptr;
      auto symRef = relocs[0].getSymbol();
      if (symRef != m_objFile->symbol_end())
        tgtExpr = MCSymbolRefExpr::create(m_context->getOrCreateSymbol(cantFail(symRef->getName())), *m_context);
      m_streamer->emitRelocDirective(*offsetExpr, relocName, tgtExpr, {}, *m_subtargetInfo);
    }
    relocs = relocs.drop_front(1);
  }
}

// =====================================================================================================================
// Decode an ELF .note
//
// @param data : The .note record (including its header)
size_t ObjDisassembler::decodeNote(StringRef data) {
  constexpr unsigned NoteHeaderSize = 12;
  if (data.size() < NoteHeaderSize)
    return 0;
  unsigned nameSize = support::endian::read32(data.data(), endian());
  unsigned descSize = support::endian::read32(data.data() + 4, endian());
  unsigned type = support::endian::read32(data.data() + 8, endian());
  unsigned descOffset = NoteHeaderSize + alignTo<4>(nameSize);
  unsigned totalSize = descOffset + alignTo<4>(descSize);
  if (totalSize > data.size())
    return 0;
  StringRef name = data.slice(NoteHeaderSize, NoteHeaderSize + nameSize);
  StringRef desc = data.slice(descOffset, descOffset + descSize);
  if (name.empty() || name.back() != '\0')
    return 0;
  name = name.slice(0, name.size() - 1);

  if (name == "AMDGPU\0" && type == ELF::NT_AMDGPU_METADATA) {
    // AMDGPU metadata note, encoded as a msgpack blob.
    msgpack::Document msgPackDoc;
    if (msgPackDoc.readFromBlob(desc, false)) {
      // Change PAL metadata registers into mnemonic names.
      // TODO: We should use AMDGPUPALMetadata in the AMDGPU target to do this. But, to access it,
      // we would need a target-specific note-dumping API like in https://reviews.llvm.org/D52822
      auto &regs = msgPackDoc.getRoot()
                       .getMap(/*Convert=*/true)[msgPackDoc.getNode("amdpal.pipelines")]
                       .getArray(/*Convert=*/true)[0]
                       .getMap(/*Convert=*/true)[msgPackDoc.getNode(".registers")];
      auto origRegs = regs.getMap(/*Convert=*/true);
      regs = msgPackDoc.getMapNode();
      for (auto entry : origRegs) {
        auto key = entry.first;
        if (const char *regName = lgc::getPalMetadataRegName(key.getUInt())) {
          std::string keyName;
          raw_string_ostream(keyName) << format("%#x", unsigned(key.getUInt())) << " (" << regName << ")";
          key = msgPackDoc.getNode(keyName, /*Copy=*/true);
        }
        regs.getMap()[key] = entry.second;
      }
      // Output the MsgPack as YAML text.
      std::string outString;
      raw_string_ostream outStream(outString);
      msgPackDoc.setHexMode();
      outStream << '\t' << AMDGPU::PALMD::AssemblerDirectiveBegin << '\n';
      msgPackDoc.toYAML(outStream);
      outStream << '\t' << AMDGPU::PALMD::AssemblerDirectiveEnd << '\n';
      m_streamer->emitRawText(outString);
      return totalSize;
    }
  }

  // Default handling of a .note record.
  m_streamer->AddComment(Twine(".note name ") + name + " type " + Twine(type));
  m_streamer->emitBinaryData(data.slice(0, totalSize));
  return totalSize;
}
