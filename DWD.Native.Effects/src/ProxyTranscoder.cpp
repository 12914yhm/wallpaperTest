#include "ProxyTranscoder.h"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
    }
    return {};
}

HRESULT CreateMediaSource(const std::wstring& sourcePath, IMFMediaSource** source) {
    if (!source) return E_POINTER;
    *source = nullptr;
    ComPtr<IMFSourceResolver> resolver;
    ComPtr<IUnknown> unknown;
    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
    HRESULT hr = MFCreateSourceResolver(&resolver);
    if (SUCCEEDED(hr)) {
        hr = resolver->CreateObjectFromURL(sourcePath.c_str(), MF_RESOLUTION_MEDIASOURCE,
                                           nullptr, &objectType, &unknown);
    }
    if (SUCCEEDED(hr)) hr = unknown->QueryInterface(IID_PPV_ARGS(source));
    return hr;
}

HRESULT CreateProfile(UINT width, UINT height, UINT fps, UINT bitrate,
                      IMFTranscodeProfile** profile) {
    if (!profile) return E_POINTER;
    *profile = nullptr;
    ComPtr<IMFTranscodeProfile> result;
    ComPtr<IMFAttributes> video;
    ComPtr<IMFAttributes> container;
    HRESULT hr = MFCreateTranscodeProfile(&result);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&video, 7);
    if (SUCCEEDED(hr)) hr = video->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = video->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(video.Get(), MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(video.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(video.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = video->SetUINT32(MF_MT_INTERLACE_MODE,
                                             MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = video->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr)) hr = result->SetVideoAttributes(video.Get());

    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&container, 4);
    if (SUCCEEDED(hr)) {
        hr = container->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                                MFTranscodeContainerType_MPEG4);
    }
    if (SUCCEEDED(hr)) {
        // Transcode API 默认只选软件 MFT；允许硬件后仍会在不可用时自动回退软件编码。
        hr = container->SetUINT32(MF_TRANSCODE_TOPOLOGYMODE,
                                  MF_TRANSCODE_TOPOLOGYMODE_HARDWARE_ALLOWED);
    }
    if (SUCCEEDED(hr)) hr = container->SetUINT32(MF_TRANSCODE_SKIP_METADATA_TRANSFER, TRUE);
    if (SUCCEEDED(hr)) hr = result->SetContainerAttributes(container.Get());
    if (SUCCEEDED(hr)) *profile = result.Detach();
    return hr;
}

HRESULT RunEncodingSession(IMFTopology* topology, HANDLE stopEvent, HANDLE parentProcess) {
    ComPtr<IMFMediaSession> session;
    HRESULT hr = MFCreateMediaSession(nullptr, &session);
    if (SUCCEEDED(hr)) hr = session->SetTopology(0, topology);
    if (FAILED(hr)) return hr;

    HANDLE waits[2]{};
    DWORD waitCount{};
    if (stopEvent) waits[waitCount++] = stopEvent;
    if (parentProcess) waits[waitCount++] = parentProcess;
    bool started = false;
    bool encodingEnded = false;
    bool closing = false;
    HRESULT result = E_FAIL;

    while (true) {
        if (waitCount) {
            const DWORD wait = WaitForMultipleObjects(waitCount, waits, FALSE, 10);
            if (wait >= WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + waitCount) {
                result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                if (!closing) {
                    session->Stop();
                    session->Close();
                    closing = true;
                }
            }
        } else {
            Sleep(10);
        }

        ComPtr<IMFMediaEvent> event;
        hr = session->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) continue;
        if (FAILED(hr)) {
            result = hr;
            break;
        }

        MediaEventType type = MEUnknown;
        HRESULT eventStatus = S_OK;
        event->GetType(&type);
        event->GetStatus(&eventStatus);
        if (FAILED(eventStatus) && !closing) {
            result = eventStatus;
            session->Close();
            closing = true;
        }

        if (type == MESessionTopologyStatus && !started && !closing) {
            UINT32 topologyStatus{};
            if (SUCCEEDED(event->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &topologyStatus)) &&
                topologyStatus == MF_TOPOSTATUS_READY) {
                PROPVARIANT start;
                PropVariantInit(&start);
                hr = session->Start(&GUID_NULL, &start);
                PropVariantClear(&start);
                if (FAILED(hr)) {
                    result = hr;
                    session->Close();
                    closing = true;
                } else {
                    started = true;
                }
            }
        } else if (type == MESessionEnded && !closing) {
            encodingEnded = true;
            result = S_OK;
            session->Close();
            closing = true;
        } else if (type == MESessionClosed) {
            if (!encodingEnded && SUCCEEDED(result)) result = E_ABORT;
            break;
        }
    }

    session->Shutdown();
    return result;
}
}

int RunProxyTranscoder() {
    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring sourcePath = ArgValue(argc, argv, L"--source");
    const std::wstring outputPath = ArgValue(argc, argv, L"--output");
    const std::wstring widthText = ArgValue(argc, argv, L"--width");
    const std::wstring heightText = ArgValue(argc, argv, L"--height");
    const std::wstring fpsText = ArgValue(argc, argv, L"--fps");
    const std::wstring bitrateText = ArgValue(argc, argv, L"--bitrate");
    const std::wstring stopName = ArgValue(argc, argv, L"--stop-event");
    const std::wstring parentText = ArgValue(argc, argv, L"--parent-pid");
    if (argv) LocalFree(argv);

    const UINT width = static_cast<UINT>(_wtoi(widthText.c_str()));
    const UINT height = static_cast<UINT>(_wtoi(heightText.c_str()));
    const UINT fps = static_cast<UINT>(_wtoi(fpsText.c_str()));
    const UINT bitrate = static_cast<UINT>(_wtoi(bitrateText.c_str()));
    if (sourcePath.empty() || outputPath.empty() || width < 320 || height < 180 ||
        (width & 1) || (height & 1) || fps == 0 || bitrate < 500'000) {
        return 2;
    }

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    HANDLE stopEvent = stopName.empty() ? nullptr : OpenEventW(SYNCHRONIZE, FALSE,
                                                                stopName.c_str());
    HANDLE parentProcess = parentText.empty() ? nullptr :
        OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(_wtoi(parentText.c_str())));

    DeleteFileW(outputPath.c_str());
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comReady = SUCCEEDED(hr);
    if (comReady) hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    const bool mfReady = SUCCEEDED(hr);

    ComPtr<IMFMediaSource> source;
    ComPtr<IMFTranscodeProfile> profile;
    ComPtr<IMFTopology> topology;
    if (SUCCEEDED(hr)) hr = CreateMediaSource(sourcePath, &source);
    if (SUCCEEDED(hr)) hr = CreateProfile(width, height, fps, bitrate, &profile);
    if (SUCCEEDED(hr)) {
        hr = MFCreateTranscodeTopology(source.Get(), outputPath.c_str(), profile.Get(), &topology);
    }
    if (SUCCEEDED(hr)) hr = RunEncodingSession(topology.Get(), stopEvent, parentProcess);

    if (source) source->Shutdown();
    topology.Reset();
    profile.Reset();
    source.Reset();
    if (mfReady) MFShutdown();
    if (comReady) CoUninitialize();
    if (stopEvent) CloseHandle(stopEvent);
    if (parentProcess) CloseHandle(parentProcess);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);

    if (FAILED(hr)) {
        DeleteFileW(outputPath.c_str());
        return 10;
    }
    return 0;
}
