// This file is not a test itself, but used to generate the .ll test file.

struct [raypayload] OuterPayload
{
    // These are written in miss, so they are not saved before recursive
    // TraceRay in miss
    float   v1[15] : write(caller,miss)    : read(caller,miss);
    // These need to be saved before recursive TraceRay.
    // However, these are only partially in registers,
    // so are only saved partially. The memory part does not need
    // to be saved.
    float   v2[15] : write(caller)         : read(caller);
};

struct [raypayload] InnerPayload
{
    float  v1     : write(caller)          : read(caller);
};

RaytracingAccelerationStructure myAccelerationStructure : register(t3);
RWTexture2D<float4> gOutput : register(u0);

[shader("miss")]
void Miss(inout OuterPayload outerPayload)
{
    InnerPayload innerPayload;
    innerPayload.v1 = outerPayload.v1[14];

    RayDesc myRay = {
        float3(0., 0., 0.),
        0.,
        float3(0., 0., 0.),
        1.0};

    TraceRay(
        myAccelerationStructure,
        0,
        0,
        0,
        0,
        0,
        myRay,
        innerPayload);

    outerPayload.v1[14] = innerPayload.v1;
}

[shader("callable")]
void callable(inout OuterPayload outerPayload)
{
    CallShader(0, outerPayload);
}
