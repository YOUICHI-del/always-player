#include "AudioDeviceWatcher.h"
#include <propsys.h>
#include <algorithm>
#include <cwctype>
#include <cstdio>

#pragma comment(lib, "ole32.lib") // PropVariantClear / CoTaskMemFree

using Microsoft::WRL::ComPtr;

namespace {
    // ★ SDKヘッダのバージョン差で定義の有無が揺れるため、ここで直接定義する。
    //   PKEY_Device_FriendlyName   = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
    //   PKEY_Device_EnumeratorName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 24
    const PROPERTYKEY kKeyFriendlyName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    const PROPERTYKEY kKeyEnumeratorName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 24 };
    //   PKEY_AudioEndpoint_FormFactor = {1da5d803-d492-4edd-8c23-e0c0ffee7f0e}, 0 (VT_UI4)
    const PROPERTYKEY kKeyFormFactor = {
        { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };

    // EndpointFormFactor の値（mmdeviceapi.h の列挙と同じ）
    constexpr uint32_t kFfSpeakers          = 1;
    constexpr uint32_t kFfLineLevel         = 2;
    constexpr uint32_t kFfHeadphones        = 3;
    constexpr uint32_t kFfHeadset           = 5;
    constexpr uint32_t kFfDigitalPassthru   = 7;
    constexpr uint32_t kFfSpdif             = 8;
    constexpr uint32_t kFfDigitalDisplay    = 9;  // HDMI/DisplayPort
    constexpr uint32_t kFfUnknown           = 10;

    uint32_t ReadFormFactor(IPropertyStore* store) {
        uint32_t result = kFfUnknown;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(kKeyFormFactor, &pv)) && pv.vt == VT_UI4) {
            result = pv.ulVal;
        }
        PropVariantClear(&pv);
        return result;
    }

    std::wstring ReadStringProp(IPropertyStore* store, const PROPERTYKEY& key) {
        std::wstring result;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            result = pv.pwszVal;
        }
        PropVariantClear(&pv);
        return result;
    }

    std::wstring ToUpper(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        return s;
    }
}

AudioDeviceWatcher::AudioDeviceWatcher() = default;

AudioDeviceWatcher::~AudioDeviceWatcher() {
    Stop();
}

bool AudioDeviceWatcher::Start(Callback cb) {
    if (m_active) return true;
    m_callback = std::move(cb);

    // ★ Player::init()の時点では、まだこのスレッドのCOMが初期化されて
    //   いない場合がある（その場合CoCreateInstanceがCO_E_NOTINITIALIZEDで
    //   失敗し、通知が一切届かなくなる）。Qtのメインスレッドと同じSTAで
    //   初期化しておく。既に初期化済みならS_FALSE（参照カウントのみ加算）、
    //   別モードで初期化済みならRPC_E_CHANGED_MODE（そのまま使えるので問題なし）。
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(hrCo);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), &m_enumerator);
    if (FAILED(hr)) {
        wchar_t msg[160];
        swprintf_s(msg, L"[Device] AudioDeviceWatcher: CoCreateInstance failed hr=0x%08X (CoInit hr=0x%08X)\n",
                   static_cast<unsigned>(hr), static_cast<unsigned>(hrCo));
        OutputDebugStringW(msg);
        return false;
    }

    hr = m_enumerator->RegisterEndpointNotificationCallback(this);
    if (FAILED(hr)) {
        wchar_t msg[128];
        swprintf_s(msg, L"[Device] AudioDeviceWatcher: Register failed hr=0x%08X\n",
                   static_cast<unsigned>(hr));
        OutputDebugStringW(msg);
        m_enumerator.Reset();
        return false;
    }
    OutputDebugStringW(L"[Device] AudioDeviceWatcher: started\n");
    m_active = true;
    return true;
}

void AudioDeviceWatcher::Stop() {
    if (m_active) {
        m_active = false;
        if (m_enumerator) {
            m_enumerator->UnregisterEndpointNotificationCallback(this);
            m_enumerator.Reset();
        }
    }
    if (m_comInitialized) {
        m_comInitialized = false;
        CoUninitialize();
    }
}

AudioDeviceKind AudioDeviceWatcher::Classify(const std::wstring& enumeratorName,
                                             const std::wstring& friendlyName,
                                             uint32_t formFactor) {
    const std::wstring en = ToUpper(enumeratorName);
    const std::wstring fn = ToUpper(friendlyName);

    // Bluetooth：BTHENUM(A2DP) / BTHHFENUM(ハンズフリー) / BTHLE系。
    // ★ 「Alternative A2DP Driver」のようなサードパーティ製ドライバは
    //   列挙子名がBTH系にならない場合があるため、名前でも補助判定する。
    if (en.find(L"BTH") != std::wstring::npos ||
        fn.find(L"BLUETOOTH") != std::wstring::npos ||
        fn.find(L"A2DP") != std::wstring::npos) {
        return AudioDeviceKind::Bluetooth;
    }
    if (en == L"USB") {
        return AudioDeviceKind::UsbDac;
    }
    // HDAUDIO（Realtek等の内蔵コーデック）、INTELAUDIO（Intel SST）など。
    // 同じ内蔵コーデックでも、端子の種類（フォームファクタ）で区別する。
    switch (formFactor) {
        case kFfHeadphones:
        case kFfHeadset:
            return AudioDeviceKind::Internal;      // ヘッドフォン端子
        case kFfLineLevel:
        case kFfDigitalPassthru:
        case kFfSpdif:
        case kFfDigitalDisplay:
            return AudioDeviceKind::ExternalOut;   // ライン出力・光/同軸・HDMI
        default:
            // Speakers／不明は「内蔵スピーカー」扱い（＝自動再開しない安全側）
            return AudioDeviceKind::BuiltInSpeaker;
    }
}

bool AudioDeviceWatcher::QueryDefaultRenderDevice(AudioDeviceInfo& out, long* hrOut) {
    out = AudioDeviceInfo{};
    if (hrOut) *hrOut = 0;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), &enumerator);
    if (FAILED(hr)) { if (hrOut) *hrOut = hr; return false; }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) { if (hrOut) *hrOut = FAILED(hr) ? hr : E_POINTER; return false; }

    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        out.id = id;
        CoTaskMemFree(id);
    }

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
        out.friendlyName   = ReadStringProp(store.Get(), kKeyFriendlyName);
        out.enumeratorName = ReadStringProp(store.Get(), kKeyEnumeratorName);
        out.formFactor     = ReadFormFactor(store.Get());
    }
    out.kind = Classify(out.enumeratorName, out.friendlyName, out.formFactor);
    return true;
}

// ── IUnknown ─────────────────────────────────────────────
ULONG STDMETHODCALLTYPE AudioDeviceWatcher::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE AudioDeviceWatcher::Release() {
    // ★ 寿命はPlayerのメンバとして管理するため、参照カウント0でdeleteはしない。
    return --m_refCount;
}

HRESULT STDMETHODCALLTYPE AudioDeviceWatcher::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppv = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// ── IMMNotificationClient ────────────────────────────────
HRESULT STDMETHODCALLTYPE AudioDeviceWatcher::OnDefaultDeviceChanged(
    EDataFlow flow, ERole role, LPCWSTR defaultDeviceId)
{
    // ★ Windowsは1回の切り替えで eConsole / eMultimedia / eCommunications の
    //   3役割ぶん通知してくる。再生側の eConsole だけを拾って重複を防ぐ。
    if (flow != eRender || role != eConsole) return S_OK;
    if (!m_active || !m_callback) return S_OK;

    // COMスレッド上。コピーを渡すだけで、重い処理はしない。
    m_callback(defaultDeviceId ? std::wstring(defaultDeviceId) : std::wstring());
    return S_OK;
}
