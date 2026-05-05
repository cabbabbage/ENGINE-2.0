RWByteAddressBuffer g_light_bins : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint linearIndex = dispatchThreadId.y * 1024u + dispatchThreadId.x;
    g_light_bins.Store(linearIndex * 4u, linearIndex);
}
