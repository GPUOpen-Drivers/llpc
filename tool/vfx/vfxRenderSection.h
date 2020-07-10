#pragma once

namespace Vfx {
// =====================================================================================================================
// Represents the class that includes data to verify a test result.
class SectionResultItem : public Section {
public:
  typedef ResultItem SubState;

  SectionResultItem() : Section(m_addrTable, MemberCount, SectionTypeUnset, "ResultItem") {
    memset(&m_state, 0, sizeof(m_state));
    m_state.resultSource = ResultSourceMaxEnum;
    m_state.compareMethod = ResultCompareMethodEqual;
  };

  // Setup member name to member mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, resultSource, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, bufferBinding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, offset, MemberTypeIVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, iVec4Value, MemberTypeIVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, i64Vec2Value, MemberTypeI64Vec2, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, fVec4Value, MemberTypeFVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, f16Vec4Value, MemberTypeF16Vec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, dVec2Value, MemberTypeDVec2, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, compareMethod, MemberTypeEnum, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 9;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent the result section
class SectionResult : public Section {
public:
  typedef TestResult SubState;

  SectionResult() : Section(m_addrTable, MemberCount, SectionTypeResult, nullptr){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionResult, m_result, MemberTypeResultItem, MaxResultCount, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    state.numResult = 0;
    for (unsigned i = 0; i < MaxResultCount; ++i) {
      if (m_result[i].isActive())
        m_result[i].getSubState(state.result[state.numResult++]);
    }
  }

private:
  static const unsigned MemberCount = 1;
  static StrToMemberAddr m_addrTable[MemberCount];

  SectionResultItem m_result[MaxResultCount]; // section result items
};

// =====================================================================================================================
// Represents the class of vertex buffer binding.
class SectionVertexBufferBinding : public Section {
public:
  typedef VertrexBufferBinding SubState;

  SectionVertexBufferBinding() : Section(m_addrTable, MemberCount, SectionTypeUnset, "VertexBufferBinding") {
    m_state.binding = VfxInvalidValue;
    m_state.strideInBytes = VfxInvalidValue;
    m_state.stepRate = VK_VERTEX_INPUT_RATE_VERTEX;
  };

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, strideInBytes, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, stepRate, MemberTypeEnum, false);
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
// Represents the class of vertex attribute.
class SectionVertexAttribute : public Section {
public:
  typedef VertexAttribute SubState;

  SectionVertexAttribute() : Section(m_addrTable, MemberCount, SectionTypeUnset, "VertexAttribute") {
    m_state.binding = VfxInvalidValue;
    m_state.format = VK_FORMAT_UNDEFINED;
    m_state.location = VfxInvalidValue;
    m_state.offsetInBytes = VfxInvalidValue;
  };

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, format, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, location, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, offsetInBytes, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 4;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the class of vertex state
class SectionVertexState : public Section {
public:
  typedef VertexState SubState;

  SectionVertexState() : Section(m_addrTable, MemberCount, SectionTypeVertexState, nullptr){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionVertexState, m_vbBinding, MemberTypeVertexBufferBindingItem,
                                   MaxVertexBufferBindingCount, true);
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionVertexState, m_attribute, MemberTypeVertexAttributeItem,
                                   MaxVertexAttributeCount, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    state.numVbBinding = 0;
    for (unsigned i = 0; i < MaxVertexBufferBindingCount; ++i) {
      if (m_vbBinding[i].isActive())
        m_vbBinding[i].getSubState(state.vbBinding[state.numVbBinding++]);
    }

    state.numAttribute = 0;
    for (unsigned i = 0; i < MaxVertexAttributeCount; ++i) {
      if (m_attribute[i].isActive())
        m_attribute[i].getSubState(state.attribute[state.numAttribute++]);
    }
  }

private:
  static const unsigned MemberCount = 2;
  static StrToMemberAddr m_addrTable[MemberCount];
  SectionVertexBufferBinding m_vbBinding[MaxVertexBufferBindingCount]; // Binding info of all vertex buffers
  SectionVertexAttribute m_attribute[MaxVertexAttributeCount];         // Attribute info of all vertex attributes
};

// =====================================================================================================================
// Represents the class that includes data needed to create buffer and buffer view object.
class SectionBufferView : public Section {
public:
  typedef BufferView SubState;

  SectionBufferView()
      : Section(m_addrTable, MemberCount, SectionTypeBufferView, nullptr), m_intData(&m_bufMem), m_uintData(&m_bufMem),
        m_int64Data(&m_bufMem), m_uint64Data(&m_bufMem), m_floatData(&m_bufMem), m_doubleData(&m_bufMem),
        m_float16Data(&m_bufMem) {
    memset(&m_state, 0, sizeof(m_state));
    m_state.size = VfxInvalidValue;
    m_state.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  };

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, binding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, descriptorType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, size, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, format, MemberTypeEnum, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_intData, MemberTypeIArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_uintData, MemberTypeUArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_int64Data, MemberTypeI64Array, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_uint64Data, MemberTypeU64Array, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_floatData, MemberTypeFArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_doubleData, MemberTypeDArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_float16Data, MemberTypeF16Array, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    m_state.dataSize = static_cast<unsigned>(m_bufMem.size());
    m_state.pData = m_state.dataSize > 0 ? &m_bufMem[0] : nullptr;
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 11;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;                    // State of BufferView
  std::vector<uint8_t> m_bufMem;       // Underlying buffer for all data types.
  std::vector<uint8_t> *m_intData;     // Contains int data of this buffer
  std::vector<uint8_t> *m_uintData;    // Contains uint data of this buffer
  std::vector<uint8_t> *m_int64Data;   // Contains int64 data of this buffer
  std::vector<uint8_t> *m_uint64Data;  // Contains uint64 data of this buffer
  std::vector<uint8_t> *m_floatData;   // Contains float data of this buffer
  std::vector<uint8_t> *m_doubleData;  // Contains double data of this buffer
  std::vector<uint8_t> *m_float16Data; // Contains float16 data of this buffer
};

// =====================================================================================================================
// Represents the class of image/image view object
class SectionImageView : public Section {
public:
  typedef ImageView SubState;

  SectionImageView() : Section(m_addrTable, MemberCount, SectionTypeImageView, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
    m_state.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    m_state.viewType = VK_IMAGE_VIEW_TYPE_2D;
    m_state.dataPattern = ImageCheckBoxUnorm;
    m_state.samples = 1;
    m_state.mipmap = 0;
  }

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, binding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, descriptorType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, size, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, viewType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, dataPattern, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, samples, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, mipmap, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 7;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
};

// =====================================================================================================================
// Represents the class of sampler object
class SectionSampler : public Section {
public:
  typedef Sampler SubState;

  SectionSampler()
      : Section(m_addrTable, MemberCount, SectionTypeSampler, nullptr)

  {
    memset(&m_state, 0, sizeof(m_state));
    m_state.descriptorType = static_cast<VkDescriptorType>(VfxInvalidValue);
    m_state.dataPattern = static_cast<SamplerPattern>(VfxInvalidValue);
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, binding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, descriptorType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, dataPattern, MemberTypeEnum, false);
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
// Represents the class that includes data to represent one push constant range
class SectionPushConstRange : public Section {
public:
  typedef PushConstRange SubState;

  SectionPushConstRange()
      : Section(m_addrTable, MemberCount, SectionTypeUnset, "pushConstRange"), m_intData(&m_bufMem),
        m_uintData(&m_bufMem), m_floatData(&m_bufMem), m_doubleData(&m_bufMem){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPushConstRange, start, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPushConstRange, length, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_intData, MemberTypeIArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_uintData, MemberTypeUArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_floatData, MemberTypeFArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_doubleData, MemberTypeDArray, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    m_state.dataSize = static_cast<unsigned>(m_bufMem.size()) * sizeof(unsigned);
    m_state.pData = state.dataSize > 0 ? &m_bufMem[0] : nullptr;
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 6;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<unsigned> m_bufMem;      // Underlying buffer for all data types.
  std::vector<unsigned> *m_intData;    // Contains int data of this push constant range
  std::vector<unsigned> *m_uintData;   // Contains uint data of this push constant range
  std::vector<unsigned> *m_floatData;  // Contains float data of this push constant range
  std::vector<unsigned> *m_doubleData; // Contains double data of this push constant range
  SubState m_state;
};

// =====================================================================================================================
// Represents the class that includes data needed to create global draw state
class SectionDrawState : public Section {
public:
  typedef DrawState SubState;

  SectionDrawState()
      : Section(m_addrTable, MemberCount, SectionTypeDrawState, nullptr), m_vs("vs"), m_tcs("tcs"), m_tes("tes"),
        m_gs("gs"), m_fs("fs"), m_cs("cs") {
    initDrawState(m_state);
  }

  // Set initial value for DrawState.
  static void initDrawState(SubState &state) {
    state.dispatch.iVec4[0] = 1;
    state.dispatch.iVec4[1] = 1;
    state.dispatch.iVec4[2] = 1;
    state.viewport.iVec4[0] = 0;
    state.viewport.iVec4[1] = 0;
    state.viewport.iVec4[2] = 0;
    state.viewport.iVec4[3] = 0;
    state.instance = 1;
    state.vertex = 4;
    state.firstInstance = 0;
    state.firstVertex = 0;
    state.index = 6;
    state.firstIndex = 0;
    state.vertexOffset = 0;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    state.polygonMode = VK_POLYGON_MODE_FILL;
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.depthBiasEnable = false;
    state.width = 0;
    state.height = 0;
    state.lineWidth = 1.0f;
  }
  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, instance, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, vertex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstInstance, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstVertex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, index, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstIndex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, vertexOffset, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, topology, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, polygonMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, cullMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, frontFace, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, depthBiasEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, patchControlPoints, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, dispatch, MemberTypeIVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, width, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, height, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, lineWidth, MemberTypeFloat, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, viewport, MemberTypeIVec4, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_vs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_tcs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_tes, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_gs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_fs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_cs, MemberTypeSpecConst, true);
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionDrawState, m_pushConstRange, MemberTypePushConstRange, MaxPushConstRangCount,
                                   true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    state = m_state;
    m_vs.getSubState(state.vs);
    m_tcs.getSubState(state.tcs);
    m_tes.getSubState(state.tes);
    m_gs.getSubState(state.gs);
    m_fs.getSubState(state.fs);
    m_cs.getSubState(state.cs);
    state.numPushConstRange = 0;
    for (unsigned i = 0; i < MaxPushConstRangCount; ++i) {
      if (m_pushConstRange[i].isActive())
        m_pushConstRange[i].getSubState(state.pushConstRange[state.numPushConstRange++]);
    }
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 25;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
  SectionSpecConst m_vs;                                         // Vertex shader's spec constant
  SectionSpecConst m_tcs;                                        // Tessellation control shader's spec constant
  SectionSpecConst m_tes;                                        // Tessellation evaluation shader's spec constant
  SectionSpecConst m_gs;                                         // Geometry shader's spec constant
  SectionSpecConst m_fs;                                         // Fragment shader's spec constant
  SectionSpecConst m_cs;                                         // Compute shader shader's spec constant
  SectionPushConstRange m_pushConstRange[MaxPushConstRangCount]; // Pipeline push constant ranges
};

} // namespace Vfx
