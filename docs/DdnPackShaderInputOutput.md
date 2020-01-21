# Detailed Design Note: Pack fragment shader inputs
**Status:** revised
## 1 Instruction
Hardware allocates parameters in the parameter cache and attributes in LDS for interpolation in a pixel shader in units of vec4, regardless of how many components are used. Currently, the inputs of fragment shaders are treated as individual vec4s. If the input shader does not fully utilize all components -- perhaps after dead-code elimination -- then parameter and attribute space is wasted. By packing components tightly, we can reduce the cost of parameter cache, LDS and so on, which can benefit the performance.
This DDN introduces the workflow change of packing in/out in VS/FS pipeline in LLPC.
## 2 Background detail
VS/FS is the most popular pipeline. To reduce risk, we support packing for VS/FS pipeline as the goal of phase 1. Considering component-based interpolation of the input of fragment shader, we adopt the idea of vector scalarization and then re-assembling vectors to achieve the purpose of packing. We use cl::PackInOut as a global switch control.
In the LLPC middle-end, fragment shader inputs are represented by two kinds of intrinsic:
```
@llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
@llpc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 interpMode, T auxInterValue)
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
  - Dynamic indexing into inputs must be considered. The initial version of this feature can simply fall back to not packing anything when dynamic indexing is used.
### 4.1 Packing overview
We will have two core data structures for the packing.
1. The LocationMap answers the question "given an original (location, elemIdx) pair, what is the re-written (location, elemIdx)"? An instance of this data structure exists once per pipeline throughout the packing process.
2. The LocationSpans answers the question which contiguous sequences of components are used together, and in which interpMode. An instance of this data structure exists temporarily while computing the LocationMap.

With these data structures in place, the packing itself proceeds in three steps:
1. Compute the mapping.
   1. Collect the required information in a LocationSpans structure by iterating over relevant input/output calls.
   2. Iterate over the collected spans to produce the new mapping.
2.	Modify the input import calls in the fragment shader.
3.	Modify the (generic) output export calls in the previous shader stage.
4.	Add the mapping as a part of shader hash code.

                    VS Output                              FS Input
        vec3,vec4,float,i64vec3,vec2            vec3,vec4,float,i64vec3,vec2
                       |                                      |
                       |            (Scalarization)           |
            export.generic.*.f32x10                 import.generic.f32.*x8
            export.generic.*.i32x10                 import.generic.i32.*x6
                                                 import.interpolant.f32.*x2
                       |                                      |
                       |          (Lower gloabl pass)         |
                       ----------------------------------------
                                          |
                                Preparation for packing
                                          |
                       ----------------------------------------
                       |     (Patch resource collect pass)    |
                       |                                      |
            export.generic.*.v4f32------->0<-------import.generic.f32.*x4
            export.generic.*.v4f32------->1<-------import.generic.f32.*x4
            export.generic.*.v4f32------->2<-------import.generic.f32.*x4
            export.generic.*.v4f32------->3<-------import.generic.f32.*x4
            export.generic.*.v2f32------->4<-------import.generic.f32.*x2
            export.generic.*.v2f32------->5<-------import.interpolant.f32.*x4

### 4.2 LocationMap
The LocationMap deals in (location, component, half) tuples:
```cpp
// Represents the location info of input/output
union InOutLocationInfo
{
    struct
    {
        uint16_t location  : 13; // The location
        uint16_t component : 2;  // The component index
        uint16_t half      : 1;  // High half in case of 16-bit attriburtes
    };
    uint16_t u16All;
};
// Represents the wrapper of input/output locatoin info, along with handlers
struct InOutLocation
{
    uint16_t AsIndex() const { return locationInfo.u16All; }

    bool operator<(const InOutLocation& rhs) const { return (this->AsIndex() < rhs.AsIndex()); }

    InOutLocationInfo locationInfo; // The location info of an input or output
};
```
The interface of the LocationMap is:
```cpp
// Represents the manager of input/output locationMap generation
class InOutLocationMapManager
{
public:
    InOutLocationMapManager() {}

    bool AddSpan(CallInst* pCall);
    void BuildLocationMap();

    bool FindMap(const InOutLocation& originalLocation, const InOutLocation*& pNewLocation);

    struct LocationSpan
    {
        uint16_t GetCompatibilityKey() const { return compatibilityInfo.u16All; }

        uint32_t AsIndex() const { return ((GetCompatibilityKey() << 16) | firstLocation.AsIndex()); }

        bool operator==(const LocationSpan& rhs) const { return (this->AsIndex() == rhs.AsIndex()); }

        bool operator<(const LocationSpan& rhs) const { return (this->AsIndex() < rhs.AsIndex()); }

        InOutLocation firstLocation;
        InOutCompatibilityInfo compatibilityInfo;
    };

private:
    bool isCompatible(const LocationSpan& rSpan, const LocationSpan& lSpan) const
    {
        return rSpan.GetCompatibilityKey() == lSpan.GetCompatibilityKey();
    }

    LLPC_DISALLOW_COPY_AND_ASSIGN(InOutLocationMapManager);

    std::vector<LocationSpan> m_locationSpans; // Tracks spans of contiguous components in the generic input space
    std::map<InOutLocation, InOutLocation> m_locationMap; // The map between original location and new location
};
```
### 4.3 LocationSpans
The LocationSpans structure tracks spans (or intervals) of contiguous components in the generic input space that:
- Are used.
- Can be allocated to the same attribute vec4 (i.e., they are compatible on flat/non-flat, 16-bit/non-16-bit).
- Must be remapped contiguously and:
  - If the span is <= 4 components, cannot be moved to straddle a vec4 boundary.
  - If the span is > 4 components, can only be moved in a way that preserves the component and half parts of the triple.

Spans are added to the structure while iterating over existing input/output calls that reference generic fragment shader inputs. While doing so:
- Overlapping spans are merged. When doing so, they must be assertion-checked for compatibility.
- Merely adjacent spans are not merged.

Interface of LocationSpans:
```cpp
// Represents the compatibility info of input/output
union InOutCompatibilityInfo
{
    struct
    {
        uint16_t halfComponentCount : 9; // The number of components measured in times of 16-bits.
                                         // A single 32-bit component will be halfComponentCount=2
        uint16_t isFlat             : 1; // Flat shading or not
        uint16_t is16Bit            : 1; // Half float or not
        uint16_t isCustom           : 1; // Custom interpolation mode or not
    };
    uint16_t u16All;
};
struct LocationSpan
{
    uint16_t GetCompatibilityKey() const { return compatibilityInfo.u16All; }

    uint32_t AsIndex() const { return ((GetCompatibilityKey() << 16) | firstLocation.AsIndex()); }

    bool operator==(const LocationSpan& rhs) const { return (this->AsIndex() == rhs.AsIndex()); }

    bool operator<(const LocationSpan& rhs) const { return (this->AsIndex() < rhs.AsIndex()); }

    InOutLocation firstLocation;
    InOutCompatibilityInfo compatibilityInfo;
};
```
### 4.4 Scalarizaiton
Scalarization is done while lowering SPIR-V inputs of FS and outputs of the previous stage. This allows intermediate passes to clean up transparently, perform dead-code elimination, other cross-stage optimizations such as constant propagation, and so on.
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
