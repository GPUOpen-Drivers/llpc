// This file is not a test, rather it was used to generate
// payload_caller_in_paq.ll and is kept so the .ll file can be re-generated.

struct [raypayload] MyPayload
{
    float  v1 : write(caller)     : read(caller);
    int    v2 : write(closesthit) : read(caller);
    double v3 : write(miss)       : read(caller);
};

RaytracingAccelerationStructure myAccelerationStructure : register(t3);
RWTexture2D<float4> gOutput : register(u0);

[shader("raygeneration")]
void RayGen()
{
    MyPayload payload;
    payload.v1 = 1.0;

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
        payload);

    gOutput[DispatchRaysIndex().xy] = float4(payload.v1, payload.v2, payload.v3, 0.);
}
