#pragma once
#include "vfxSection.h"

namespace Vfx {
// =====================================================================================================================
// Represents the sub section descriptor range value
class SectionDescriptorRangeValueItem : public Section {
public:
  typedef Vkgc::StaticDescriptorValue SubState;

  SectionDescriptorRangeValueItem() : Section(m_addrTable, MemberCount, SectionTypeUnset, "descriptorRangeValue") {
    m_intData = &m_bufMem;
    m_uintData = &m_bufMem;
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, visibility, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, type, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, set, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, arraySize, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, m_uintData, MemberTypeUArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, m_intData, MemberTypeIArray, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }
  void getSubState(SubState &state) {
    state = m_state;
    state.pValue = m_bufMem.size() > 0 ? (const unsigned *)(&m_bufMem[0]) : nullptr;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 7;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<uint8_t> *m_intData;
  std::vector<uint8_t> *m_uintData;
  SubState m_state;
  std::vector<uint8_t> m_bufMem;
};

// =====================================================================================================================
// Represents the sub section resource mapping node
class SectionResourceMappingNode : public Section {
public:
  typedef Vkgc::ResourceMappingNode SubState;

  SectionResourceMappingNode() : Section(m_addrTable, MemberCount, SectionTypeUnset, "userDataNode") {
    memset(&m_state, 0, sizeof(m_state));
    m_visibility = 0;
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, m_visibility, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, type, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, sizeInDwords, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, offsetInDwords, MemberTypeInt, false);
    INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, set, srdRange.set,
                                           SectionResourceMappingNode::getResourceMapNodeSet, MemberTypeInt, false);
    INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, binding, srdRange.binding,
                                           SectionResourceMappingNode::getResourceMapNodeBinding, MemberTypeInt, false);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMappingNode, m_next, MemberTypeResourceMappingNode, true);
    INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, indirectUserDataCount, userDataPtr.sizeInDwords,
                                           SectionResourceMappingNode::getResourceMapNodeUserDataCount, MemberTypeInt,
                                           false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  SubState &getSubStateRef() { return m_state; };

  void getSubState(SubState &state) {
    if (m_state.type == Vkgc::ResourceMappingNodeType::DescriptorTableVaPtr) {
      m_nextNodeBuf.resize(m_next.size());
      for (unsigned i = 0; i < m_next.size(); ++i)
        m_next[i].getSubState(m_nextNodeBuf[i]);
      m_state.tablePtr.pNext = &m_nextNodeBuf[0];
      m_state.tablePtr.nodeCount = static_cast<unsigned>(m_nextNodeBuf.size());
    }
    state = m_state;
  };

  void getSubState(Vkgc::ResourceMappingRootNode &state) {
    getSubState(state.node);
    state.visibility = m_visibility;
  }

private:
  static void *getResourceMapNodeSet(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.srdRange.set);
  }

  static void *getResourceMapNodeBinding(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.srdRange.binding);
  }

  static void *getResourceMapNodeUserDataCount(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.userDataPtr.sizeInDwords);
  }

  static const unsigned MemberCount = 10;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<SectionResourceMappingNode> m_next; // Next resource mapping node
  uint32_t m_visibility;
  SubState m_state;
  std::vector<Vkgc::ResourceMappingNode> m_nextNodeBuf; // Contains next nodes
};

// =====================================================================================================================
// Represents the sub section shader option
class SectionShaderOption : public Section {
public:
  typedef Vkgc::PipelineShaderOptions SubState;

  SectionShaderOption() : Section(m_addrTable, MemberCount, SectionTypeUnset, "options") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, trapPresent, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, debugMode, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, enablePerformanceData, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, allowReZ, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, vgprLimit, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, sgprLimit, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, maxThreadGroupsPerComputeUnit, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, waveSize, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, wgpMode, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, waveBreakSize, MemberTypeEnum, false);

    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, forceLoopUnrollCount, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, useSiScheduler, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, updateDescInElf, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, allowVaryWaveSize, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, enableLoadScalarizer, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableLicm, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, unrollThreshold, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, scalarThreshold, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, fp32DenormalMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableLoopUnroll, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, adjustDepthImportVrs, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableLicmThreshold, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, unrollHintThreshold, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, dontUnrollHintThreshold, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, fastMathFlags, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableFastMathFlags, MemberTypeInt, false);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 26;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section pipeline shader info
class SectionShaderInfo : public Section {
public:
  typedef Vkgc::PipelineShaderInfo SubState;
  SectionShaderInfo(const SectionInfo &info)
      : Section(m_addrTable, MemberCount, info.type, nullptr), m_shaderStage(static_cast<ShaderStage>(info.property)) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_entryPoint, MemberTypeString, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_specConst, MemberTypeSpecInfo, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_options, MemberTypeShaderOption, true);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, m_descriptorRangeValue, MemberTypeDescriptorRangeValue, true);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, m_userDataNode, MemberTypeResourceMappingNode, true);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    memset(&state, 0, sizeof(SubState));
    state.entryStage = m_shaderStage;
    state.pEntryTarget = m_entryPoint.c_str();
    memcpy(&state.options, &m_state.options, sizeof(m_state.options));

    m_specConst.getSubState(m_specializationInfo);
    state.pSpecializationInfo = &m_specializationInfo;

    m_options.getSubState(state.options);

    if (m_descriptorRangeValue.size() > 0) {
      m_descriptorRangeValues.resize(m_descriptorRangeValue.size());
      for (unsigned i = 0; i < static_cast<unsigned>(m_descriptorRangeValue.size()); ++i)
        m_descriptorRangeValue[i].getSubState(m_descriptorRangeValues[i]);
    }

    if (m_userDataNode.size() > 0) {
      m_userDataNodes.resize(m_userDataNode.size());
      for (unsigned i = 0; i < static_cast<unsigned>(m_userDataNode.size()); ++i)
        m_userDataNode[i].getSubState(m_userDataNodes[i]);
    }
  };
  SubState &getSubStateRef() { return m_state; };

  const char *getEntryPoint() const { return m_entryPoint.empty() ? nullptr : m_entryPoint.c_str(); }
  ShaderStage getShaderStage() { return m_shaderStage; }

  void getSubState(std::vector<Vkgc::ResourceMappingRootNode> &userDataNodes) {
    for (auto &srcNode : m_userDataNodes)
      userDataNodes.push_back({srcNode, static_cast<unsigned>(1 << m_shaderStage)});
  }

  void getSubState(std::vector<Vkgc::StaticDescriptorValue> &descriptorRangeValues) {
    for (auto &srcValue : m_descriptorRangeValues) {
      Vkgc::StaticDescriptorValue dstValue = {};
      memcpy(&dstValue, &srcValue, sizeof(srcValue));
      dstValue.visibility = 1 << m_shaderStage;
      descriptorRangeValues.push_back(dstValue);
    }
  }

private:
  static const unsigned MemberCount = 5;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
  SectionSpecInfo m_specConst;   // Specialization constant info
  SectionShaderOption m_options; // Pipeline shader options
  std::string m_entryPoint;      // Entry point name

  // Used for backwards compatibility with Version 1 .pipe files
  std::vector<SectionDescriptorRangeValueItem> m_descriptorRangeValue; // Contains descriptor range value
  std::vector<SectionResourceMappingNode> m_userDataNode;              // Contains user data node
  std::vector<SectionDescriptorRangeValueItem::SubState> m_descriptorRangeValues;
  std::vector<SectionResourceMappingNode::SubState> m_userDataNodes;

  VkSpecializationInfo m_specializationInfo;
  ShaderStage m_shaderStage;
};

// =====================================================================================================================
// Represents the sub section ResourceMapping
class SectionResourceMapping : public Section {
public:
  typedef Vkgc::ResourceMappingData SubState;

  SectionResourceMapping() : Section(m_addrTable, MemberCount, SectionTypeResourceMapping, "ResourceMapping") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMapping, m_descriptorRangeValue, MemberTypeDescriptorRangeValue,
                                      true);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMapping, m_userDataNode, MemberTypeResourceMappingNode, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    memset(&state, 0, sizeof(SubState));

    if (m_descriptorRangeValue.size() > 0) {
      m_descriptorRangeValues.resize(m_descriptorRangeValue.size());
      for (unsigned i = 0; i < m_descriptorRangeValue.size(); ++i)
        m_descriptorRangeValue[i].getSubState(m_descriptorRangeValues[i]);
      state.staticDescriptorValueCount = static_cast<unsigned>(m_descriptorRangeValue.size());
      state.pStaticDescriptorValues = &m_descriptorRangeValues[0];
    }

    if (m_userDataNode.size() > 0) {
      state.userDataNodeCount = static_cast<unsigned>(m_userDataNode.size());
      m_userDataNodes.resize(state.userDataNodeCount);
      for (unsigned i = 0; i < state.userDataNodeCount; ++i)
        m_userDataNode[i].getSubState(m_userDataNodes[i]);
      state.pUserDataNodes = &m_userDataNodes[0];
    }
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 2;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
  std::vector<SectionDescriptorRangeValueItem> m_descriptorRangeValue; // Contains descriptor range value
  std::vector<SectionResourceMappingNode> m_userDataNode;              // Contains user data node

  std::vector<Vkgc::StaticDescriptorValue> m_descriptorRangeValues;
  std::vector<Vkgc::ResourceMappingRootNode> m_userDataNodes;
};

// =====================================================================================================================
// Represents the sub section ExtendedRobustness
class SectionExtendedRobustness : public Section {
public:
  typedef Vkgc::ExtendedRobustness SubState;

  SectionExtendedRobustness() : Section(m_addrTable, MemberCount, SectionTypeUnset, "extendedRobustness") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, robustBufferAccess, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, robustImageAccess, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, nullDescriptor, MemberTypeBool, false);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section pipeline option
class SectionPipelineOption : public Section {
public:
  typedef Vkgc::PipelineOptions SubState;

  SectionPipelineOption() : Section(m_addrTable, MemberCount, SectionTypeUnset, "options") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, includeDisassembly, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, scalarBlockLayout, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, includeIr, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, robustBufferAccess, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, reconfigWorkgroupLayout, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTableUsage, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTablePtrHigh, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, resourceLayoutScheme, MemberTypeEnum, false);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 53
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, optimizationLevel, MemberTypeInt, false);
#endif
    INIT_MEMBER_NAME_TO_ADDR(SectionPipelineOption, m_extendedRobustness, MemberTypeExtendedRobustness, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    m_extendedRobustness.getSubState(m_state.extendedRobustness);
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 11;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
  SectionExtendedRobustness m_extendedRobustness;
};

// =====================================================================================================================
// Represents the sub section NGG state
class SectionNggState : public Section {
public:
  typedef Vkgc::NggState SubState;

  SectionNggState() : Section(m_addrTable, MemberCount, SectionTypeUnset, "nggState") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableNgg, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableGsUse, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, forceCullingMode, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, compactMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableVertexReuse, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableBackfaceCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableFrustumCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableBoxFilterCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableSphereCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableSmallPrimFilter, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableCullDistanceCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, backfaceExponent, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, subgroupSizing, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, primsPerSubgroup, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, vertsPerSubgroup, MemberTypeInt, false);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 17;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the section graphics state
class SectionGraphicsState : public Section {
public:
  typedef GraphicsPipelineState SubState;

  SectionGraphicsState() : Section(m_addrTable, MemberCount, SectionTypeGraphicsState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, topology, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, provokingVertexMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, patchControlPoints, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, deviceIndex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, disableVertexReuse, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, depthClipEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rasterizerDiscardEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, perSampleShading, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, numSamples, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, pixelShaderSamples, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, samplePatternIdx, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, usrClipPlaneMask, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, alphaToCoverageEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, dualSourceBlendEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, switchWinding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableMultiView, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_options, MemberTypePipelineOption, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_nggState, MemberTypeNggState, true);
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGraphicsState, m_colorBuffer, MemberTypeColorBufferItem,
                                   Vkgc::MaxColorTargets, true);

    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, dynamicVertexStride, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableUberFetchShader, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableEarlyCompile, MemberTypeBool, false);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    for (unsigned i = 0; i < Vkgc::MaxColorTargets; ++i)
      m_colorBuffer[i].getSubState(m_state.colorBuffer[i]);
    m_options.getSubState(m_state.options);
    m_nggState.getSubState(m_state.nggState);
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  SectionNggState m_nggState;
  static const unsigned MemberCount = 25;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
  SectionColorBuffer m_colorBuffer[Vkgc::MaxColorTargets]; // Color buffer
  SectionPipelineOption m_options;
};

// =====================================================================================================================
// Represents the section compute state
class SectionComputeState : public Section {
public:
  typedef ComputePipelineState SubState;

  SectionComputeState() : Section(m_addrTable, MemberCount, SectionTypeComputeState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, deviceIndex, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_options, MemberTypePipelineOption, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    m_options.getSubState(m_state.options);
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 5;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
  SectionPipelineOption m_options;
};

} // namespace Vfx
