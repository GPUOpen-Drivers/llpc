# Graphics Register Handler

# Introduction
This framework is introduced to simpilfy the management of register values (which is fetched from SRD) and improve performance on maintaining expressions for these registers. Without this framework, the implementation may easily lead to hardly understandable generated code and introduce redundant operations.

# Background

Take `SQ_IMG_SAMP_WORD2` from `llpc/imported/chip/gfx9/gfx9_plus_merged_registers.h` for example:
```
union SQ_IMG_SAMP_WORD2 {
    struct {
        unsigned int LOD_BIAS                                                     : 14;
        unsigned int LOD_BIAS_SEC                                                 :  6;
        unsigned int XY_MAG_FILTER                                                :  2;
        unsigned int XY_MIN_FILTER                                                :  2;
        unsigned int Z_FILTER                                                     :  2;
        unsigned int MIP_FILTER                                                   :  2;
        unsigned int MIP_POINT_PRECLAMP                                           :  1;
        unsigned int BLEND_ZERO_PRT                                               :  1;
        unsigned int FILTER_PREC_FIX                                              :  1;
        unsigned int ANISO_OVERRIDE                                               :  1;
    } bits, bitfields;

    unsigned int u32All;
    signed int   i32All;
    float        f32All;
};
```
Note: `SQ_IMG_SAMP_WORDn` is stored in `vec <n x i32>`.

If we want to adjust `XY_MAG_FILTER` depending on some logical judgement at runtime, there's some fixed steps we need to follow:

`FUNCTION (1)`

1. Extract target element `i32` from `vec <n x i32>`
2. Use `amdgcn_ubfe` to extract specified range of bits (Occupied by `XY_MAG_FILTER`)
3. A series modification on the symbol of `XY_MAG_FILTER`
4. Apply the new symbolic expression of `XY_MAG_FILTER` to the target `i32`
5. Insert the target `i32` element back to `vec <n x i32>`

## Problems

* Modifying different bits of graphics register (DWORD) multiple times in a naive way may result in inefficient code with redundant instructions, e.g.:

        Call `FUNCTION (1)` for `LOD_BIAS`
        Call `FUNCTION (1)` for `XY_MAG_FILTER`

    Which results in:
    1. Extract target element `i32` from `vec <n x i32>`
    2. Use `amdgcn_ubfe` to extract specified range of bits (Occupied by `LOD_BIAS`)
    3. A series modification on the symbol of `LOD_BIAS`
    4. Apply the new symbolic expression of `LOD_BIAS` to the target `i32`
    5. Insert target `i32` element back to `vec <n x i32>`
    6. Extract target `i32` element from `vec <n x i32>`
    7. Use `amdgcn_ubfe` to extract specified range of bits (Occupied by `XY_MAG_FILTER`)
    8. A series modification to `XY_MAG_FILTER`
    9. Apply the new symbolic expression of `XY_MAG_FILTER` to the target `i32`
   10. Insert the target `i32` element back to `vec <n x i32>`

    But this series of manipulation could just be simplify to:
    1. Extract target element `i32` from `vec <n x i32>`
    2. Use `amdgcn_ubfe` to extract specified range of bits (Occupied by `LOD_BIAS`)
    3. A series modification on the symbol of `LOD_BIAS`
    4. Use `amdgcn_ubfe` to extract specified range of bits (Occupied by `XY_MAG_FILTER`)
    5. A series modification on the symbol of `XY_MAG_FILTER`
    6. Merge the new `LOD_BIAS` and `XY_MAG_FILTER` to the target `i32`
    7. Insert the target `i32` element back to `vec <n x i32>`

* How to avoid calling `amdgcn_ubfe` multiple times for the same range bits.

* If there's actually no modification of `XY_MAG_FILTER` applied to target `i32`, how could we prevent it from inserting the DWORDs back to vec `<n xi32>`.

* For the same register, it may locates differently among GfxIps. Such as `WIDTH`, it locates in `SQ_IMG_RSRC_WORD2` for gfx9, but may locates in `SQ_IMG_RSRC_WORD3` for gfx10. We need to manage these information properly.

# Implementation Overview

We introduce GfxRegHelper for efficiently handling register operations described above.

## Diagram
### Introduced structs:
```
struct BitsInfo
{
    uint32_t index;
    uint32_t offset;
    uint32_t count;
};
```
```
struct BitsState
{
    Value* pValue = nullptr;
    bool isModified = false;
};
```
SQ_IMG_SAMP register bits infomation look up table (Gfx9-10)
```
static constexpr BitsInfo g_sqImgSampRegBitsGfx9[SqSampRegsCount] =
{       // Which DWORD      Start bit        Count
    {            0,            30,             2 }, // FilterMode
    {            2,            20,             2 }, // XyMagFilter
    {            2,            22,             2 }, // XyMinFilter
};
```

### Phase 1 Bound Registers, BitsState and BitsInfo Look Up Table

* Bound `m_pRegister` to `SQ_IMG_SAMP <4 x i32>`
* Bound `g_sqImgSampRegBitsGfx9` to `m_bitsInfo`
* Initialize BitsState

#### Diagram 1
         ---------------------                                ------------------------------
        |SQ_IMG_SAMP:<4 x i32>|                              | BitsState[0]={nullptr, false}|
         ---------------------                               | BitsState[1]={nullptr, false}|
                         |                                   | BitsState[2]={nullptr, false}|
                         |  (Be bounded to m_pRegister)       ------------------------------
        GfxRegHelper:     ----------|                                      |
         ---------------------------|------------------------              |
        |  GfxRegHandlerBase:       |                        |             |
        |     ----------------------|-------------------     |             |
        |    |            ------------------            |    |             |
        |    |           |Value* m_pRegister|           |    |             |
        |    |            ------------------            |    |             |
        |    |                                          |    |             |
        |    |            ---------------------------   |    |             |
        |    |           |SQ_IMG_SAMP_WORD0 = nullptr|  |    |             |
        |    | m_dwords: |SQ_IMG_SAMP_WORD1 = nullptr|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD2 = nullptr|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD3 = nullptr|  |    |             |
        |    |            ---------------------------   |    |             |
        |    | m_dirtyDwords = 0000u                    |    |             |
        |     ------------------------------------------     |             |
        |                                                    |             |
        |  BitsState m_bitsState; <-----------------------------------------     -------------------------------------
        |                                                    |                  | g_sqImgSampRegBitsGfx9[0]={0, 30, 2}|
        |  const BitsInfo* m_bitsInfo; <--------------------------------------> | g_sqImgSampRegBitsGfx9[1]={2, 20, 2}|
        |                                                    |                  | g_sqImgSampRegBitsGfx9[2]={2, 22, 2}|
         ----------------------------------------------------                    -------------------------------------

### Phase 2 Get XyMagFilter Value

* Look up `XyMagFilter` bits information from table: `g_sqImgSampRegBitsGfx9[1]={2, 20, 2}`
* Extract the `BitsInfo.index`'th DWORD element to m_dwords, the index of which is `2` in this case
* Call `amdgcn_ubfe` with `BitsInfo.offset` and `BitsInfo.count`
* Cache the value of `XyMagFilter` to `BitsState[1].pValue`

#### Diagram 2
         ---------------------                                ------------------------------
        |SQ_IMG_SAMP:<4 x i32>|                              | BitsState[0]={nullptr, false}|
         ---------------------                               | BitsState[1]={0x45678, false}|
                         |                                   | BitsState[2]={nullptr, false}|
                         |  (Be bounded to m_pRegister)       ------------------------------
        GfxRegHelper:     ----------|                                      |
         ---------------------------|------------------------              |
        |  GfxRegHandlerBase:       |                        |             |
        |     ----------------------|-------------------     |             |
        |    |            ------------------            |    |             |
        |    |           |Value* m_pRegister|           |    |             |
        |    |            ------------------            |    |             |
        |    |                                          |    |             |
        |    |            ---------------------------   |    |             |
        |    |           |SQ_IMG_SAMP_WORD0 = nullptr|  |    |             |
        |    | m_dwords: |SQ_IMG_SAMP_WORD1 = nullptr|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD2 = 0x12345|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD3 = nullptr|  |    |             |
        |    |            ---------------------------   |    |             |
        |    | m_dirtyDwords = 0000u                    |    |             |
        |     ------------------------------------------     |             |
        |                                                    |             |
        |  BitsState m_bitsState; <-----------------------------------------     -------------------------------------
        |                                                    |                  | g_sqImgSampRegBitsGfx9[0]={0, 30, 2}|
        |  const BitsInfo* m_bitsInfo; <--------------------------------------> | g_sqImgSampRegBitsGfx9[1]={2, 20, 2}|
        |                                                    |                  | g_sqImgSampRegBitsGfx9[2]={2, 22, 2}|
         ----------------------------------------------------                    -------------------------------------

### Phase 3 Set XyMagFilter Value after Modification

* Set new `XyMagFilter`'s corresponding DWORD back to `SQ_IMG_SAMP_WORD2`, and set `.isModified` to be `true`
* Set `m_dirtyDwords` from `0000u` to `0100u`

#### Diagram 3
         ---------------------                                ------------------------------
        |SQ_IMG_SAMP:<4 x i32>|                              | BitsState[0]={nullptr, false}|
         ---------------------                               | BitsState[1]={0x45678,  true}|
                         |                                   | BitsState[2]={nullptr, false}|
                         |  (Be bounded to m_pRegister)       ------------------------------
        GfxRegHelper:     ----------|                                      |
         ---------------------------|------------------------              |
        |  GfxRegHandlerBase:       |                        |             |
        |     ----------------------|-------------------     |             |
        |    |            ------------------            |    |             |
        |    |           |Value* m_pRegister|           |    |             |
        |    |            ------------------            |    |             |
        |    |                                          |    |             |
        |    |            ---------------------------   |    |             |
        |    |           |SQ_IMG_SAMP_WORD0 = nullptr|  |    |             |
        |    | m_dwords: |SQ_IMG_SAMP_WORD1 = nullptr|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD2 = 0x54321|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD3 = nullptr|  |    |             |
        |    |            ---------------------------   |    |             |
        |    | m_dirtyDwords = 0100u                    |    |             |
        |     ------------------------------------------     |             |
        |                                                    |             |
        |  BitsState m_bitsState; <-----------------------------------------     -------------------------------------
        |                                                    |                  | g_sqImgSampRegBitsGfx9[0]={0, 30, 2}|
        |  const BitsInfo* m_bitsInfo; <--------------------------------------> | g_sqImgSampRegBitsGfx9[1]={2, 20, 2}|
        |                                                    |                  | g_sqImgSampRegBitsGfx9[2]={2, 22, 2}|
         ----------------------------------------------------                    -------------------------------------

### Phase 3-1 Dump new expression to m_dwords with new SQ_IMG_SAMP:<4 x i32>

* Only insert matched `i32` back to `<n x i32>` if the corresponding bits are marked up to `1` in m_dirtyDwords.

### Phase 3-2 If want to get XyMagFilter Value again for usage

* It will check `BitsState[1]={0x45678, true}`
    * if `.isModified == false`
        * Return `.pValue = 0x45678`
    * else
        * Call `amdgcn_ubfe` to get pValue to `BitsState[1]={0x87654, true}`
        * Set `.isModified` to `false`
        * Return `.pValue = 0x87654`

#### Diagram 3-2
         ---------------------                                ------------------------------
        |SQ_IMG_SAMP:<4 x i32>|                              | BitsState[0]={nullptr, false}|
         ---------------------                               | BitsState[1]={0x87654, false}|
                         |                                   | BitsState[2]={nullptr, false}|
                         |  (Be bounded to m_pRegister)       ------------------------------
        GfxRegHelper:     ----------|                                      |
         ---------------------------|------------------------              |
        |  GfxRegHandlerBase:       |                        |             |
        |     ----------------------|-------------------     |             |
        |    |            ------------------            |    |             |
        |    |           |Value* m_pRegister|           |    |             |
        |    |            ------------------            |    |             |
        |    |                                          |    |             |
        |    |            ---------------------------   |    |             |
        |    |           |SQ_IMG_SAMP_WORD0 = nullptr|  |    |             |
        |    | m_dwords: |SQ_IMG_SAMP_WORD1 = nullptr|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD2 = 0x54321|  |    |             |
        |    |           |SQ_IMG_SAMP_WORD3 = nullptr|  |    |             |
        |    |            ---------------------------   |    |             |
        |    | m_dirtyDwords = 0100u                    |    |             |
        |     ------------------------------------------     |             |
        |                                                    |             |
        |  BitsState m_bitsState; <-----------------------------------------     -------------------------------------
        |                                                    |                  | g_sqImgSampRegBitsGfx9[0]={0, 30, 2}|
        |  const BitsInfo* m_bitsInfo; <--------------------------------------> | g_sqImgSampRegBitsGfx9[1]={2, 20, 2}|
        |                                                    |                  | g_sqImgSampRegBitsGfx9[2]={2, 22, 2}|
         ----------------------------------------------------                    -------------------------------------

# Status
* The framework has been implemented. Please refer to:
    * `util/llpcGfxRegHandler.h/.cpp`
    * `util/llpcGfxRegHelper.h/.cpp`
* The supported graphics registers are currently be:
    * `SQ_IMG_SAMP_WORD`
    * `SQ_IMG_RSRC_WORD`
* The current supported GfxIps are:
    * `GFX9` and `GFX10`

## Next Step
* The BitsInfo should better be generated from `gfx9_plus_merged_registers.h` automatically.