#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// ─────────────────────────────────────────────────────────
// AudioDeviceWatcher
//
// Windowsの「既定の再生デバイス」が切り替わったことを検知する
// （IMMNotificationClient）。Qtに依存しない純粋なWin32/COMクラス。
//
// ★ 目的：Bluetooth → USB DAC／内蔵スピーカー／ヘッドフォン端子へ
//   切り替わった瞬間、WASAPI排他（＝Windowsの音量シークバーが効かない
//   デジタル0dB出力）で爆音が出る事故を防ぐ。検知したらPlayer側で
//   即座に再生を止め、確認ダイアログ → OK → 同じ位置から再生再開する。
//
// ★ コールバックはWindowsのCOMスレッドから呼ばれる。呼び出し先では
//   重い処理やブロックをせず、UIスレッドへ転送するだけにすること。
// ─────────────────────────────────────────────────────────

enum class AudioDeviceKind {
    Unknown,
    UsbDac,         // USB接続（DAC側のボリュームで調整）
    Bluetooth,      // Bluetooth（共有・Windows音量連動。確認不要で再開）
    Internal,       // PCのヘッドフォン端子（確認ダイアログ → 再開）
    ExternalOut,    // PCのライン出力・光/同軸・HDMI（確認ダイアログ → 再開）
    BuiltInSpeaker, // 内蔵スピーカー（周囲に音が出るため自動再開しない）
};

struct AudioDeviceInfo {
    std::wstring    id;
    std::wstring    friendlyName;
    std::wstring    enumeratorName; // "USB" / "BTHENUM" / "HDAUDIO" など（判定の根拠）
    uint32_t        formFactor = 10; // EndpointFormFactor（1=Speakers, 3=Headphones 等）
    AudioDeviceKind kind = AudioDeviceKind::Unknown;
};

class AudioDeviceWatcher : public IMMNotificationClient {
public:
    // 引数は新しい既定デバイスのID（デバイスが1台も無くなった場合は空文字）
    using Callback = std::function<void(const std::wstring& newDefaultId)>;

    AudioDeviceWatcher();
    virtual ~AudioDeviceWatcher();

    // COM初期化済みのスレッド（QtのUIスレッド）から呼ぶこと。
    bool Start(Callback cb);
    void Stop();

    // 現在の既定再生デバイス（eRender/eConsole）の情報を取得する。
    // hrOut：失敗時の原因（診断ログ用。不要ならnullptr）
    static bool QueryDefaultRenderDevice(AudioDeviceInfo& out, long* hrOut = nullptr);
    static AudioDeviceKind Classify(const std::wstring& enumeratorName,
                                    const std::wstring& friendlyName,
                                    uint32_t formFactor);

    // ── IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;

    // ── IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR defaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> m_refCount{1};
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;
    Callback m_callback;
    std::atomic<bool> m_active{false};
    bool m_comInitialized = false; // Start()でCoInitializeExが成功した場合のみtrue
};
