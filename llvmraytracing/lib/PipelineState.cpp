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
 * @file  PipelineState.cpp
 * @brief Implementation of helpers for llvmraytracing pipeline state.
 ***********************************************************************************************************************
 */

#include "llvmraytracing/PipelineState.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"

using namespace llvm;

namespace {
// Constants used in the msgpack format
namespace MsgPackFormat {

constexpr unsigned MajorVersion = 2;

static constexpr char Version[] = "version";
static constexpr char MaxUsedPayloadRegisterCount[] = "max_used_payload_register_count";
static constexpr char SpecializeDriverShadersState[] = "specialize_driver_shaders_state";

} // namespace MsgPackFormat
} // anonymous namespace

namespace llvmraytracing {

Expected<PipelineState> PipelineState::decodeMsgpack(llvm::msgpack::DocNode &Root) {
  auto &Node = Root.getMap();

  auto GetUInt = [](msgpack::DocNode &Node, auto &Out) {
    if (!Node.isEmpty())
      Out = Node.getUInt();
  };

  uint64_t Version = 0;
  GetUInt(Node[MsgPackFormat::Version], Version);
  if (Version != MsgPackFormat::MajorVersion)
    return make_error<StringError>("bad/missing llvmraytracing pipelinestate version", inconvertibleErrorCode());

  PipelineState State = {};
  GetUInt(Node[MsgPackFormat::MaxUsedPayloadRegisterCount], State.MaxUsedPayloadRegisterCount);

  auto &SDSNode = Node[MsgPackFormat::SpecializeDriverShadersState];
  auto SDSStateOrErr = SpecializeDriverShadersState::decodeMsgpack(SDSNode);
  if (auto Err = SDSStateOrErr.takeError())
    return Err;
  State.SDSState = *SDSStateOrErr;

  return State;
}

Expected<PipelineState> PipelineState::decodeMsgpack(StringRef Data) {
  msgpack::Document Doc;

  if (!Doc.readFromBlob(Data, false))
    return make_error<StringError>("failed to parse msgpack", inconvertibleErrorCode());

  auto &Root = Doc.getRoot().getMap();
  return decodeMsgpack(Root);
}

void PipelineState::encodeMsgpack(llvm::msgpack::DocNode &Root) const {
  auto &Node = Root.getMap(true);
  Node[MsgPackFormat::Version] = MsgPackFormat::MajorVersion;
  Node[MsgPackFormat::MaxUsedPayloadRegisterCount] = MaxUsedPayloadRegisterCount;
  SDSState.encodeMsgpack(Node[MsgPackFormat::SpecializeDriverShadersState]);
}

std::string PipelineState::encodeMsgpack() const {
  msgpack::Document Doc;

  auto &Root = Doc.getRoot().getMap(true);
  encodeMsgpack(Root);

  std::string Out;
  Doc.writeToBlob(Out);
  return Out;
}

llvm::Expected<PipelineState> PipelineState::fromModuleMetadata(const llvm::Module &M) {
  PipelineState State = {};
  auto OptMaxUsedPayloadRegCount = ContHelper::tryGetMaxUsedPayloadRegisterCount(M);
  if (OptMaxUsedPayloadRegCount.has_value())
    State.MaxUsedPayloadRegisterCount = *OptMaxUsedPayloadRegCount;
  auto SDSStateOrErr = SpecializeDriverShadersState::fromModuleMetadata(M);
  if (auto Err = SDSStateOrErr.takeError())
    return Err;
  State.SDSState = *SDSStateOrErr;
  return State;
}

void PipelineState::exportModuleMetadata(llvm::Module &M) const {
  if (MaxUsedPayloadRegisterCount) {
    ContHelper::setMaxUsedPayloadRegisterCount(M, MaxUsedPayloadRegisterCount);
  }
  SDSState.exportModuleMetadata(M);
}

void PipelineState::merge(const PipelineState &Other) {
  MaxUsedPayloadRegisterCount = std::max(MaxUsedPayloadRegisterCount, Other.MaxUsedPayloadRegisterCount);
  SDSState.merge(Other.SDSState);
}

void PipelineState::print(llvm::raw_ostream &OS) const {
  OS << "PipelineState { MaxUsedPayloadRegisterCount=" << MaxUsedPayloadRegisterCount << ", SDSState=";
  SDSState.print(OS);
  OS << " }\n";
}

#ifndef NDEBUG
void PipelineState::dump() const {
  print(dbgs());
}
#endif

} // namespace llvmraytracing
