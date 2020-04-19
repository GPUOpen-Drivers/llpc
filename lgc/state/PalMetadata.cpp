/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PalMetadata.cpp
 * @brief LLPC source file: PalMetadata class for manipulating PAL metadata
 *
 * The PalMetadata object can be retrieved using PipelineState::getPalMetadata(), and is used by various parts
 * of LGC to write information to PAL metadata at the time the information is generated. The PalMetadata object
 * is carried through the middle-end, and serialized to IR metadata at the end of the middle-end (or at the
 * point -stop-before etc stops compilation, if earlier).
 ***********************************************************************************************************************
 */
#include "lgc/state/PalMetadata.h"
#include "lgc/state/AbiMetadata.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "lgc-pal-metadata"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Construct empty object
PalMetadata::PalMetadata() {
  m_document = new msgpack::Document;
}

// =====================================================================================================================
// Constructor given pipeline IR module. This reads the already-existing PAL metadata if any.
//
// @param module : Pipeline IR module
PalMetadata::PalMetadata(Module *module) {
  m_document = new msgpack::Document;
  NamedMDNode *namedMd = module->getNamedMetadata(PalMetadataName);
  if (namedMd && namedMd->getNumOperands()) {
    // The IR named metadata node contains an MDTuple containing an MDString containing the msgpack data.
    auto mdTuple = dyn_cast<MDTuple>(namedMd->getOperand(0));
    if (mdTuple && mdTuple->getNumOperands()) {
      if (auto mdString = dyn_cast<MDString>(mdTuple->getOperand(0))) {
        bool success = m_document->readFromBlob(mdString->getString(), /*multi=*/false);
        assert(success && "Bad PAL metadata format");
        ((void)success);
      }
    }
  }
}

// =====================================================================================================================
// Destructor
PalMetadata::~PalMetadata() {
  delete m_document;
}

// =====================================================================================================================
// Record the PAL metadata into IR metadata in the specified module. This is used both for passing the PAL metadata
// to the AMDGPU back-end, and when compilation stops early due to -stop-before etc.
//
// @param module : Pipeline IR module
void PalMetadata::record(Module *module) {
  // Add the metadata version number.
  auto versionNode = m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Version].getArray(true);
  versionNode[0] = m_document->getNode(Util::Abi::PipelineMetadataMajorVersion);
  versionNode[1] = m_document->getNode(Util::Abi::PipelineMetadataMinorVersion);

  // Write the MsgPack document into an IR metadata node.
  // The IR named metadata node contains an MDTuple containing an MDString containing the msgpack data.
  std::string blob;
  m_document->writeToBlob(blob);
  MDString *abiMetaString = MDString::get(module->getContext(), blob);
  MDNode *abiMetaNode = MDNode::get(module->getContext(), abiMetaString);
  NamedMDNode *namedMeta = module->getOrInsertNamedMetadata(PalMetadataName);
  namedMeta->addOperand(abiMetaNode);
}
