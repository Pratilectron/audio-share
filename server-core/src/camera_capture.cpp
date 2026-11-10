/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>
*/

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {

bool encode_jpeg(const BYTE* bgr, UINT width, UINT height, UINT stride, std::vector<uint8_t>& jpeg)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat24bppBGR, stride, stride * height, const_cast<BYTE*>(bgr), &bitmap))) {
        return false;
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) {
        return false;
    }
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
        return false;
    }
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(width, height))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format))) {
        return false;
    }
    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }
    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &global)) || !global) {
        return false;
    }
    SIZE_T size = GlobalSize(global);
    void* data = GlobalLock(global);
    if (!data || size == 0) {
        return false;
    }
    jpeg.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    GlobalUnlock(global);
    return !jpeg.empty();
}

} // namespace

bool capture_camera_jpeg(std::vector<uint8_t>& jpeg, std::string& error)
{
    jpeg.clear();
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        error = "Media Foundation is unavailable";
        return false;
    }

    bool ok = false;
    ComPtr<IMFAttributes> attributes;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    do {
        if (FAILED(MFCreateAttributes(&attributes, 1))) {
            error = "cannot prepare the camera";
            break;
        }
        attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count)) || count == 0 || !devices) {
            error = "no camera";
            break;
        }
        ComPtr<IMFActivate> activate = devices[0];
        ComPtr<IMFMediaSource> source;
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&source)))) {
            error = "cannot open the camera";
            break;
        }
        ComPtr<IMFAttributes> reader_attrs;
        MFCreateAttributes(&reader_attrs, 1);
        reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), reader_attrs.Get(), &reader))) {
            error = "cannot read the camera";
            break;
        }
        ComPtr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()))) {
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);
            if (FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()))) {
                error = "camera cannot deliver RGB";
                break;
            }
        }
        ComPtr<IMFMediaType> current;
        if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current))) {
            error = "camera frame size is unknown";
            break;
        }
        UINT32 width = 0, height = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
        GUID subtype {};
        current->GetGUID(MF_MT_SUBTYPE, &subtype);
        const bool rgb32 = (subtype == MFVideoFormat_RGB32);
        const bool rgb24 = (subtype == MFVideoFormat_RGB24);
        if (width == 0 || height == 0 || (!rgb32 && !rgb24)) {
            error = "camera frame format is unsupported";
            break;
        }

        ComPtr<IMFSample> sample;
        DWORD stream = 0, flags = 0;
        LONGLONG stamp = 0;
        for (int attempt = 0; attempt < 60; ++attempt) {
            sample.Reset();
            if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &stamp, &sample))) {
                continue;
            }
            if (sample) {
                break;
            }
        }
        if (!sample) {
            error = "camera returned no frame";
            break;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            error = "cannot read the camera frame";
            break;
        }
        BYTE* data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        if (FAILED(buffer->Lock(&data, &max_len, &cur_len)) || !data || cur_len == 0) {
            error = "cannot lock the camera frame";
            break;
        }
        const UINT bytes_per_pixel = rgb32 ? 4u : 3u;
        const UINT stride = width * bytes_per_pixel;
        if (cur_len < stride * height) {
            buffer->Unlock();
            error = "camera frame is truncated";
            break;
        }
        if (rgb24) {
            ok = encode_jpeg(data, width, height, stride, jpeg);
        } else {
            std::vector<BYTE> bgr(static_cast<size_t>(width) * height * 3);
            for (UINT y = 0; y < height; ++y) {
                const BYTE* src = data + static_cast<size_t>(y) * stride;
                BYTE* dst = bgr.data() + static_cast<size_t>(y) * width * 3;
                for (UINT x = 0; x < width; ++x) {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
            }
            ok = encode_jpeg(bgr.data(), width, height, width * 3, jpeg);
        }
        buffer->Unlock();
        if (!ok) {
            error = "cannot encode the camera frame";
        }
    } while (false);

    if (devices) {
        for (UINT32 i = 0; i < count; ++i) {
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
    }
    MFShutdown();
    return ok;
}

#else

bool capture_camera_jpeg(std::vector<uint8_t>& jpeg, std::string& error)
{
    (void)jpeg;
    error = "unsupported";
    return false;
}

#endif
