/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/Support/StringSaver.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 401324
// Old version
#include "llvm/Support/TargetRegistry.h"
#else
// New version (and unknown version)
#include "llvm/MC/TargetRegistry.h"
#endif
#include "llvm/Support/TargetSelect.h"

using namespace llvm;
using namespace object;

namespace lgc {
// Get name of PAL metadata register, or "" if not known
const char *getPalMetadataRegName(unsigned regNumber);
} // namespace lgc

namespace {

// Represents an operand of a disassembler instruction.
struct InstOp {
  Optional<int64_t> imm;
  Optional<unsigned> sReg;
};

// Represents a disassembled instruction or directive.
struct InstOrDirective {
  MCDisassembler::DecodeStatus status = MCDisassembler::Fail;
  uint64_t offset;
  ArrayRef<uint8_t> bytes;
  MCInst mcInst;

  StringRef mnemonic;
  StringRef comment;
  InstOp op0;
  InstOp op1;
  InstOp op2;

  const MCExpr *valueDirectiveExpr = nullptr;

  uint64_t getEndOffset() const { return offset + bytes.size(); }
};

// Stores symbols.
struct SymbolPool {
  SectionSymbolsTy symbols;

  // Translates (offset, symbol type) pairs to symbols.
  DenseMap<std::pair<uint64_t, unsigned>, MCSymbol *> symbolMap;
};

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
  MCInstPrinter *m_instPrinter = nullptr;
  MCContext *m_context = nullptr;
  std::vector<object::RelocationRef> relocs;
  BumpPtrAllocator m_stringsAlloc;
  StringSaver m_strings{m_stringsAlloc};

public:
  static void disassembleObject(MemoryBufferRef data, raw_ostream &ostream) {
    ObjDisassembler objDis(data, ostream);
    objDis.run();
  }

private:
  ObjDisassembler(MemoryBufferRef data, raw_ostream &ostream) : m_data(data), m_ostream(ostream) {}

  void run();
  void processSection(ELFSectionRef sectionRef);
  void gatherSectionSymbols(ELFSectionRef sectionRef, SymbolPool &symbols);
  void gatherRelocs(ELFSectionRef sectionRef, std::vector<object::RelocationRef> &relocs);
  void tryDisassembleSection(ELFSectionRef sectionRef, unsigned sectType, unsigned sectFlags, bool outputting,
                             SymbolPool &symbols, ArrayRef<object::RelocationRef> relocs);
  bool disasmInstSeq(SmallVectorImpl<InstOrDirective> &seq, uint64_t offset, bool outputting, StringRef contents,
                     SymbolPool &symbols);
  bool disasmLongJump(SmallVectorImpl<InstOrDirective> &seq, const InstOrDirective &inst, bool outputting,
                      StringRef contents, SymbolPool &symbols);
  bool disasmTableJump(SmallVectorImpl<InstOrDirective> &seq, const InstOrDirective &inst, bool outputting,
                       StringRef contents, SymbolPool &symbols);
  InstOrDirective disasmInst(uint64_t offset, StringRef contents);
  void addBinaryEncodingComment(raw_ostream &stream, unsigned instAlignment, ArrayRef<uint8_t> instBytes);
  void outputInst(InstOrDirective inst, unsigned instAlignment);
  void outputData(bool outputting, uint64_t offset, StringRef data, ArrayRef<object::RelocationRef> &relocs);
  void outputRelocs(bool outputting, uint64_t offset, uint64_t size, ArrayRef<object::RelocationRef> &relocs);
  size_t decodeNote(StringRef data);
  MCSymbol *getOrCreateSymbol(SymbolPool &symbols, uint64_t offset, Twine name = {}, unsigned type = ELF::STT_NOTYPE);

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

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 389223
  MCObjectFileInfo objFileInfo;
  MCContext context(triple, asmInfo.get(), regInfo.get(), &objFileInfo, m_subtargetInfo.get());
  objFileInfo.initMCObjectFileInfo(context, false);
#else
  MCContext context(triple, asmInfo.get(), regInfo.get(), m_subtargetInfo.get());
  std::unique_ptr<MCObjectFileInfo> objFileInfo(m_target->createMCObjectFileInfo(context, /*PIC=*/false));
  if (!objFileInfo)
    report_fatal_error("No MC object file info");
  context.setObjectFileInfo(objFileInfo.get());
#endif
  m_context = &context;

  m_instDisassembler.reset(m_target->createMCDisassembler(*m_subtargetInfo, *m_context));
  if (!m_instDisassembler)
    report_fatal_error(m_data.getBufferIdentifier() + ": No disassembler for target");
  m_instPrinter = m_target->createMCInstPrinter(triple, asmInfo->getAssemblerDialect(), *asmInfo, *instrInfo, *regInfo);
  if (!m_instPrinter)
    report_fatal_error(m_data.getBufferIdentifier() + ": No instruction printer for target");

  auto fostream = std::make_unique<formatted_raw_ostream>(m_ostream);
  m_streamer.reset(m_target->createAsmStreamer(*m_context, std::move(fostream), true, false, m_instPrinter, nullptr,
                                               nullptr, false));

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
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 425813
  // Old version of code
  m_streamer->AddBlankLine();
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  m_streamer->addBlankLine();
#endif
  unsigned sectFlags = sectionRef.getFlags();
  MCSection *sect = m_context->getELFSection(cantFail(sectionRef.getName()), sectType, sectFlags);
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 425813
  // Old version of code
  m_streamer->SwitchSection(sect);
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  m_streamer->switchSection(sect);
#endif

  // Create all symbols in this section. Also emit directives for symbol type and size,
  // adding a synthesized label for the end of the symbol.
  SymbolPool symbols;
  gatherSectionSymbols(sectionRef, symbols);

  // Collect and sort the relocs for the section.
  std::vector<object::RelocationRef> relocs;
  gatherRelocs(sectionRef, relocs);

  // Disassemble the section multiple times until no new synthesized labels appear, then disassemble
  // one final time actually streaming the output. For non-code, just use a single outputting pass.
  // The loop terminates after that one final time, at the "Done final outputting pass" comment below.
  bool outputting = !(sectFlags & ELF::SHF_EXECINSTR);
  for (;;) {
    // One iteration of disassembling the section.

    // If AMDGPU, create a symbolizer, giving it the symbols.
    MCSymbolizer *symbolizerPtr = nullptr;
    if (m_objFile->getArch() == Triple::amdgcn) {
      std::unique_ptr<MCRelocationInfo> relInfo(m_target->createMCRelocationInfo(m_tripleName, *m_context));
      if (relInfo) {
        std::unique_ptr<MCSymbolizer> symbolizer(
            m_target->createMCSymbolizer(m_tripleName, /* GetOpInfo= */ nullptr, /* SymbolLookUp= */ nullptr,
                                         &symbols.symbols, m_context, std::move(relInfo)));
        symbolizerPtr = &*symbolizer;
        m_instDisassembler->setSymbolizer(std::move(symbolizer));
      }
    }

    // Disassemble the section contents.
    size_t prevNumSymbols = symbols.symbols.size();
    stable_sort(symbols.symbols); // Stable sort as there may be duplicate addresses.
    tryDisassembleSection(sectionRef, sectType, sectFlags, outputting, symbols, relocs);
    if (outputting)
      break; // Done final outputting pass.

    if (symbolizerPtr) {
      for (uint64_t offset : symbolizerPtr->getReferencedAddresses())
        getOrCreateSymbol(symbols, offset);
    }

    // If there were no new symbols, then we can do the final output in
    // the next pass.
    outputting = (symbols.symbols.size() == prevNumSymbols);
  }
}

// =====================================================================================================================
// Create all symbols in the given section. Also emit directives for symbol type and size.
// The size is an expression endSym-sym where endSym is a synthesized label at the end of the function.
//
// @param sectionRef : The section being disassembled
// @param [out] symbols : Symbols to populate
void ObjDisassembler::gatherSectionSymbols(ELFSectionRef sectionRef, SymbolPool &symbols) {
  for (ELFSymbolRef symbolRef : m_objFile->symbols()) {
    if (cantFail(symbolRef.getSection()) != sectionRef)
      continue;

    uint64_t offset = cantFail(symbolRef.getValue());
    StringRef name = cantFail(symbolRef.getName());
    unsigned type = symbolRef.getELFType();
    MCSymbol *sym = getOrCreateSymbol(symbols, offset, name, type);

    switch (type) {
    case ELF::STT_FUNC:
      m_streamer->emitSymbolAttribute(sym, MCSA_ELF_TypeFunction);
      break;
    case ELF::STT_OBJECT:
      m_streamer->emitSymbolAttribute(sym, MCSA_ELF_TypeObject);
      break;
    }

    if (uint64_t size = symbolRef.getSize()) {
      uint64_t endOffset = offset + size;
      if (endOffset <= cantFail(sectionRef.getContents()).size()) {
        MCSymbol *endSym = getOrCreateSymbol(symbols, endOffset, Twine(name) + "_symend");
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
                                            bool outputting, SymbolPool &symbols,
                                            ArrayRef<object::RelocationRef> relocs) {

  bool isCode = sectFlags & ELF::SHF_EXECINSTR;
  bool isNote = sectType == ELF::SHT_NOTE;
  unsigned instAlignment = 1;
  if (isCode)
    instAlignment = m_context->getAsmInfo()->getMinInstAlignment();

  // Get the section contents, and disassemble until nothing left.
  StringRef contents = cantFail(sectionRef.getContents());
  size_t offset = 0, lastOffset = 0;
  size_t nextSymbol = 0;

  // The current sequence of instructions, if any.
  // In the table-jump sequence, currently seen as the longest one, there
  // are 8 instructions followed by likely more than 8 target offset
  // entries, which suggests 32 be the suitable power of two for the size.
  SmallVector<InstOrDirective, 32> instSeq;

  for (;;) {
    size_t endOffset = contents.size();
    if (nextSymbol != symbols.symbols.size() && symbols.symbols[nextSymbol].Addr < endOffset)
      endOffset = symbols.symbols[nextSymbol].Addr;

    if (offset == endOffset) {
      // We're about to emit a symbol or finish the section.
      // If there is any remaining non-disassemblable data, output it.
      if (lastOffset != offset) {
        outputData(outputting, lastOffset, contents.slice(lastOffset, offset), relocs);
        lastOffset = offset;
      }

      if (nextSymbol != symbols.symbols.size() && symbols.symbols[nextSymbol].Addr == offset) {
        // Output a symbol or label here.
        if (outputting) {
          MCSymbol *sym = m_context->getOrCreateSymbol(symbols.symbols[nextSymbol].Name);
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

    // Skip instructions that are at already disassembled offsets.
    while (!instSeq.empty() && instSeq.front().offset < offset)
      instSeq.erase(instSeq.begin());

    // Try disassembling an instruction.
    if (!isCode || (offset & instAlignment - 1) != 0 ||
        (instSeq.empty() && !disasmInstSeq(instSeq, offset, outputting, contents, symbols))) {
      // No disassemblable instruction here. Try the next instruction unit.
      offset = std::min(size_t(alignTo(offset + 1, instAlignment)), endOffset);
      continue;
    }

    InstOrDirective inst = instSeq[0];
    instSeq.erase(instSeq.begin());

    // Got a disassemblable instruction.
    // First output any non-disassemblable data up to this point.
    if (lastOffset != offset)
      outputData(outputting, lastOffset, contents.slice(lastOffset, offset), relocs);

    // Output reloc.
    outputRelocs(outputting, offset, inst.bytes.size(), relocs);

    if (outputting)
      outputInst(inst, instAlignment);

    offset += inst.bytes.size();
    lastOffset = offset;
  }
}

// =====================================================================================================================
// Try disassembling an instruction sequence.
//
// @param [out] seq : The sequence of instructions or directives
// @param offset : The offset at which the sequence begins
// @param outputting : True to actually stream the disassembly output
// @param contents : The bytes of the section
// @param symbols : The symbols of the section
// @returns : Return true on success.
bool ObjDisassembler::disasmInstSeq(SmallVectorImpl<InstOrDirective> &seq, uint64_t offset, bool outputting,
                                    StringRef contents, SymbolPool &symbols) {
  assert(seq.empty() && "Asked for a new instruction sequence while "
                        "still having the previous one!");
  InstOrDirective inst = disasmInst(offset, contents);
  if (inst.status == MCDisassembler::Fail)
    return false;

  if (disasmLongJump(seq, inst, outputting, contents, symbols) ||
      disasmTableJump(seq, inst, outputting, contents, symbols))
    return true;

  seq.emplace_back(inst);
  return true;
}

// =====================================================================================================================
// Try disassembling a long jump sequence.
//
// @param [out] seq : The sequence of instructions or directives
// @param inst : The first instruction of the potential sequence
// @param outputting : True to actually stream the disassembly output
// @param contents : The bytes of the section
// @param symbols : The symbols of the section
// @returns : Return true on success.
bool ObjDisassembler::disasmLongJump(SmallVectorImpl<InstOrDirective> &seq, const InstOrDirective &inst,
                                     bool outputting, StringRef contents, SymbolPool &symbols) {
  InstOrDirective getpc = inst;
  if (getpc.mnemonic != "s_getpc_b64")
    return false;

  InstOrDirective add = disasmInst(getpc.getEndOffset(), contents);
  if (add.mnemonic != "s_add_u32" || *add.op0.sReg != *getpc.op0.sReg || *add.op1.sReg != *getpc.op0.sReg ||
      !add.op2.imm)
    return false;

  InstOrDirective addc = disasmInst(add.getEndOffset(), contents);
  if (addc.mnemonic != "s_addc_u32" || *addc.op0.sReg != *getpc.op0.sReg + 1 || *addc.op1.sReg != *getpc.op0.sReg + 1 ||
      !addc.op2.imm || *addc.op2.imm != 0)
    return false;

  InstOrDirective setpc = disasmInst(addc.getEndOffset(), contents);
  if (setpc.mnemonic != "s_setpc_b64" || *setpc.op0.sReg != *getpc.op0.sReg)
    return false;

  MCSymbol *getpcLabel = getOrCreateSymbol(symbols, getpc.getEndOffset());
  MCSymbol *targetLabel = getOrCreateSymbol(symbols, getpc.getEndOffset() + *add.op2.imm);
  if (outputting) {
    const MCExpr *targetOffsetExpr = MCBinaryExpr::createSub(
        MCSymbolRefExpr::create(targetLabel, *m_context), MCSymbolRefExpr::create(getpcLabel, *m_context), *m_context);
    add.mcInst.getOperand(2) = MCOperand::createExpr(targetOffsetExpr);
  }

  for (const InstOrDirective &i : {getpc, add, addc, setpc})
    seq.emplace_back(i);

  return true;
}

// =====================================================================================================================
// Try disassembling a table-jump sequence.
//
// @param [out] seq : The sequence of instructions or directives
// @param inst : The first instruction of the potential sequence
// @param outputting : True to actually stream the disassembly output
// @param contents : The bytes of the section
// @param symbols : The symbols of the section
// @returns : Return true on success.
bool ObjDisassembler::disasmTableJump(SmallVectorImpl<InstOrDirective> &seq, const InstOrDirective &inst,
                                      bool outputting, StringRef contents, SymbolPool &symbols) {
  InstOrDirective min = inst;
  if (min.mnemonic != "s_min_u32" || !min.op2.imm)
    return false;

  InstOrDirective getpc = disasmInst(inst.getEndOffset(), contents);
  if (getpc.mnemonic != "s_getpc_b64")
    return false;

  InstOrDirective lshl3Add = disasmInst(getpc.getEndOffset(), contents);
  if (lshl3Add.mnemonic != "s_lshl3_add_u32" || !lshl3Add.op1.sReg || *lshl3Add.op1.sReg != *min.op0.sReg ||
      !lshl3Add.op2.imm) {
    return false;
  }

  InstOrDirective load = disasmInst(lshl3Add.getEndOffset(), contents);
  if (load.mnemonic != "s_load_dwordx2" || *load.op1.sReg != *getpc.op0.sReg || !load.op2.sReg ||
      *load.op2.sReg != *lshl3Add.op0.sReg)
    return false;

  InstOrDirective waitcnt = disasmInst(load.getEndOffset(), contents);
  if (waitcnt.mnemonic != "s_waitcnt")
    return false;

  InstOrDirective add = disasmInst(waitcnt.getEndOffset(), contents);
  if (add.mnemonic != "s_add_u32" || *add.op1.sReg != *load.op0.sReg || *add.op2.sReg != *getpc.op0.sReg)
    return false;

  InstOrDirective addc = disasmInst(add.getEndOffset(), contents);
  if (addc.mnemonic != "s_addc_u32" || *addc.op0.sReg != *add.op0.sReg + 1 || *addc.op1.sReg != *load.op0.sReg + 1 ||
      *addc.op2.sReg != *getpc.op0.sReg + 1)
    return false;

  InstOrDirective setpc = disasmInst(addc.getEndOffset(), contents);
  if (setpc.mnemonic != "s_setpc_b64" || *setpc.op0.sReg != *add.op0.sReg)
    return false;

  MCSymbol *getpcLabel = getOrCreateSymbol(symbols, getpc.getEndOffset());
  MCSymbol *tableLabel = getOrCreateSymbol(symbols, getpc.getEndOffset() + *lshl3Add.op2.imm);
  if (outputting) {
    const MCExpr *tableSize = MCBinaryExpr::createSub(MCSymbolRefExpr::create(tableLabel, *m_context),
                                                      MCSymbolRefExpr::create(getpcLabel, *m_context), *m_context);
    lshl3Add.mcInst.getOperand(2) = MCOperand::createExpr(tableSize);
  }

  for (const InstOrDirective &inst : {min, getpc, lshl3Add, load, waitcnt, add, addc, setpc})
    seq.emplace_back(inst);

  unsigned numEntries = *min.op2.imm + 1;
  for (unsigned i = 0; i != numEntries; ++i) {
    InstOrDirective quad;
    quad.offset = getpc.getEndOffset() + *lshl3Add.op2.imm + i * 8;
    quad.bytes = {reinterpret_cast<const uint8_t *>(contents.data()) + quad.offset, 8};
    uint64_t targetOffset = getpc.getEndOffset() + support::endian::read64le(quad.bytes.data());
    const MCSymbol *targetLabel = getOrCreateSymbol(symbols, targetOffset);
    if (outputting) {
      quad.valueDirectiveExpr = MCBinaryExpr::createSub(MCSymbolRefExpr::create(targetLabel, *m_context),
                                                        MCSymbolRefExpr::create(getpcLabel, *m_context), *m_context);
    }
    seq.emplace_back(quad);
  }

  return true;
}

static InstOp parseInstOp(StringRef op) {
  InstOp res;
  int64_t imm;
  if (!op.getAsInteger(/* Radix= */ 0, imm)) {
    res.imm = imm;
    return res;
  }

  if (op.startswith("s")) {
    StringRef s = op.drop_front(1);
    s.consume_front("[");
    unsigned n;
    if (!s.consumeInteger(/* Radix= */ 10, n)) {
      res.sReg = n;
      return res;
    }
  }

  return res;
}

// =====================================================================================================================
// Disassembles instruction at a given offset.
//
// @param offset : The offset of the instruction.
// @param contents : The bytes of the section
// @returns : The instruction.
InstOrDirective ObjDisassembler::disasmInst(uint64_t offset, StringRef contents) {
  InstOrDirective inst;
  inst.offset = offset;
  uint64_t size = 0;
  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(contents.data()), contents.size());
  bytes = bytes.slice(offset);
  std::string comment;
  raw_string_ostream commentStream(comment);
  inst.status = m_instDisassembler->getInstruction(inst.mcInst, size, bytes, offset, commentStream);
  inst.bytes = bytes.take_front(size);

  if (inst.status == MCDisassembler::Fail)
    return inst;

  std::string instStr;
  raw_string_ostream os(instStr);
  m_instPrinter->printInst(&inst.mcInst, /* Address= */ 0, /* Annot= */ "", *m_subtargetInfo, os);

  StringRef s = instStr;
  StringRef mnemonic;
  std::tie(mnemonic, s) = s.ltrim().split(' ');
  inst.mnemonic = m_strings.save(mnemonic);

  inst.comment = m_strings.save(comment);

  StringRef op0, op1, op2;
  std::tie(op0, s) = s.ltrim().split(',');
  std::tie(op1, s) = s.ltrim().split(',');
  std::tie(op2, s) = s.ltrim().split(',');

  inst.op0 = parseInstOp(op0);
  inst.op1 = parseInstOp(op1);
  inst.op2 = parseInstOp(op2);
  return inst;
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
// Outputs a given instruction or directive.
//
// @param inst : The instruction or directive to output.
// @param instAlignment : Alignment (instruction unit size in bytes)
void ObjDisassembler::outputInst(InstOrDirective inst, unsigned instAlignment) {
  // Output the binary encoding as a comment.
  if (inst.status == MCDisassembler::SoftFail)
    m_streamer->AddComment("Illegal instruction encoding ");
  std::string comment = inst.comment.str();
  raw_string_ostream commentStream(comment);
  if (!comment.empty())
    commentStream << " ";
  commentStream << format("%06x:", inst.offset);
  addBinaryEncodingComment(commentStream, instAlignment, inst.bytes);
  // Output the instruction to the streamer.
  if (!comment.empty())
    m_streamer->AddComment(comment);
  if (const MCExpr *expr = inst.valueDirectiveExpr)
    m_streamer->emitValue(expr, inst.bytes.size());
  else
    m_streamer->emitInstruction(inst.mcInst, *m_subtargetInfo);
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
  // Check whether the data is mostly ASCII, possibly with a terminating 0.
  size_t asciiCount = 0;
  for (char ch : data) {
    if ((ch >= ' ' && ch <= '~') || ch == '\n' || ch == '\r' || ch == '\t')
      ++asciiCount;
  }
  bool isAscii = asciiCount * 10 >= data.size() * 9;

  while (!data.empty()) {
    if (!relocs.empty() && relocs[0].getOffset() == offset)
      outputRelocs(outputting, offset, 1, relocs);

    // Only go as far as the next reloc.
    size_t size = data.size();
    if (!relocs.empty())
      size = std::min(size, size_t(relocs[0].getOffset() - offset));

    // If outputting ascii, only go as far as just past the next bunch of consecutive newlines.
    if (isAscii) {
      size_t nl = data.find('\n');
      if (nl != StringRef::npos) {
        for (size = nl + 1; size != data.size() && data[size] == '\n'; ++size)
          ;
      }
    }

    if (outputting) {
      if (isAscii)
        m_streamer->emitBytes(data.take_front(size));
      else
        m_streamer->emitBinaryData(data.take_front(size));
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

// =====================================================================================================================
// Lookup the symbol of the specified offset and type. Create a new one if not exists.
//
// @param symbols : The symbols of the section
// @param offset : The offset in section the symbol refers to
// @param name : The name of the new symbol or empty twine for a default name
// @param type : The ELF type of the symbol
// @returns : Returns the existing or new symbol.
MCSymbol *ObjDisassembler::getOrCreateSymbol(SymbolPool &symbols, uint64_t offset, Twine name, unsigned type) {
  MCSymbol *&sym = symbols.symbolMap[std::make_pair(offset, type)];
  if (!sym) {
    StringRef savedName = m_strings.save(name.isTriviallyEmpty() ? "_L" + Twine::utohexstr(offset) : name);
    symbols.symbols.emplace_back(SymbolInfoTy(offset, savedName, type));
    sym = m_context->getOrCreateSymbol(savedName);
  }
  return sym;
}
