/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  lgc.cpp
 * @brief LLPC source file: contains implementation of LGC standalone tool.
 ***********************************************************************************************************************
 */

#include "lgc/ElfLinker.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/PassManager.h"
#include "lgc/Pipeline.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm-dialects/Dialect/Dialect.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/Verifier.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 442438
// Old version of the code
#include "llvm/IR/IRPrintingPasses.h"
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/IRPrinter/IRPrintingPasses.h"
#endif
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Target/TargetMachine.h"

using namespace lgc;
using namespace llvm;

namespace {
// Category for lgc options that are shown in "-help".
cl::OptionCategory LgcCategory("lgc");

// Input sources
cl::list<std::string> InFiles(cl::Positional, cl::ZeroOrMore, cl::cat(LgcCategory),
                              cl::desc("Input file(s) (\"-\" for stdin)"));

// -extract: extract a single module from a multi-module input file
cl::opt<unsigned> Extract("extract", cl::desc("Extract single module from multi-module input file. Index is 1-based"),
                          cl::init(0), cl::cat(LgcCategory), cl::value_desc("index"));

// -glue: compile a single glue shader instead of doing a link
cl::opt<unsigned> Glue("glue", cl::desc("Compile a single glue shader instead of doing a link. Index is 1-based"),
                       cl::init(0), cl::cat(LgcCategory), cl::value_desc("index"));

// -l: link
cl::opt<bool> Link("l", cl::cat(LgcCategory),
                   cl::desc("Link shader/part-pipeline ELFs. First input filename is "
                            "IR providing pipeline state; subsequent ones are ELF files."));

// -passes: pass pipeline
cl::opt<std::string> Passes("passes", cl::cat(LgcCategory), cl::value_desc("passes"),
                            cl::desc("Run the given pass pipeline, described using the same syntax as for LLVM's opt"
                                     "tool"));

// -o: output filename
cl::opt<std::string> OutFileName("o", cl::cat(LgcCategory), cl::desc("Output filename ('-' for stdout)"),
                                 cl::value_desc("filename"));

// -other: filename of other part-pipeline ELF
cl::opt<std::string> OtherName("other", cl::cat(LgcCategory),
                               cl::desc("Name of 'other' FS part-pipeline ELF when compiling non-FS part-pipeline"),
                               cl::value_desc("filename"));

// -pal-abi-version: PAL pipeline ABI version to compile for (default is latest known)
cl::opt<unsigned> PalAbiVersion("pal-abi-version", cl::init(0xFFFFFFFF), cl::cat(LgcCategory),
                                cl::desc("PAL pipeline version to compile for (default latest known)"),
                                cl::value_desc("version"));

// -v: enable verbose output
cl::opt<bool> VerboseOutput("v", cl::cat(LgcCategory), cl::desc("Enable verbose output"), cl::init(false));

} // anonymous namespace

// =====================================================================================================================
// Checks whether the input data is actually a ELF binary
//
// @param data : Input data to check
static bool isElfBinary(StringRef data) {
  bool isElfBin = false;
  if (data.size() >= sizeof(ELF::Elf64_Ehdr)) {
    auto header = reinterpret_cast<const ELF::Elf64_Ehdr *>(data.data());
    isElfBin = header->checkMagic();
  }
  return isElfBin;
}

// =====================================================================================================================
// Checks whether the output data is actually ISA assembler text
//
// @param data : Input data to check
static bool isIsaText(StringRef data) {
  // This is called by the lgc standalone tool to help distinguish between its three output types of ELF binary,
  // LLVM IR assembler and ISA assembler. Here we use the fact that ISA assembler is the only one that starts
  // with a tab character.
  return data.startswith("\t");
}

// =====================================================================================================================
// Run the pass pipeline given by the -passes command-line option and output the final IR to outStream.
//
// @param pipeline : The LGC pipeline for the module
// @param module : The module
// @param outStream : The output stream for the final IR
static bool runPassPipeline(Pipeline &pipeline, Module &module, raw_pwrite_stream &outStream) {
  // Set up "whole pipeline" passes, where we have a single module representing the whole pipeline.
  LgcContext *lgcContext = pipeline.getLgcContext();
  std::unique_ptr<lgc::PassManager> passMgr(lgc::PassManager::Create(lgcContext));
  passMgr->registerFunctionAnalysis([&] { return lgcContext->getTargetMachine()->getTargetIRAnalysis(); });
  passMgr->registerModuleAnalysis([&] { return PipelineShaders(); });
  passMgr->registerModuleAnalysis([&] { return PipelineStateWrapper(static_cast<PipelineState *>(&pipeline)); });
  Patch::registerPasses(*passMgr);

  // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
  lgcContext->preparePassManager(*passMgr);

  PassBuilder passBuilder(lgcContext->getTargetMachine(), PipelineTuningOptions(), {},
                          &passMgr->getInstrumentationCallbacks());
  Patch::registerPasses(passBuilder);

  if (auto err = passBuilder.parsePassPipeline(*passMgr, Passes)) {
    errs() << "Failed to parse -passes: " << toString(std::move(err)) << '\n';
    return false;
  }

  // This mode of the tool is only ever used for development and testing, so unconditionally run the verifier on the
  // final output.
  passMgr->addPass(VerifierPass());

  switch (codegen::getFileType()) {
  case CGFT_AssemblyFile:
    passMgr->addPass(PrintModulePass(outStream));
    break;
  case CGFT_ObjectFile:
    passMgr->addPass(BitcodeWriterPass(outStream));
    break;
  case CGFT_Null:
    break;
  }

  passMgr->run(module);
  return true;
}

// =====================================================================================================================
// Main code of LGC standalone tool
//
// @param argc : Count of command-line arguments
// @param argv : Command-line arguments
int main(int argc, char **argv) {
  const char *progName = sys::path::filename(argv[0]).data();
  LgcContext::initialize();

  LLVMContext context;
  auto dialectContext = llvm_dialects::DialectContext::make<LgcDialect>(context);

  // Set our category on options that we want to show in -help, and hide other options.
  auto opts = cl::getRegisteredOptions();
  opts["mcpu"]->addCategory(LgcCategory);
  opts["filetype"]->addCategory(LgcCategory);
  opts["emit-llvm"]->addCategory(LgcCategory);
  opts["verify-ir"]->addCategory(LgcCategory);
  cl::HideUnrelatedOptions(LgcCategory);

  // Parse command line.
  static const char *commandDesc = "lgc: command-line tool for LGC, the LLPC middle-end compiler\n"
                                   "\n"
                                   "The lgc tool parses one or more modules of LLVM IR assembler from the input\n"
                                   "file(s) and compiles each one using the LGC interface, into AMDGPU ELF or\n"
                                   "assembly. Generally, each input module would have been derived by compiling\n"
                                   "a shader or pipeline with amdllpc, and using the -emit-lgc option to stop\n"
                                   "before running LGC.\n"
                                   "\n"
                                   "If the -l (link) option is given, then the lgc tool instead parses a single\n"
                                   "module of LLVM IR assembler from the first input file, and uses the IR metadata\n"
                                   "from that to set LGC pipeline state. Then it reads the remaining input files,\n"
                                   "all compiled ELF files, and performs an LGC pipeline link.\n"
                                   "\n"
                                   "If the -glue option is given in addition to the -l (link) option, then input\n"
                                   "files are the same as in a link operation, but lgc instead compiles the glue\n"
                                   "shader of the given one-based index that would be used in the link.\n"
                                   "\n"
                                   "If the -passes option is given, modules are instead run through a pass pipeline\n"
                                   "as defined by the -passes argument, which uses the same syntax as LLVM's opt\n"
                                   "tool, and the resulting IR is output as assembly or bitcode. Passes from both\n"
                                   " LLVM and LGC can be used.\n";
  cl::ParseCommandLineOptions(argc, argv, commandDesc);

  // Find the -mcpu option and get its value.
  auto mcpu = opts.find("mcpu");
  assert(mcpu != opts.end());
  auto *mcpuOpt = reinterpret_cast<cl::opt<std::string> *>(mcpu->second);
  StringRef gpuName = *mcpuOpt;
  if (gpuName == "")
    gpuName = "gfx802";

  // Default to reading from stdin and writing to stdout
  if (InFiles.empty())
    InFiles.push_back("-");

  if (OutFileName.empty() && InFiles[0] == "-")
    OutFileName = "-";

  // If we will be outputting to stdout, default to -filetype=asm
  if (OutFileName == "-") {
    auto optIterator = cl::getRegisteredOptions().find("filetype");
    assert(optIterator != cl::getRegisteredOptions().end());
    cl::Option *opt = optIterator->second;
    if (opt->getNumOccurrences() == 0)
      *static_cast<cl::opt<CodeGenFileType> *>(opt) = CGFT_AssemblyFile;
  }

  // Create the LgcContext.
  std::unique_ptr<TargetMachine> targetMachine(LgcContext::createTargetMachine(gpuName, CodeGenOpt::Level::Default));
  if (!targetMachine) {
    errs() << progName << ": GPU type '" << gpuName << "' not recognized\n";
    return 1;
  }
  std::unique_ptr<LgcContext> lgcContext(LgcContext::create(&*targetMachine, context, PalAbiVersion));

  if (VerboseOutput)
    lgcContext->setLlpcOuts(&outs());

  // Read the "other" part-pipeline ELF input.
  std::unique_ptr<MemoryBuffer> otherBuffer;
  if (!OtherName.empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr = MemoryBuffer::getFileOrSTDIN(OtherName);
    if (std::error_code errorCode = fileOrErr.getError()) {
      auto error = SMDiagnostic(OtherName, SourceMgr::DK_Error, "Could not open input file: " + errorCode.message());
      error.print(progName, errs());
      errs() << "\n";
      return 1;
    }
    otherBuffer = std::move(*fileOrErr);
  }

  // Read the input files.
  SmallVector<std::unique_ptr<MemoryBuffer>, 4> inBuffers;
  for (const auto &inFileName : InFiles) {
    // Read the input file. getFileOrSTDIN handles the case of inFileName being "-".
    ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr = MemoryBuffer::getFileOrSTDIN(inFileName);
    if (std::error_code errorCode = fileOrErr.getError()) {
      auto error = SMDiagnostic(inFileName, SourceMgr::DK_Error, "Could not open input file: " + errorCode.message());
      error.print(progName, errs());
      errs() << "\n";
      return 1;
    }
    inBuffers.push_back(std::move(*fileOrErr));
  }

  // Process each input file.
  for (auto &inBuffer : inBuffers) {
    MemoryBufferRef bufferRef = inBuffer->getMemBufferRef();
    StringRef bufferName = bufferRef.getBufferIdentifier();

    // Split the input into multiple LLVM IR modules. We assume that a new module starts with
    // a "target" line to set the datalayout or triple, or a "define" line for a new function,
    // but not until after we have seen at least one line starting with '!' (metadata declaration)
    // in the previous module.
    SmallVector<StringRef, 4> separatedAsms;
    StringRef remaining = bufferRef.getBuffer();
    separatedAsms.push_back(remaining);
    bool hadMetadata = false;
    for (;;) {
      auto notSpacePos = remaining.find_first_not_of(" \t\n");
      if (notSpacePos != StringRef::npos) {
        if (remaining[notSpacePos] == '!')
          hadMetadata = true;
        else if (hadMetadata && (remaining.drop_front(notSpacePos).startswith("target") ||
                                 remaining.drop_front(notSpacePos).startswith("define"))) {
          // End the current split module and go on to the next one.
          separatedAsms.back() = separatedAsms.back().slice(0, remaining.data() - separatedAsms.back().data());
          separatedAsms.push_back(remaining);
          hadMetadata = false;
        }
      }
      auto nlPos = remaining.find_first_of('\n');
      if (nlPos == StringRef::npos)
        break;
      remaining = remaining.slice(nlPos + 1, StringRef::npos);
    }

    // Check that the -extract option is not out of range.
    if (Extract > separatedAsms.size()) {
      errs() << progName << ": " << bufferName << ": Not enough modules for -extract value\n";
      exit(1);
    }

    // Process each module. Put extra newlines at the start of each one other than the first so that
    // line numbers are correct for error reporting.
    unsigned extraNlCount = 0;
    for (unsigned idx = 0; idx != separatedAsms.size(); ++idx) {
      StringRef separatedAsm = separatedAsms[idx];
      std::string asmText;
      asmText.insert(asmText.end(), extraNlCount, '\n');
      extraNlCount += separatedAsm.count('\n');
      asmText += separatedAsm;

      // Skip this module if -extract was specified for a different index.
      if (Extract && Extract != idx + 1)
        continue;

      // Use a MemoryBufferRef with the original filename so error reporting reports it.
      MemoryBufferRef asmBuffer(asmText, bufferName);

      // Assemble the text
      SMDiagnostic error;
      std::unique_ptr<Module> module = parseAssembly(asmBuffer, error, context);
      if (!module) {
        error.print(progName, errs());
        errs() << "\n";
        return 1;
      }

      // Verify the resulting IR.
      if (verifyModule(*module, &errs())) {
        errs() << progName << ": " << bufferName << ": IR verification errors in module " << idx << "\n";
        return 1;
      }

      // Set the triple and data layout, so you can write tests without bothering to specify them.
      TargetMachine *targetMachine = lgcContext->getTargetMachine();
      module->setTargetTriple(targetMachine->getTargetTriple().getTriple());
      module->setDataLayout(targetMachine->createDataLayout());

      // Determine whether we are outputting to a file.
      bool outputToFile = OutFileName != "-";
      if (OutFileName.empty()) {
        // No -o specified: output to stdout if input is -
        outputToFile = bufferName != "-" && bufferName != "<stdin>";
      }

      SmallString<64> outFileName;
      if (OutFileName.empty()) {
        // Start to determine the output filename by taking the input filename, removing the directory,
        // removing the extension. We add the extension below once we can see what the output contents
        // look like.
        outFileName = sys::path::stem(bufferName);
      }

      SmallString<16> outBuffer;
      raw_svector_ostream outStream(outBuffer);
      std::unique_ptr<Pipeline> pipeline(lgcContext->createPipeline());
      StringRef err;

      // If there is an "other" part-pipeline ELF, give its metadata to our compile's pipeline by setting
      // up a pipeline and linker for it that we are not otherwise going to use.
      if (otherBuffer) {
        std::unique_ptr<Pipeline> otherPipeline(lgcContext->createPipeline());
        std::unique_ptr<ElfLinker> linker(otherPipeline->createElfLinker(otherBuffer->getMemBufferRef()));
        pipeline->setOtherPartPipeline(*otherPipeline, &*module);
      }

      if (Link) {
        // The -l option (link) is handled differently: We have just read the first input file as IR, and
        // we get the pipeline state from that. Subsequent input files are ELF, and we link them.
        pipeline->setStateFromModule(&*module);

        SmallVector<MemoryBufferRef, 4> elfRefs;
        for (unsigned i = 1; i != inBuffers.size(); ++i)
          elfRefs.push_back(inBuffers[i]->getMemBufferRef());
        std::unique_ptr<ElfLinker> elfLinker(pipeline->createElfLinker(elfRefs));

        if (Glue) {
          // Instead of doing a full link, we have been asked to compile a glue shader used in a link.
          ArrayRef<StringRef> glueInfo = elfLinker->getGlueInfo();
          if (Glue > glueInfo.size())
            report_fatal_error("Only " + Twine(glueInfo.size()) + " glue shader(s) in this link");
          outStream << elfLinker->compileGlue(Glue - 1);
          if (outStream.str().empty())
            err = pipeline->getLastError();
        } else {
          // Do a full link.
          if (!elfLinker->link(outStream))
            err = pipeline->getLastError();
        }
      } else if (Passes.getNumOccurrences() > 0) {
        // Run a pass pipeline.
        pipeline->setStateFromModule(&*module);

        if (!runPassPipeline(*pipeline, *module, outStream))
          return 1;
      } else {
        // Run the middle-end compiler.
        if (!pipeline->generate(std::move(module), outStream, nullptr, {}))
          err = pipeline->getLastError();
      }

      if (err != "") {
        // Link or compile reported recoverable error.
        errs() << err << "\n";
        return 1;
      }

      if (!outputToFile) {
        // Output to stdout.
        outs() << outBuffer;
      } else {
        // Output to file.
        if (outFileName.empty()) {
          // Use given filename.
          outFileName = OutFileName;
        } else {
          // We are in the middle of deriving the output filename from the input filename. Add the
          // extension now.
          const char *ext = ".s";
          if (isElfBinary(outBuffer)) {
            ext = ".elf";
          } else if (isIsaText(outBuffer)) {
            ext = ".s";
          } else {
            ext = ".ll";
          }
          outFileName += ext;
        }

        bool fileWriteSuccess = false;
        if (FILE *outFile = fopen(outFileName.c_str(), "wb")) {
          if (fwrite(outBuffer.data(), 1, outBuffer.size(), outFile) == outBuffer.size())
            fileWriteSuccess = fclose(outFile) == 0;
        }
        if (!fileWriteSuccess) {
          errs() << progName << ": " << outFileName << ": " << strerror(errno) << "\n";
          return 1;
        }
      }

      // With the -l option (link), we have already consumed all the input files.
      if (Link)
        return 0;
    }
  }

  return 0;
}
