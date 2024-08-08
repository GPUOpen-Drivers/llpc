#pragma once
#include "vfxSection.h"

namespace Vfx {
// =====================================================================================================================
// Represents the sub section descriptor range value
class SectionDescriptorRangeValueItem : public Section {
public:
  typedef Vkgc::StaticDescriptorValue SubState;

  SectionDescriptorRangeValueItem() : Section(getAddrTable(), SectionTypeUnset, "descriptorRangeValue") {
    m_intData = &m_bufMem;
    m_uintData = &m_bufMem;
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) {
    state = m_state;
    state.pValue = m_bufMem.size() > 0 ? (const unsigned *)(&m_bufMem[0]) : nullptr;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, visibility, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, type, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, set, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, binding, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, arraySize, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, m_uintData, MemberTypeUArray, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, m_intData, MemberTypeIArray, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

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

  SectionResourceMappingNode() : Section(getAddrTable(), SectionTypeUnset, "userDataNode") {
    memset(&m_state, 0, sizeof(m_state));
    m_visibility = 0;
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
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, m_visibility, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, type, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, sizeInDwords, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, offsetInDwords, MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, set, srdRange.set,
                                             SectionResourceMappingNode::getResourceMapNodeSet, MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, binding, srdRange.binding,
                                             SectionResourceMappingNode::getResourceMapNodeBinding, MemberTypeInt,
                                             false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, strideInDwords, srdRange.strideInDwords,
                                             SectionResourceMappingNode::getResourceMapNodeStride, MemberTypeInt,
                                             false);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMappingNode, m_next, MemberTypeResourceMappingNode, true);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(
          SectionResourceMappingNode, indirectUserDataCount, userDataPtr.sizeInDwords,
          SectionResourceMappingNode::getResourceMapNodeUserDataCount, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  static void *getResourceMapNodeSet(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.srdRange.set);
  }

  static void *getResourceMapNodeBinding(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.srdRange.binding);
  }

  static void *getResourceMapNodeStride(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.srdRange.strideInDwords);
  }

  static void *getResourceMapNodeUserDataCount(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.userDataPtr.sizeInDwords);
  }

  std::vector<SectionResourceMappingNode> m_next; // Next resource mapping node
  uint32_t m_visibility;
  SubState m_state;
  std::vector<Vkgc::ResourceMappingNode> m_nextNodeBuf; // Contains next nodes
};

// =====================================================================================================================
// Represents one entry in a default uniform constant map
class SectionUniformConstantMapEntry : public Section {
public:
  typedef Vkgc::UniformConstantMapEntry SubState;

  SectionUniformConstantMapEntry()
      : Section(getAddrTable(), SectionTypeUniformConstantMapEntry, "UniformConstantMapEntry") {
    memset(&m_state, 0, sizeof(m_state));
  }

  SubState &getSubStateRef() { return m_state; }

  void getSubState(SubState &state) { state = m_state; }

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionUniformConstantMapEntry, location, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionUniformConstantMapEntry, offset, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }
  SubState m_state;
};

// =====================================================================================================================
// Represents one default uniform constant map
class SectionUniformConstantMap : public Section {
public:
  typedef Vkgc::UniformConstantMap SubState;

  SectionUniformConstantMap() : Section(getAddrTable(), SectionTypeUniformConstantMap, "UniformConstantMap") {
    memset(&m_state, 0, sizeof(m_state));
  }
  SubState &getSubStateRef() { return m_state; }

  void getSubState(SubState &state) {
    m_uniformConstantsData.resize(m_uniformConstants.size());
    for (unsigned i = 0; i < m_uniformConstants.size(); i++) {
      auto &s = m_uniformConstants[i];
      s.getSubState(m_uniformConstantsData[i]);
    }
    m_state.numUniformConstants = m_uniformConstants.size();
    m_state.pUniforms = m_uniformConstantsData.data();
    state = m_state;
  }

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionUniformConstantMap, visibility, MemberTypeInt, false);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionUniformConstantMap, m_uniformConstants,
                                        MemberTypeUniformConstantMapEntry, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }
  SubState m_state;
  std::vector<SectionUniformConstantMapEntry> m_uniformConstants;
  std::vector<SectionUniformConstantMapEntry::SubState> m_uniformConstantsData;
};

// =====================================================================================================================
// Represents the sub section shader option
class SectionShaderOption : public Section {
public:
  typedef Vkgc::PipelineShaderOptions SubState;

  SectionShaderOption() : Section(getAddrTable(), SectionTypeUnset, "options"), m_clientHash{} {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) {
    m_state.clientHash.lower = m_clientHash.i64Vec2[0];
    m_state.clientHash.upper = m_clientHash.i64Vec2[1];
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_NAME_TO_ADDR(SectionShaderOption, m_clientHash, MemberTypeI64Vec2, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, trapPresent, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, debugMode, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, enablePerformanceData, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, allowReZ, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, forceLateZ, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, vgprLimit, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, sgprLimit, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, maxThreadGroupsPerComputeUnit, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, waveSize, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, subgroupSize, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, wgpMode, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, waveBreakSize, MemberTypeEnum, false);

      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, forceLoopUnrollCount, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, useSiScheduler, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableCodeSinking, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, favorLatencyHiding, MemberTypeBool, false);
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
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, noContractOpDot, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, fastMathFlags, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableFastMathFlags, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, ldsSpillLimitDwords, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, scalarizeWaterfallLoads, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, overrideForceThreadIdSwizzling, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, overrideShaderThreadGroupSizeX, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, overrideShaderThreadGroupSizeY, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, overrideShaderThreadGroupSizeZ, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, nsaThreshold, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, aggressiveInvariantLoads, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, workaroundStorageImageFormats, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, workaroundInitializeOutputsToZero, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableFMA, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableReadFirstLaneWorkaround, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, backwardPropagateNoContract, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, forwardPropagateNoContract, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, constantBufferBindingOffset, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, imageSampleDrefReturnsRgba, MemberTypeBool, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
  IUFValue m_clientHash;
};

// =====================================================================================================================
// Represents the sub section pipeline shader info
class SectionShaderInfo : public Section {
public:
  typedef Vkgc::PipelineShaderInfo SubState;
  SectionShaderInfo(const SectionInfo &info)
      : Section(getAddrTable(), info.type, nullptr), m_shaderStage(static_cast<ShaderStage>(info.property)) {
    memset(&m_state, 0, sizeof(m_state));
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
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_entryPoint, MemberTypeString, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_specConst, MemberTypeSpecInfo, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_options, MemberTypeShaderOption, true);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, m_descriptorRangeValue, MemberTypeDescriptorRangeValue,
                                        true);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, m_userDataNode, MemberTypeResourceMappingNode, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

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

  SectionResourceMapping() : Section(getAddrTable(), SectionTypeResourceMapping, "ResourceMapping") {
    memset(&m_state, 0, sizeof(m_state));
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
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMapping, m_descriptorRangeValue, MemberTypeDescriptorRangeValue,
                                        true);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMapping, m_userDataNode, MemberTypeResourceMappingNode, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

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

  SectionExtendedRobustness() : Section(getAddrTable(), SectionTypeUnset, "extendedRobustness") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, robustBufferAccess, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, robustImageAccess, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, nullDescriptor, MemberTypeBool, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 73
// =====================================================================================================================
// Represents the sub section GLState
class SectionGlState : public Section {
public:
  typedef Vkgc::PipelineOptions::GLState SubState;

  SectionGlState() : Section(getAddrTable(), SectionTypeUnset, "glState") { memset(&m_state, 0, sizeof(m_state)); }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, replaceSetWithResourceType, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, disableSampleMask, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, buildResourcesDataForShaderModule, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, disableTruncCoordForGather, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, enableCombinedTexture, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, vertex64BitsAttribSingleLoc, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, enableFragColor, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, disableBaseVertex, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, bindlessTextureMode, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, bindlessImageMode, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, enablePolygonStipple, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, enableLineSmooth, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, emulateWideLineStipple, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGlState, enablePointSmooth, MemberTypeBool, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};
#endif

// =====================================================================================================================
// Represents the sub section pipeline option
class SectionPipelineOption : public Section {
public:
  typedef Vkgc::PipelineOptions SubState;

  SectionPipelineOption() : Section(getAddrTable(), SectionTypeUnset, "options") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) {
    m_extendedRobustness.getSubState(m_state.extendedRobustness);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 73
    m_glState.getSubState(m_state.glState);
#endif
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, includeDisassembly, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, scalarBlockLayout, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, includeIr, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, robustBufferAccess, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableRelocatableShaderElf, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, disableImageResourceCheck, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableScratchAccessBoundsChecks, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, reconfigWorkgroupLayout, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, forceCsThreadIdSwizzling, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, overrideThreadGroupSizeX, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, overrideThreadGroupSizeY, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, overrideThreadGroupSizeZ, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTableUsage, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTablePtrHigh, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, resourceLayoutScheme, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, optimizationLevel, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, threadGroupSwizzleMode, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, reverseThreadGroup, MemberTypeBool, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionPipelineOption, m_extendedRobustness, MemberTypeExtendedRobustness, true);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, forceNonUniformResourceIndexStageMask, MemberTypeInt,
                                     false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableImplicitInvariantExports, MemberTypeBool, false);
      // One internal member
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, internalRtShaders, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableRayQuery, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, optimizeTessFactor, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableInterpModePatch, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, pageMigrationEnabled, MemberTypeBool, false);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 73
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, replaceSetWithResourceType, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, disableSampleMask, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, buildResourcesDataForShaderModule, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, disableTruncCoordForGather, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableCombinedTexture, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, vertex64BitsAttribSingleLoc, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableFragColor, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, disableBaseVertex, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, bindlessTextureMode, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, bindlessImageMode, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enablePolygonStipple, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enableLineSmooth, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, emulateWideLineStipple, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enablePointSmooth, MemberTypeBool, false);
#else
      INIT_MEMBER_NAME_TO_ADDR(SectionPipelineOption, m_glState, MemberTypeGlState, true);
#endif
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, enablePrimGeneratedQuery, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, disablePerCompFetch, MemberTypeBool, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
  SectionExtendedRobustness m_extendedRobustness;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 73
  SectionGlState m_glState;
#endif
};

// =====================================================================================================================
// Represents the sub section NGG state
class SectionNggState : public Section {
public:
  typedef Vkgc::NggState SubState;

  SectionNggState() : Section(getAddrTable(), SectionTypeUnset, "nggState") { memset(&m_state, 0, sizeof(m_state)); }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableNgg, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableGsUse, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, forceCullingMode, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, compactVertex, MemberTypeBool, false);
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
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section IndirectCalleeSavedRegs state
class SectionIndirectCalleeSavedRegs : public Section {
public:
  typedef Vkgc::RayTracingShaderExportConfig SubState;
  SectionIndirectCalleeSavedRegs() : Section(getAddrTable(), SectionTypeUnset, "exportConfig") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state.indirectCalleeSavedRegs = m_state.indirectCalleeSavedRegs; }

  SubState &getSubStateRef() { return m_state; }

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionIndirectCalleeSavedRegs, raygen, raygen,
                                             SectionIndirectCalleeSavedRegs::GetExportConfigMember<offsetof(
                                                 Vkgc::RayTracingShaderExportConfig, indirectCalleeSavedRegs.raygen)>,
                                             MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionIndirectCalleeSavedRegs, miss, miss,
                                             SectionIndirectCalleeSavedRegs::GetExportConfigMember<offsetof(
                                                 Vkgc::RayTracingShaderExportConfig, indirectCalleeSavedRegs.miss)>,
                                             MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(
          SectionIndirectCalleeSavedRegs, closestHit, closestHit,
          SectionIndirectCalleeSavedRegs::GetExportConfigMember<offsetof(Vkgc::RayTracingShaderExportConfig,
                                                                         indirectCalleeSavedRegs.closestHit)>,
          MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionIndirectCalleeSavedRegs, anyHit, anyHit,
                                             SectionIndirectCalleeSavedRegs::GetExportConfigMember<offsetof(
                                                 Vkgc::RayTracingShaderExportConfig, indirectCalleeSavedRegs.anyHit)>,
                                             MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(
          SectionIndirectCalleeSavedRegs, intersection, intersection,
          SectionIndirectCalleeSavedRegs::GetExportConfigMember<offsetof(Vkgc::RayTracingShaderExportConfig,
                                                                         indirectCalleeSavedRegs.intersection)>,
          MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionIndirectCalleeSavedRegs, callable, callable,
                                             SectionIndirectCalleeSavedRegs::GetExportConfigMember<offsetof(
                                                 Vkgc::RayTracingShaderExportConfig, indirectCalleeSavedRegs.callable)>,
                                             MemberTypeInt, false);
      INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(
          SectionIndirectCalleeSavedRegs, traceRays, traceRays,
          SectionIndirectCalleeSavedRegs::GetExportConfigMember<offsetof(Vkgc::RayTracingShaderExportConfig,
                                                                         indirectCalleeSavedRegs.traceRays)>,
          MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  template <size_t Offset> static void *GetExportConfigMember(void *pObj) {
    return reinterpret_cast<uint8_t *>(&reinterpret_cast<SectionIndirectCalleeSavedRegs *>(pObj)->m_state) + Offset;
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section RayTracingShaderExportConfig state
class SectionRayTracingShaderExportConfig : public Section {
public:
  typedef Vkgc::RayTracingShaderExportConfig SubState;
  SectionRayTracingShaderExportConfig() : Section(getAddrTable(), SectionTypeUnset, "exportConfig") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, indirectCallingConvention, MemberTypeInt,
                                     false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, enableUniformNoReturn, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, enableTraceRayArgsInLds, MemberTypeBool,
                                     false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, enableReducedLinkageOpt, MemberTypeBool,
                                     false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, readsDispatchRaysIndex, MemberTypeBool,
                                     false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, enableDynamicLaunch, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, emitRaytracingShaderDataToken, MemberTypeBool,
                                     false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingShaderExportConfig, m_indirectCalleeSavedRegs,
                               MemberTypeIndirectCalleeSavedRegs, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  void getSubState(SubState &state) {
    state = m_state;
    m_indirectCalleeSavedRegs.getSubState(state);
  }

  SubState &getSubStateRef() { return m_state; }

private:
  SubState m_state;
  SectionIndirectCalleeSavedRegs m_indirectCalleeSavedRegs;
};

// =====================================================================================================================
// Represents the sub section GpurtFuncTable state
class SectionGpurtFuncTable : public Section {
public:
  typedef Vkgc::GpurtFuncTable SubState;
  SectionGpurtFuncTable() : Section(getAddrTable(), SectionTypeUnset, "gpurtFuncTable") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) {
    for (unsigned i = 0; i < Vkgc::RT_ENTRY_FUNC_COUNT; i++)
      strcpy(state.pFunc[i], m_pFunc[i].c_str());
  }

  SubState &getSubStateRef() { return m_state; }

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGpurtFuncTable, m_pFunc, MemberTypeString, Vkgc::RT_ENTRY_FUNC_COUNT,
                                     false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
  std::string m_pFunc[Vkgc::RT_ENTRY_FUNC_COUNT];
};

// =====================================================================================================================
// Represents a raytracing library summary
class SectionRayTracingLibrarySummary : public Section {
public:
  SectionRayTracingLibrarySummary() : Section({}, SectionTypeRayTracingLibrarySummary, "RayTracingLibrarySummary") {}

  virtual void addLine(const char *line) { m_yaml += line; }

  Vkgc::BinaryData getSubState();

private:
  std::string m_yaml;
  std::string m_msgpack;
};

// =====================================================================================================================
// Represents the sub section RtState state
class SectionRtState : public Section {
public:
  typedef Vkgc::RtState SubState;
  SectionRtState() : Section(getAddrTable(), SectionTypeUnset, "rtState") { memset(&m_state, 0, sizeof(m_state)); }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    state = m_state;
    state.bvhResDesc.dataSizeInDwords = m_bvhResDescSize;
    for (unsigned i = 0; i < m_bvhResDesc.size(); ++i)
      state.bvhResDesc.descriptorData[i] = m_bvhResDesc[i];
    m_exportConfig.getSubState(state.exportConfig);
    m_gpurtFuncTable.getSubState(state.gpurtFuncTable);

    if (!parseRtIpVersion(&state.rtIpVersion)) {
      PARSE_ERROR(*errorMsg, 0, "Failed to parse rtIpVersion\n");
    }

    std::string dummySource;
    if (!m_gpurtShaderLibrary.empty()) {
      bool ret = readFile(docFilename, m_gpurtShaderLibrary, true, &m_gpurtShaderLibraryBinary, &dummySource, errorMsg);
      if (ret) {
        state.gpurtShaderLibrary.codeSize = m_gpurtShaderLibraryBinary.size();
        state.gpurtShaderLibrary.pCode = &m_gpurtShaderLibraryBinary[0];
      }
    }
  }

  SubState &getSubStateRef() { return m_state; }

private:
  bool parseRtIpVersion(Vkgc::RtIpVersion *rtIpVersion);

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_NAME_TO_ADDR(SectionRtState, m_bvhResDescSize, MemberTypeInt, false);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionRtState, m_bvhResDesc, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, nodeStrideShift, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, staticPipelineFlags, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, triCompressMode, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, pipelineFlags, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, threadGroupSizeX, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, threadGroupSizeY, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, threadGroupSizeZ, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, boxSortHeuristicMode, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, counterMode, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, counterMask, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, rayQueryCsSwizzle, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, ldsStackSize, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, dispatchRaysThreadGroupSize, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, ldsSizePerThreadGroup, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, outerTileSize, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, dispatchDimSwizzleMode, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableRayQueryCsSwizzle, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableDispatchRaysInnerSwizzle, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableDispatchRaysOuterSwizzle, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, forceInvalidAccelStruct, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableRayTracingCounters, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableRayTracingHwTraversalStack, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableOptimalLdsStackSizeForIndirect, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableOptimalLdsStackSizeForUnified, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, maxRayLength, MemberTypeFloat, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRtState, m_exportConfig, MemberTypeRayTracingShaderExportConfig, true);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, gpurtFeatureFlags, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRtState, m_gpurtShaderLibrary, MemberTypeString, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRtState, m_gpurtFuncTable, MemberTypeGpurtFuncTable, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionRtState, m_rtIpVersion, MemberTypeString, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, gpurtOverride, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, rtIpOverride, MemberTypeBool, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
  SectionRayTracingShaderExportConfig m_exportConfig;
  std::string m_gpurtShaderLibrary;
  std::vector<uint8_t> m_gpurtShaderLibraryBinary;
  SectionGpurtFuncTable m_gpurtFuncTable;
  std::string m_rtIpVersion;
  unsigned m_bvhResDescSize = 0;
  std::vector<unsigned> m_bvhResDesc;
};

// =====================================================================================================================
// Represents the sub section XfbOutInfo
class SectionXfbOutInfo : public Section {
public:
  typedef Vkgc::XfbOutInfo SubState;

  SectionXfbOutInfo() : Section(getAddrTable(), SectionTypeUnset, "XfbOutInfo") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionXfbOutInfo, isBuiltIn, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionXfbOutInfo, location, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionXfbOutInfo, component, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionXfbOutInfo, xfbBuffer, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionXfbOutInfo, xfbOffset, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionXfbOutInfo, xfbStride, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionXfbOutInfo, streamId, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section AdvancedBlendInfo
class SectionAdvancedBlendInfo : public Section {
public:
  typedef Vkgc::AdvancedBlendInfo SubState;

  SectionAdvancedBlendInfo() : Section(getAddrTable(), SectionTypeUnset, "advancedBlendInfo") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionAdvancedBlendInfo, enableAdvancedBlend, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionAdvancedBlendInfo, binding, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the section graphics state
class SectionGraphicsState : public Section {
public:
  typedef Vkgc::GraphicsPipelineBuildInfo SubState;

  SectionGraphicsState()
      : Section(getAddrTable(), SectionTypeGraphicsState, nullptr), m_pUniformMaps{}, m_uniformMaps{} {
    memset(&m_state, 0, sizeof(m_state));

    m_usrClipPlaneMask = 0;
    m_tessLevelInner[0] = -1.0f;
    m_tessLevelInner[1] = -1.0f;
    m_tessLevelOuter[0] = -1.0f;
    m_tessLevelOuter[1] = -1.0f;
    m_tessLevelOuter[2] = -1.0f;
    m_tessLevelOuter[3] = -1.0f;
    m_clientMetadata = &m_clientMetadataBufMem;
    m_forceDisableStreamOut = false;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 71
    m_state.ppUniformMaps = &m_pUniformMaps[0];
#else
    m_state.glState.ppUniformMaps = &m_pUniformMaps[0];
#endif
  }

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, iaState, topology, MemberTypeEnum, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, iaState, patchControlPoints, MemberTypeInt, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, iaState, deviceIndex, MemberTypeInt, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, iaState, disableVertexReuse, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, iaState, switchWinding, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, iaState, enableMultiView, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, iaState, useVertexBufferDescArray, MemberTypeBool,
                                         false);
      INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGraphicsState, m_tessLevelInner, MemberTypeFloat, 2, false);
      INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGraphicsState, m_tessLevelOuter, MemberTypeFloat, 4, false);

      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, vpState, depthClipEnable, MemberTypeBool, false);

      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, rasterizerDiscardEnable, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, innerCoverage, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, perSampleShading, MemberTypeBool, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_usrClipPlaneMask, MemberTypeInt, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, numSamples, MemberTypeInt, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, pixelShaderSamples, MemberTypeInt, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, samplePatternIdx, MemberTypeInt, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, dynamicSampleInfo, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, rasterStream, MemberTypeInt, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rsState, provokingVertexMode, MemberTypeEnum, false);

      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, cbState, alphaToCoverageEnable, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, cbState, dualSourceBlendEnable, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, cbState, dualSourceBlendDynamic, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, glState, enableColorClampVs, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, glState, enableColorClampFs, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, glState, enableFlatShade, MemberTypeBool, false);
      INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGraphicsState, m_colorBuffer, MemberTypeColorBufferItem,
                                     Vkgc::MaxColorTargets, true);

      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_nggState, MemberTypeNggState, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_options, MemberTypePipelineOption, true);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, unlinked, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, dynamicVertexStride, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableUberFetchShader, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableColorExportShader, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableEarlyCompile, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, dynamicTopology, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, useSoftwareVertexBufferDescriptors, MemberTypeBool, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_shaderLibrary, MemberTypeString, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_rtState, MemberTypeRtState, true);

      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_clientMetadata, MemberTypeU8Array, false);
      INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGraphicsState, m_uniformConstantMaps, MemberTypeUniformConstantMap,
                                     Vkgc::ShaderStageGfxCount, true);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 71
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, originUpperLeft, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, vbAddressLowBitsKnown, MemberTypeBool, false);
#else
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, glState, originUpperLeft, MemberTypeBool, false);
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, glState, vbAddressLowBitsKnown, MemberTypeBool, false);
#endif
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_forceDisableStreamOut, MemberTypeBool, false);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 70
      INIT_STATE_SUB_MEMBER_NAME_TO_ADDR(SectionGraphicsState, apiXfbOutData, forceEnablePrimStats, MemberTypeBool,
                                         false);
#endif
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionGraphicsState, m_xfbOutInfo, MemberTypeXfbOutInfo, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_advancedBlendInfo, MemberTypeAdvancedBlendInfo, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    for (unsigned i = 0; i < Vkgc::MaxColorTargets; ++i) {
      ColorBuffer colorBuffer = {};
      m_colorBuffer[i].getSubState(colorBuffer);
      m_state.cbState.target[i].blendEnable = colorBuffer.blendEnable;
      m_state.cbState.target[i].blendSrcAlphaToColor = colorBuffer.blendSrcAlphaToColor;
      m_state.cbState.target[i].channelWriteMask = colorBuffer.channelWriteMask;
      m_state.cbState.target[i].format = colorBuffer.format;
    }
    m_advancedBlendInfo.getSubState(m_state.advancedBlendInfo);
    m_options.getSubState(m_state.options);
    m_nggState.getSubState(m_state.nggState);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 71
    auto pGlState = &m_state;
#else
    auto pGlState = &m_state.glState;
#endif
    for (unsigned i = 0; i < Vkgc::ShaderStageGfxCount; i++) {
      m_uniformConstantMaps[i].getSubState(m_uniformMaps[i]);
      if (m_uniformMaps[i].numUniformConstants > 0) {
        pGlState->ppUniformMaps[pGlState->numUniformConstantMaps++] = &m_uniformMaps[i];
      }
    }

    pGlState->apiXfbOutData.forceDisableStreamOut = m_forceDisableStreamOut;
    if (m_xfbOutInfo.size() > 0) {
      pGlState->apiXfbOutData.numXfbOutInfo = static_cast<unsigned>(m_xfbOutInfo.size());
      m_xfbOutInfoData.resize(pGlState->apiXfbOutData.numXfbOutInfo);
      for (unsigned i = 0; i < pGlState->apiXfbOutData.numXfbOutInfo; ++i)
        m_xfbOutInfo[i].getSubState(m_xfbOutInfoData[i]);
      pGlState->apiXfbOutData.pXfbOutInfos = &m_xfbOutInfoData[0];
    }

    if (m_clientMetadataBufMem.size() > 0) {
      m_state.clientMetadataSize = m_clientMetadataBufMem.size();
      m_state.pClientMetadata = m_clientMetadataBufMem.data();
    }

    m_state.rsState.usrClipPlaneMask = m_usrClipPlaneMask;

    if ((m_tessLevelInner[0] != -1.0f) || (m_tessLevelOuter[0] != -1.0f)) {
      m_tessLevel.inner[0] = m_tessLevelInner[0];
      m_tessLevel.inner[1] = m_tessLevelInner[1];
      m_tessLevel.outer[0] = m_tessLevelOuter[0];
      m_tessLevel.outer[1] = m_tessLevelOuter[1];
      m_tessLevel.outer[2] = m_tessLevelOuter[2];
      m_tessLevel.outer[3] = m_tessLevelOuter[3];
      m_state.iaState.tessLevel = &m_tessLevel;
    }

    state = m_state;

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
    std::string dummySource;
    if (!m_shaderLibrary.empty()) {
      bool ret = readFile(docFilename, m_shaderLibrary, true, &m_shaderLibraryBytes, &dummySource, errorMsg);
      if (ret) {
        state.shaderLibrary.codeSize = m_shaderLibraryBytes.size();
        state.shaderLibrary.pCode = &m_shaderLibraryBytes[0];
      }
    }
#endif
    m_rtState.getSubState(docFilename, state.rtState, errorMsg);
  };
  SubState &getSubStateRef() { return m_state; };

private:
  SectionNggState m_nggState;
  SubState m_state;
  SectionColorBuffer m_colorBuffer[Vkgc::MaxColorTargets]; // Color buffer
  SectionPipelineOption m_options;
  Vkgc::UniformConstantMap *m_pUniformMaps[Vkgc::ShaderStageGfxCount];
  Vkgc::UniformConstantMap m_uniformMaps[Vkgc::ShaderStageGfxCount];
  SectionUniformConstantMap m_uniformConstantMaps[Vkgc::ShaderStageGfxCount];
  std::string m_shaderLibrary;
  std::vector<uint8_t> m_shaderLibraryBytes;
  std::vector<uint8_t> *m_clientMetadata;
  std::vector<uint8_t> m_clientMetadataBufMem;
  SectionRtState m_rtState;
  bool m_forceDisableStreamOut;
  float m_tessLevelInner[2];
  float m_tessLevelOuter[4];
  Vkgc::TessellationLevel m_tessLevel;
  std::vector<SectionXfbOutInfo> m_xfbOutInfo;
  std::vector<Vkgc::XfbOutInfo> m_xfbOutInfoData;
  unsigned m_usrClipPlaneMask;
  SectionAdvancedBlendInfo m_advancedBlendInfo;
};

// =====================================================================================================================
// Represents the section compute state
class SectionComputeState : public Section {
public:
  typedef Vkgc::ComputePipelineBuildInfo SubState;

  SectionComputeState() : Section(getAddrTable(), SectionTypeComputeState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
    m_clientMetadata = &m_clientMetadataBufMem;
  }

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, deviceIndex, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, unlinked, MemberTypeBool, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_options, MemberTypePipelineOption, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_shaderLibrary, MemberTypeString, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_rtState, MemberTypeRtState, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_clientMetadata, MemberTypeU8Array, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_uniformConstantMap, MemberTypeUniformConstantMap, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    m_options.getSubState(m_state.options);
    state = m_state;

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
    std::string dummySource;
    if (!m_shaderLibrary.empty()) {
      bool ret = readFile(docFilename, m_shaderLibrary, true, &m_shaderLibraryBytes, &dummySource, errorMsg);
      if (ret) {
        state.shaderLibrary.codeSize = m_shaderLibraryBytes.size();
        state.shaderLibrary.pCode = &m_shaderLibraryBytes[0];
      }
    }
#endif
    m_rtState.getSubState(docFilename, state.rtState, errorMsg);

    m_uniformConstantMap.getSubState(m_uniformMap);
    if (m_uniformMap.numUniformConstants > 0) {
      state.pUniformMap = &m_uniformMap;
    }

    if (m_clientMetadataBufMem.size() > 0) {
      state.clientMetadataSize = m_clientMetadataBufMem.size();
      state.pClientMetadata = m_clientMetadataBufMem.data();
    }
  }
  SubState &getSubStateRef() { return m_state; }

private:
  SubState m_state;
  SectionPipelineOption m_options;
  std::string m_shaderLibrary;
  std::vector<uint8_t> m_shaderLibraryBytes;
  SectionRtState m_rtState;
  Vkgc::UniformConstantMap m_uniformMap;
  SectionUniformConstantMap m_uniformConstantMap;
  std::vector<uint8_t> *m_clientMetadata;
  std::vector<uint8_t> m_clientMetadataBufMem;
};

// =====================================================================================================================
// Represents the section ray tracing state
class SectionRayTracingState : public Section {
public:
  typedef Vkgc::RayTracingPipelineBuildInfo SubState;

  SectionRayTracingState() : Section(getAddrTable(), SectionTypeComputeState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
    m_clientMetadata = &m_clientMetadataBufMem;
  }

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, deviceIndex, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, deviceCount, MemberTypeInt, false);

      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionRayTracingState, m_groups, MemberTypeShaderGroup, true);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, libraryMode, MemberTypeInt, false);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingState, m_shaderTraceRay, MemberTypeString, false);
#endif
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingState, m_options, MemberTypePipelineOption, true);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, maxRecursionDepth, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, indirectStageMask, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, mode, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingState, m_rtState, MemberTypeRtState, true);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, hasPipelineLibrary, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, pipelineLibStageMask, MemberTypeInt, false);

      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, payloadSizeMaxInLib, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, attributeSizeMaxInLib, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, isReplay, MemberTypeBool, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingState, m_clientMetadata, MemberTypeU8Array, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, cpsFlags, MemberTypeInt, false);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionRayTracingState, m_gpurtOptions, MemberTypeGpurtOption, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    m_options.getSubState(m_state.options);
    m_state.shaderGroupCount = static_cast<unsigned>(m_groups.size());
    m_vkShaderGroups.resize(m_state.shaderGroupCount);
    for (unsigned i = 0; i < m_state.shaderGroupCount; ++i)
      m_groups[i].getSubState(m_vkShaderGroups[i]);

    m_state.pShaderGroups = (m_state.shaderGroupCount) > 0 ? &m_vkShaderGroups[0] : nullptr;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
    std::string dummySource;
    if (!m_shaderTraceRay.empty()) {
      bool ret = readFile(docFilename, m_shaderTraceRay, true, &m_traceRayBinary, &dummySource, errorMsg);
      if (ret) {
        m_state.shaderTraceRay.codeSize = m_traceRayBinary.size();
        m_state.shaderTraceRay.pCode = &m_traceRayBinary[0];
      }
    }
#endif
    m_rtState.getSubState(docFilename, m_state.rtState, errorMsg);
    if (m_clientMetadataBufMem.size() > 0) {
      m_state.clientMetadataSize = m_clientMetadataBufMem.size();
      m_state.pClientMetadata = m_clientMetadataBufMem.data();
    }

    m_state.gpurtOptionCount = static_cast<unsigned>(m_gpurtOptions.size());
    m_vkgcGpurtOptions.resize(m_state.gpurtOptionCount);
    for (unsigned i = 0; i < m_state.gpurtOptionCount; ++i)
      m_gpurtOptions[i].getSubState(m_vkgcGpurtOptions[i]);
    m_state.pGpurtOptions = (m_state.gpurtOptionCount) > 0 ? m_vkgcGpurtOptions.data() : nullptr;

    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  SubState m_state;
  SectionPipelineOption m_options;
  SectionRtState m_rtState;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  std::string m_shaderTraceRay;
#endif
  std::vector<SectionShaderGroup> m_groups;
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> m_vkShaderGroups;
  std::vector<uint8_t> m_traceRayBinary;
  std::vector<uint8_t> *m_clientMetadata;
  std::vector<uint8_t> m_clientMetadataBufMem;
  std::vector<SectionGpurtOption> m_gpurtOptions;
  std::vector<Vkgc::GpurtOption> m_vkgcGpurtOptions;
};

} // namespace Vfx
