#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cwchar>
#include <vector>

// One requested frame from the genuine device, before discard-style Present.
// This does not show a window, synthesize pixels, or install a rendering hook.
class QuietFrameCapture {
public:
    enum Status : LONG { Idle, Pending, Capturing, Complete, Failed };
    bool Request(const wchar_t* path) {
        if (!path || !*path || wcslen(path) >= MAX_PATH) return false;
        AcquireSRWLockExclusive(&lock_);
        const bool available = status_ != Pending && status_ != Capturing;
        if (available) {
            wcscpy_s(path_, path);
            result_ = E_PENDING;
            status_ = Pending;
        }
        ReleaseSRWLockExclusive(&lock_);
        return available;
    }
    LONG State(HRESULT* result = nullptr) const {
        AcquireSRWLockShared(&lock_);
        const LONG state = status_;
        if (result) *result = result_;
        ReleaseSRWLockShared(&lock_);
        return state;
    }
    void BeforePresent(IDirect3DDevice9* device) {
        wchar_t path[MAX_PATH]{};
        AcquireSRWLockExclusive(&lock_);
        const bool requested = status_ == Pending;
        if (requested) { status_ = Capturing; wcscpy_s(path, path_); }
        ReleaseSRWLockExclusive(&lock_);
        if (!requested) return;
        HRESULT result = E_FAIL;
        try { result = Capture(device, path); }
        catch (...) { result = E_OUTOFMEMORY; }
        AcquireSRWLockExclusive(&lock_);
        result_ = result;
        status_ = SUCCEEDED(result) ? Complete : Failed;
        ReleaseSRWLockExclusive(&lock_);
    }

    static HRESULT WriteBitmap(const wchar_t* path, const D3DLOCKED_RECT& pixels,
                               const D3DSURFACE_DESC& desc) {
        const bool rgb565 = desc.Format == D3DFMT_R5G6B5;
        if (!pixels.pBits || !desc.Width || !desc.Height || desc.Width > 8192 || desc.Height > 8192 ||
            (!rgb565 && desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) ||
            pixels.Pitch < static_cast<INT>(desc.Width * (rgb565 ? 2 : 4))) return E_INVALIDARG;
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(desc.Width);
        info.biHeight = -static_cast<LONG>(desc.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = desc.Width * desc.Height * 4;
        std::vector<uint32_t> row(desc.Width);
        HANDLE output = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        auto write = [output](const void* data, DWORD bytes) {
            DWORD written = 0;
            return WriteFile(output, data, bytes, &written, nullptr) && written == bytes;
        };
        bool ok = write(&file, sizeof(file)) && write(&info, sizeof(info));
        for (UINT y = 0; ok && y < desc.Height; ++y) {
            const auto* source = static_cast<const uint8_t*>(pixels.pBits) + y * pixels.Pitch;
            for (UINT x = 0; x < desc.Width; ++x) {
                if (rgb565) {
                    const uint16_t value = reinterpret_cast<const uint16_t*>(source)[x];
                    const uint32_t r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
                    row[x] = 0xFF000000u | ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
                } else row[x] = reinterpret_cast<const uint32_t*>(source)[x] | 0xFF000000u;
            }
            ok = write(row.data(), desc.Width * 4);
        }
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(output);
        if (!ok) { DeleteFileW(path); return HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT); }
        return S_OK;
    }
private:
    struct Surface {
        IDirect3DSurface9* value = nullptr;
        ~Surface() { if (value) value->Release(); }
    };
    static HRESULT Capture(IDirect3DDevice9* device, const wchar_t* path) {
        if (!device) return E_POINTER;
        Surface back, resolved, memory;
        HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back.value);
        if (FAILED(hr)) return hr;
        D3DSURFACE_DESC desc{};
        if (FAILED(hr = back.value->GetDesc(&desc))) return hr;
        IDirect3DSurface9* source = back.value;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            if (FAILED(hr = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &resolved.value, nullptr))) return hr;
            if (FAILED(hr = device->StretchRect(back.value, nullptr, resolved.value, nullptr, D3DTEXF_NONE))) return hr;
            source = resolved.value;
        }
        if (FAILED(hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                D3DPOOL_SYSTEMMEM, &memory.value, nullptr))) return hr;
        if (FAILED(hr = device->GetRenderTargetData(source, memory.value))) return hr;
        D3DLOCKED_RECT pixels{};
        if (FAILED(hr = memory.value->LockRect(&pixels, nullptr, D3DLOCK_READONLY))) return hr;
        try { hr = WriteBitmap(path, pixels, desc); }
        catch (...) { memory.value->UnlockRect(); throw; }
        const HRESULT unlocked = memory.value->UnlockRect();
        return FAILED(hr) ? hr : unlocked;
    }
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    LONG status_ = Idle;
    HRESULT result_ = E_PENDING;
    wchar_t path_[MAX_PATH]{};
};
