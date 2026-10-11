/* Build adapter for the pinned MIT-licensed PortAudio WDM-KS driver.
 * The included source retains its original copyright/license notice.
 * This accessor avoids guessing a private struct layout from application C++.
 */
#include "pa_win_wdmks.c"

PaError Dashboard_KsDeviceIdentity(PaDeviceIndex index, wchar_t* identity, unsigned capacity)
{
    PaUtilHostApiRepresentation* api = NULL;
    PaDeviceIndex local = 0;
    PaWinWdmDeviceInfo* info;
    PaError result = PaUtil_GetHostApiRepresentation(&api, paWDMKS);
    if (result != paNoError) return result;
    result = PaUtil_DeviceIndexToHostApiDeviceIndex(&local, index, api);
    if (result != paNoError) return result;
    if (!identity || capacity < MAX_PATH + 64) return paBadBufferPtr;
    info = (PaWinWdmDeviceInfo*)api->deviceInfos[local];
    if (!info || !info->filter) return paInvalidDevice;
    _snwprintf(identity, capacity - 1, L"ks:%ls:pin=%lu:mux=%d",
        info->filter->devInfo.filterPath, info->pin, info->muxPosition);
    identity[capacity - 1] = 0;
    return paNoError;
}
