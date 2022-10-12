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
// Represents the sub section shader option
class SectionShaderOption : public Section {
public:
  typedef Vkgc::PipelineShaderOptions SubState;

  SectionShaderOption() : Section(getAddrTable(), SectionTypeUnset, "options") { memset(&m_state, 0, sizeof(m_state)); }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
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
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, ldsSpillLimitDwords, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, scalarizeWaterfallLoads, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, overrideShaderThreadGroupSizeX, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, overrideShaderThreadGroupSizeY, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, overrideShaderThreadGroupSizeZ, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, nsaThreshold, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, aggressiveInvariantLoads, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableInvariantLoads, MemberTypeBool, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
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
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, reconfigWorkgroupLayout, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, forceCsThreadIdSwizzling, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, overrideThreadGroupSizeX, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, overrideThreadGroupSizeY, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, overrideThreadGroupSizeZ, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTableUsage, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTablePtrHigh, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, resourceLayoutScheme, MemberTypeEnum, false);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 53
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, optimizationLevel, MemberTypeInt, false);
#endif
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, threadGroupSwizzleMode, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, reverseThreadGroup, MemberTypeBool, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionPipelineOption, m_extendedRobustness, MemberTypeExtendedRobustness, true);
      // One internal member
#if VKI_RAY_TRACING
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, internalRtShaders, MemberTypeBool, false);
#endif
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
  SectionExtendedRobustness m_extendedRobustness;
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
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

#if VKI_RAY_TRACING
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
    constexpr size_t ExportConfigOffset = offsetof(SectionIndirectCalleeSavedRegs, m_state);
    return reinterpret_cast<uint8_t *>(pObj) + ExportConfigOffset + Offset;
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

#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 15
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
#endif

// =====================================================================================================================
// Represents the sub section RtState state
class SectionRtState : public Section {
public:
  typedef Vkgc::RtState SubState;
  SectionRtState() : Section(getAddrTable(), SectionTypeUnset, "rtState") { memset(&m_state, 0, sizeof(m_state)); }

  void getSubState(SubState &state) {
    state = m_state;
    state.bvhResDesc.dataSizeInDwords = m_bvhResDescSize;
    for (unsigned i = 0; i < m_bvhResDesc.size(); ++i)
      state.bvhResDesc.descriptorData[i] = m_bvhResDesc[i];
    m_exportConfig.getSubState(state.exportConfig);
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 15
    m_gpurtFuncTable.getSubState(state.gpurtFuncTable);
#endif
  }

  SubState &getSubStateRef() { return m_state; }

private:
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
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableDispatchRaysInnerSwizzle, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableDispatchRaysOuterSwizzle, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, forceInvalidAccelStruct, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableRayTracingCounters, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableOptimalLdsStackSizeForIndirect, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRtState, enableOptimalLdsStackSizeForUnified, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRtState, m_exportConfig, MemberTypeRayTracingShaderExportConfig, true);
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 15
      INIT_MEMBER_NAME_TO_ADDR(SectionRtState, m_gpurtFuncTable, MemberTypeGpurtFuncTable, true);
#endif
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
  SectionRayTracingShaderExportConfig m_exportConfig;
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 15
  SectionGpurtFuncTable m_gpurtFuncTable;
#endif
  unsigned m_bvhResDescSize;
  std::vector<unsigned> m_bvhResDesc;
};
#endif

// =====================================================================================================================
// Represents the section graphics state
class SectionGraphicsState : public Section {
public:
  typedef GraphicsPipelineState SubState;

  SectionGraphicsState() : Section(getAddrTable(), SectionTypeGraphicsState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
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

#if VKI_RAY_TRACING
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_shaderLibrary, MemberTypeString, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_rtState, MemberTypeRtState, true);
#endif
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    for (unsigned i = 0; i < Vkgc::MaxColorTargets; ++i)
      m_colorBuffer[i].getSubState(m_state.colorBuffer[i]);
    m_options.getSubState(m_state.options);
    m_nggState.getSubState(m_state.nggState);
    state = m_state;
#if VKI_RAY_TRACING
    std::string dummySource;
    if (!m_shaderLibrary.empty()) {
      bool ret = readFile(docFilename, m_shaderLibrary, true, &m_shaderLibraryBytes, &dummySource, errorMsg);
      if (ret) {
        state.shaderLibrary.codeSize = m_shaderLibraryBytes.size();
        state.shaderLibrary.pCode = &m_shaderLibraryBytes[0];
      }
      m_rtState.getSubState(state.rtState);
    }
#endif
  };
  SubState &getSubStateRef() { return m_state; };

private:
  SectionNggState m_nggState;
  SubState m_state;
  SectionColorBuffer m_colorBuffer[Vkgc::MaxColorTargets]; // Color buffer
  SectionPipelineOption m_options;
#if VKI_RAY_TRACING
  std::string m_shaderLibrary;
  std::vector<uint8_t> m_shaderLibraryBytes;
  SectionRtState m_rtState;
#endif
};

// =====================================================================================================================
// Represents the section compute state
class SectionComputeState : public Section {
public:
  typedef ComputePipelineState SubState;

  SectionComputeState() : Section(getAddrTable(), SectionTypeComputeState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, deviceIndex, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_options, MemberTypePipelineOption, true);
#if VKI_RAY_TRACING
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_shaderLibrary, MemberTypeString, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_rtState, MemberTypeRtState, true);
#endif
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    m_options.getSubState(m_state.options);
    state = m_state;
#if VKI_RAY_TRACING
    std::string dummySource;
    if (!m_shaderLibrary.empty()) {
      bool ret = readFile(docFilename, m_shaderLibrary, true, &m_shaderLibraryBytes, &dummySource, errorMsg);
      if (ret) {
        state.shaderLibrary.codeSize = m_shaderLibraryBytes.size();
        state.shaderLibrary.pCode = &m_shaderLibraryBytes[0];
      }
      m_rtState.getSubState(state.rtState);
    }
#endif
  };
  SubState &getSubStateRef() { return m_state; };

private:
  SubState m_state;
  SectionPipelineOption m_options;
#if VKI_RAY_TRACING
  std::string m_shaderLibrary;
  std::vector<uint8_t> m_shaderLibraryBytes;
  SectionRtState m_rtState;
#endif
};

#if VKI_RAY_TRACING
// =====================================================================================================================
// Represents the section ray tracing state
class SectionRayTracingState : public Section {
public:
  typedef RayTracingPipelineState SubState;

  SectionRayTracingState() : Section(getAddrTable(), SectionTypeComputeState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, deviceIndex, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingState, m_options, MemberTypePipelineOption, true);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionRayTracingState, m_groups, MemberTypeShaderGroup, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingState, m_shaderTraceRay, MemberTypeString, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, maxRecursionDepth, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, indirectStageMask, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionRayTracingState, m_rtState, MemberTypeRtState, true);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, payloadSizeMaxInLib, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, attributeSizeMaxInLib, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, hasPipelineLibrary, MemberTypeBool, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionRayTracingState, pipelineLibStageMask, MemberTypeInt, false);
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
    std::string dummySource;
    if (!m_shaderTraceRay.empty()) {
      bool ret = readFile(docFilename, m_shaderTraceRay, true, &m_traceRayBinary, &dummySource, errorMsg);
      if (ret) {
        m_state.shaderTraceRay.codeSize = m_traceRayBinary.size();
        m_state.shaderTraceRay.pCode = &m_traceRayBinary[0];
      }
    }
    m_rtState.getSubState(m_state.rtState);
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  SubState m_state;
  SectionPipelineOption m_options;
  SectionRtState m_rtState;
  std::string m_shaderTraceRay;
  std::vector<SectionShaderGroup> m_groups;
  std::vector<VkRayTracingShaderGroupCreateInfoKHR> m_vkShaderGroups;
  std::vector<uint8_t> m_traceRayBinary;
};
#endif

} // namespace Vfx
