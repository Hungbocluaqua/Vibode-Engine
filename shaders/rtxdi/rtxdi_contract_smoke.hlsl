#include <Rtxdi/DI/Reservoir.hlsli>

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> outputBuffer;

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    RTXDI_DIReservoir candidate = RTXDI_EmptyDIReservoir();
    candidate.lightData = RTXDI_DIReservoir_LightValidBit;
    candidate.targetPdf = 1.0;
    candidate.weightSum = 1.0;
    candidate.M = 1.0;
    RTXDI_CombineDIReservoirs(reservoir, candidate, 0.5, 1.0);
    RTXDI_FinalizeResampling(reservoir, 1.0, max(reservoir.M, 1.0));
    outputBuffer[dispatchThreadId.x] = float4(
        reservoir.weightSum,
        reservoir.targetPdf,
        reservoir.M,
        RTXDI_IsValidDIReservoir(reservoir) ? 1.0 : 0.0);
}
