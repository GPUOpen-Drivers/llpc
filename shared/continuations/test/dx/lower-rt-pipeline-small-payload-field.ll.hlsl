// This file is not a test itself, but used to generate the .ll test file.

struct [raypayload] Payload
{
    int v[5]                 : write(caller) : read(miss, caller);
    min16uint smallField     : write(miss)   : read(caller);
    min16uint3 smallFieldVec : write(miss)   : read(caller);
};

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.smallField = 17;
}
