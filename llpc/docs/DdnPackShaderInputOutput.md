# Detailed Design Note: Pack fragment shader inputs
**Status:** revised
## 1 Instruction
Hardware allocates parameters in the parameter cache and attributes in LDS for interpolation in a pixel shader in units of vec4, regardless of how many components are used. Currently, the inputs of fragment shaders are treated as individual vec4s. If the input shader does not fully utilize all components -- perhaps after dead-code elimination -- then parameter and attribute space is wasted. By packing components tightly, we can reduce the cost of parameter cache, LDS and so on, which can benefit the performance.
This DDN introduces the workflow change of packing in/out in VS/FS pipeline in LLPC.
## 2 Background detail
VS/FS is the most popular pipeline. To reduce risk, we support packing for VS/FS pipeline as the goal of phase 1. Considering component-based interpolation of the input of fragment shader, we adopt the idea of vector scalarization and then re-assembling vectors to achieve the purpose of packing. We use cl::PackInOut as a global switch control.
In the LLPC middle-end, fragment shader inputs are represented by two kinds of intrinsic:
```
@lgc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
@lgc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 interpMode, T auxInterValue)
```
## 3 Interface change
No interface change.
## 4 Internal change
The core task of pixel shader input packing is to re-write the location and elemIdx fields of the fragment shader's input import functions to result in a denser packing.
Additionally:
- In addition to re-writing the input locations in the fragment shader, also the output locations of the previous shader stage need to be re-written to match.
- Input import and output export functions must be scalarized to make the packing more effective, for two reasons:
  - Scalarized imports make it easy to recognize when the original shader code has an input vector of which only one component is unused.
  - Scalarized imports allow a better packing: for example, two vec3s and one vec2 can be packed into two vec4s.
- Output exports must be re-vectorized for performance.
- Certain factors can limit the extent to which inputs can be packed:
  - Flat inputs cannot be packed together with non-flat inputs.
  - 16-bit inputs (less than 32-bit) cannot be packed together with 32-bit inputs (64-bit will be split into two 32-bits)
  - Custom interpolation mode inputs cannot be packed together with other interpolation modes inputs.
  - Dynamic indexing into inputs must be considered. The initial version of this feature can simply fall back to not packing anything when dynamic indexing is used.
### 4.1 Packing overview
We will have two core data structures for the packing.
1. The LocationMap answers the question "given an original (location, elemIdx) pair, what is the re-written (location, elemIdx)"? An instance of this data structure exists once per pipeline throughout the packing process.
2. The LocationSpans answers the question which contiguous sequences of components are used together, and in which interpMode. An instance of this data structure exists temporarily while computing the LocationMap.

With these data structures in place, the packing itself proceeds in middle-end in three steps:
1. Compute the mapping.
   - Collect the required information in a LocationSpans structure by iterating over relevant input/output calls.
   - Iterate over the collected spans to produce the new mapping and fill the InOutLocMap in the fragment shader.
2.	Modify the (generic) output export calls in the previous shader stage.
3.	Add the mapping as a part of shader hash code.

```
                                The workflow of input/output packing
Example:
layout(location = 0) in vec3 v1;
layout(location = 1) in float v2;
layout(location = 2) flat in int64_t v3;
layout(location = 3) flat in i16vec2 v4;
layout(location = 4) in float16_t v5;
layout(location = 5) in float16_t v6;
layout(location = 6) in float16_t v7;
layout(location = 7) in float16_t v8;

                    VS Output                              FS Input
                       |                                      |
                       |      (Patch resource collect pass)   |
                       |            (Scalarization)           |
            export.generic.*.f32x4                 import.generic.f32.*x4
            export.generic.*.i32x6                 import.interpolant.i32.*x6
            export.generic.*.f16x6                 import.interpolant.f16.*x6
                       |                                      |
                       |                             (Add locationSpans)
                       |                      (Build LocationMap from LocationSpans)
                       |                                      |
                       |                            (0,0,false) <-> (0,0,false)
                       |                            (0,1,false) <-> (0,1,false)
                       |                            (0,2,false) <-> (0,2,false)
                       |                            (1,0,false) <-> (0,3,false)
                                                    (4,0,false) <-> (1,0,false)
                       |                            (5,0,false) <-> (1,0,true)
                       |                            (6,0,false) <-> (1,1,false)
                       |                            (7,0,false) <-> (1,1,true)
                       |                            (2,0,false) <-> (2,0,false)
                       |                            (2,1,false) <-> (2,1,false)
                       |                            (3,0,false) <-> (2,2,false)
                       |                            (3,1,false) <-> (2,2,true)
                       |                                      |
                       |``````````````````````````````````````|
            (Reassemble output export)               (Fill InOutLocMap with AsIndex() of origin and new)
                       |                                      |
            export.generic.*.v4f32                            |
            export.generic.*.v4f32                            |
            export.generic.*.v4f32                            |
                       |                                      |
                       ----------------------------------------
                       |       (PatchInOutImportExport pass)  |
        exp.f32(i32 immarg 32, i32 immarg 15)       interp.p1.f16(,,,highHalf,)
        exp.f32(i32 immarg 33, i32 immarg 15)       interp.p2.f16(,,,highHalf,)
        exp.f32(i32 immarg 34, i32 immarg 15)       interp.mov.* (ubfe for 16-bit)
                       |                                      |
                       ----------------------------------------
                                                              |
                                                      BuildPsRegConfig()
                                                (set FP16_INTERP_MODE/ATTR0_VALID/ATTR0_VALID for non-flat 16-bit)

```
### 4.2 LocationMap
The LocationMap deals in (location, component, half) tuples:
```cpp
// in llpcResourceUsage.h
// Represents the location info of input/output
union InOutLocationInfo {
  struct {
    uint16_t half : 1;      // High half in case of 16-bit attributes
    uint16_t component : 2; // The component index
    uint16_t location : 13; // The location
  };
  uint16_t u16All;
};

// In llpcPatchResourceCollect.h
// Represents the wrapper of input/output location info, along with handlers
struct InOutLocation {
  uint16_t asIndex() const { return locationInfo.u16All; }

  bool operator<(const InOutLocation &rhs) const { return this->asIndex() < rhs.asIndex(); }

  InOutLocationInfo locationInfo; // The location info of an input or output
};
```
The interface of the LocationMap is:
```cpp
// Represents the manager of input/output locationMap generation
class InOutLocationMapManager {
public:
  InOutLocationMapManager() {}

  void addSpan(llvm::CallInst* call);
  void buildLocationMap();

  bool findMap(const InOutLocation &originalLocation, const InOutLocation *&newLocation);

  struct LocationSpan {
    uint16_t getCompatibilityKey() const { return compatibilityInfo.u16All; }

    unsigned asIndex() const { return ((getCompatibilityKey() << 16) | firstLocation.asIndex()); }

    bool operator==(const LocationSpan &rhs) const { return this->asIndex() == rhs.asIndex(); }

    bool operator<(const LocationSpan &rhs) const { return this->asIndex() < rhs.asIndex(); }

    InOutLocation firstLocation;
    InOutCompatibilityInfo compatibilityInfo;
  };

private:
  InOutLocationMapManager(const InOutLocationMapManager &) = delete;
  InOutLocationMapManager &operator=(const InOutLocationMapManager &) = delete;

  bool isCompatible(const LocationSpan &rSpan, const LocationSpan &lSpan) const {
    return rSpan.getCompatibilityKey() == lSpan.getCompatibilityKey();
  }

  std::vector<LocationSpan> m_locationSpans; // Tracks spans of contiguous components in the generic input space
  std::map<InOutLocation, InOutLocation> m_locationMap; // The map between original location and new location
};
```
### 4.3 LocationSpans
The LocationSpans structure tracks spans (or intervals) of contiguous components in the generic input space that:
- Are used.
- Can be allocated to the same attribute vec4 (i.e., they are have the same value of InOutCompatibilityInfo, When they are compatible on bitWidth, Smooth and linear interpolant mode can be packed together, flat and custom interpolant modes are packed separately).
- Must be remapped contiguously and:
  - If the span is <= 4 components, cannot be moved to straddle a vec4 boundary.
  - If the span is > 4 components, can only be moved in a way that preserves the component and half parts of the triple.

Spans are added to the structure while iterating over existing input/output calls that reference generic fragment shader inputs. While doing so:
- Overlapping spans are merged. When doing so, they must be assertion-checked for compatibility.
- Merely adjacent spans are not merged.

Interface of LocationSpans:
```cpp
// Represents the compatibility info of input/output
union InOutCompatibilityInfo {
  struct {
    uint16_t halfComponentCount : 9; // The number of components measured in times of 16-bits.
                                     // A single 32-bit component will be halfComponentCount=2
    uint16_t isFlat : 1;             // Flat shading or not
    uint16_t is16Bit : 1;            // 16-bit (i8/i16/f16, i8 is treated as 16-bit) or not
    uint16_t isCustom : 1;           // Custom interpolation mode or not
  };
  uint16_t u16All;
};
// Defined in InOutLocationMapManager
struct LocationSpan {
uint16_t getCompatibilityKey() const { return compatibilityInfo.u16All; }

unsigned asIndex() const { return ((getCompatibilityKey() << 16) | firstLocation.asIndex()); }

bool operator==(const LocationSpan &rhs) const { return this->asIndex() == rhs.asIndex(); }

bool operator<(const LocationSpan &rhs) const { return this->asIndex() < rhs.asIndex(); }

InOutLocation firstLocation;
InOutCompatibilityInfo compatibilityInfo;
};
```
### 4.4 Scalarization
Scalarization is done before processShader() in the resource collect pass. Hence, we need find unused elements by traversing each call users. In the future, we can fix this properly by doing the whole of generic input/output assignment later on in the middle-end, somewhere in the LLVM middle-end optimization pass flow.
### 4.5 Re-vectorization
Fragment shader input instructions do not benefit from re-vectorization.
Output instructions of the previous shader stage do benefit from re-vectorization, since export instructions are expensive. In general, we should strive to combine export instructions as much as possible. Furthermore, parameter exports should generally be done at the end of the hardware vertex or primitive shader stage.
Therefore, this part of the feature changes the PatchInOutImportExport to handle exporting of generic outputs from the last geometry stage differently as follows:
1. For every generic output component, insert an alloca instruction in the function entry block.
2. Lower all output.export.generic calls into stores to the corresponding alloca'd variable.
3. Build export intrinsics for all alloca'd variables before the function's return statement.

Note that:
- This procedure relies on having a single return statement in the function. This needs to be asserted and/or assured.
- Ensure that the mem2reg pass is run to cleanup the alloca instructions.

## 5 Implementation
### 5.1 Plan
Implementation have four phases in rough.
- Phase 1: Support packing in/out for XX-FS pipeline; testing may be restricted to VS-FS pipelines by disabling packing for others with a global switch (~24 weeks)
- Phase 2: Support packing half float for VS-FS pipeline (~4 weeks)
- Phase 3: Enable packing all XX-FS pipelines (~3 weeks)
### 5.2 Status
- Phase1 is completed with all tests passed.
- Phase2 is under review.
