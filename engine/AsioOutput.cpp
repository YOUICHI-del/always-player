#include "AsioOutput.h"
#include "AsioConvert.h"
#include <windows.h>
#include <objbase.h>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

// ─────────────────────────────────────────────────────────
// ASIOコールバック（ドライバのスレッドから呼ばれる）。
// ASIOはコールバックにユーザーデータを渡せないので、今アクティブな
// AsioOutputを1つだけ記録しておき、そこへ転送する。
// ─────────────────────────────────────────────────────────
namespace {
    std::atomic<AsioOutput*> g_active{nullptr};

    void CbBufferSwitch(long index, ASIOBool /*directProcess*/) {
        if (AsioOutput* a = g_active.load(std::memory_order_acquire)) a->OnBufferSwitch(index);
    }
    ASIOTime* CbBufferSwitchTimeInfo(ASIOTime* /*params*/, long index, ASIOBool /*directProcess*/) {
        if (AsioOutput* a = g_active.load(std::memory_order_acquire)) a->OnBufferSwitch(index);
        return nullptr;
    }
    void CbSampleRateDidChange(ASIOSampleRate /*rate*/) {
        // レート変更はリセット要求と同様に扱う（開き直してもらう）
        if (AsioOutput* a = g_active.load(std::memory_order_acquire)) a->OnResetRequest();
    }
    long CbAsioMessage(long selector, long value, void* /*message*/, double* /*opt*/) {
        switch (selector) {
        case kAsioSelectorSupported:
            switch (value) {
            case kAsioResetRequest: case kAsioEngineVersion: case kAsioResyncRequest:
            case kAsioLatenciesChanged: case kAsioSupportsTimeInfo:
                return 1;
            default:
                return 0;
            }
        case kAsioEngineVersion:    return 2;
        case kAsioResetRequest:
            // ドライバの設定（バッファサイズ等）が変わった／デバイスが抜かれた等。
            // ここ（ドライバのスレッド）では何もせず、Player側に開き直してもらう。
            if (AsioOutput* a = g_active.load(std::memory_order_acquire)) a->OnResetRequest();
            return 1;
        case kAsioResyncRequest:    return 1;
        case kAsioLatenciesChanged: return 1;
        case kAsioSupportsTimeInfo: return 1;   // bufferSwitchTimeInfoを使ってよい
        case kAsioSupportsTimeCode: return 0;
        default:                    return 0;
        }
    }
    ASIOCallbacks g_callbacks = {
        CbBufferSwitch, CbSampleRateDidChange, CbAsioMessage, CbBufferSwitchTimeInfo
    };

    // HKLM\SOFTWARE\ASIO\<name> の CLSID を取得
    bool LookupClsid(const std::wstring& name, CLSID& clsid) {
        const std::wstring keyPath = L"SOFTWARE\\ASIO\\" + name;
        wchar_t buf[128] = {};
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath.c_str(), L"CLSID", RRF_RT_REG_SZ,
                         nullptr, buf, &size) != ERROR_SUCCESS)
            return false;
        return SUCCEEDED(CLSIDFromString(buf, &clsid));
    }
}

AsioOutput::AsioOutput() = default;

AsioOutput::~AsioOutput() {
    Unload();
    if (m_comInitialized) CoUninitialize();
}

std::vector<std::wstring> AsioOutput::EnumerateDrivers() {
    std::vector<std::wstring> result;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return result;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD len = 256;
        if (RegEnumKeyExW(hKey, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        CLSID clsid;
        if (LookupClsid(name, clsid)) result.emplace_back(name);
    }
    RegCloseKey(hKey);
    return result;
}

void AsioOutput::SetDriverName(const std::wstring& name) {
    if (name != m_driverName) {
        Unload();
        m_driverName = name;
    }
}

bool AsioOutput::EnsureLoaded() {
    if (m_asio && m_loadedName == m_driverName) return true;
    Unload();
    m_lastError.clear();
    if (m_driverName.empty()) { m_lastError = "no ASIO driver selected"; return false; }

    // ASIOドライバはCOMのインプロセスサーバー。呼び出しスレッド（GUIスレッド）で
    // COMが未初期化なら初期化する（Qtが初期化済みならS_FALSE/RPC_E_CHANGED_MODEで何もしない）。
    if (!m_comInitialized) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comInitialized = (hr == S_OK || hr == S_FALSE);
    }

    CLSID clsid;
    if (!LookupClsid(m_driverName, clsid)) { m_lastError = "ASIO driver not registered"; return false; }

    // ★ v10: init()に渡すウィンドウハンドル。ドライバによってはホスト（このアプリ）自身の
    //   有効なウィンドウが必要（SDKの注意書きより）。まずメイン画面のハンドルで試し、
    //   断られたらデスクトップのハンドルでもう一度試す（そのたびにドライバを作り直す）。
    void* handles[2] = { m_sysHandle, GetDesktopWindow() };
    std::string lastMsg;
    for (void* hwnd : handles) {
        if (!hwnd) continue;
        // ASIOドライバはCLSIDをそのままインターフェースIDとしても使う（SDKの作法）
        IASIO* drv = nullptr;
        const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                            reinterpret_cast<void**>(&drv));
        if (FAILED(hr) || !drv) {
            char buf[64];
            sprintf_s(buf, "cannot load ASIO driver (hr=0x%08lX)", static_cast<unsigned long>(hr));
            m_lastError = buf;
            return false;
        }
        const bool okInit = drv->init(hwnd) != 0;
        Log(std::string("init(") + (hwnd == m_sysHandle ? "app-window" : "desktop") + ") -> " + (okInit ? "OK" : "FAILED"));
        if (okInit) {
            m_asio = drv;
            m_loadedName = m_driverName;
            return true;
        }
        char msg[128] = {};
        drv->getErrorMessage(msg);
        if (msg[0]) lastMsg = msg;
        drv->Release();
    }
    m_lastError = lastMsg.empty()
        ? "ASIO driver init failed (the device may be in use by another app, or needs to be reconnected)"
        : lastMsg;
    return false;
}

bool AsioOutput::SupportsNativeDsd() {
    // 曲ごとに問い合わせないよう、ドライバ名ごとに結果を覚えておく
    // （再生中＝バッファ作成済みの状態で問い合わせるのを避ける意味もある）
    if (!m_dsdCapName.empty() && m_dsdCapName == m_driverName) return m_dsdCap;
    if (!EnsureLoaded()) return false;
    ASIOIoFormat f{};
    f.FormatType = kASIODSDFormat;
    m_dsdCap = (m_asio->future(kAsioCanDoIoFormat, &f) == ASE_SUCCESS);
    m_dsdCapName = m_driverName;
    Log(std::string("native DSD supported: ") + (m_dsdCap ? "yes" : "no"));
    return m_dsdCap;
}

AudioBackendResult AsioOutput::Initialize(const AudioFormat& format) {
    DisposeBuffers();
    if (!EnsureLoaded()) return AudioBackendResult::DeviceNotFound;
    if (format.channels != 2) return AudioBackendResult::FormatNotSupported;

    // ★ v10: PCM／ネイティブDSDの切替（バッファ作成前＝"prepared"状態に入る前に行う必要がある）
    if (m_nativeDsd != m_driverInDsd) {
        ASIOIoFormat f{};
        f.FormatType = m_nativeDsd ? kASIODSDFormat : kASIOPCMFormat;
        const ASIOError e = m_asio->future(kAsioSetIoFormat, &f);
        Log(std::string("SetIoFormat(") + (m_nativeDsd ? "DSD" : "PCM") + ") -> " + std::to_string(static_cast<long>(e)));
        if (e != ASE_SUCCESS) {
            if (m_nativeDsd) return AudioBackendResult::FormatNotSupported;
            // PCMへ戻せない場合はドライバを読み込み直して初期状態（PCM）にする
            Unload();
            if (!EnsureLoaded()) return AudioBackendResult::DeviceNotFound;
        }
        m_driverInDsd = m_nativeDsd;
    }

    // サンプルレート（ネイティブDSDではDSDレート＝1bitサンプルのレート。上位からは/8で届く）
    const ASIOSampleRate rate = static_cast<ASIOSampleRate>(format.sampleRate) * (m_nativeDsd ? 8.0 : 1.0);
    if (m_asio->canSampleRate(rate) != ASE_OK) return AudioBackendResult::FormatNotSupported;
    ASIOSampleRate cur = 0;
    if (m_asio->getSampleRate(&cur) != ASE_OK || cur != rate) {
        if (m_asio->setSampleRate(rate) != ASE_OK) return AudioBackendResult::FormatNotSupported;
    }

    long numIn = 0, numOut = 0;
    if (m_asio->getChannels(&numIn, &numOut) != ASE_OK || numOut < 2)
        return AudioBackendResult::DeviceNotFound;

    // バッファサイズはドライバの推奨値（ユーザーはドライバの設定画面で変えられる）
    long minSize = 0, maxSize = 0, prefSize = 0, gran = 0;
    if (m_asio->getBufferSize(&minSize, &maxSize, &prefSize, &gran) != ASE_OK || prefSize <= 0)
        return AudioBackendResult::UnknownError;

    // 出力の形式（L/Rとも同じ前提。違えば非対応）
    ASIOChannelInfo ci[2] = {};
    for (int c = 0; c < 2; ++c) {
        ci[c].channel = c;
        ci[c].isInput = ASIOFalse;
        if (m_asio->getChannelInfo(&ci[c]) != ASE_OK) return AudioBackendResult::UnknownError;
    }
    if (ci[0].type != ci[1].type || !asioconv::Supported(ci[0].type))
        return AudioBackendResult::FormatNotSupported;
    // ネイティブDSDならDSD形式、PCMならPCM形式でなければならない
    if (asioconv::IsDsd(ci[0].type) != m_nativeDsd)
        return AudioBackendResult::FormatNotSupported;
    // DSDではバッファサイズは1bitサンプル数。上位は1バイト（8サンプル）単位で扱う
    if (m_nativeDsd && (prefSize % 8) != 0)
        return AudioBackendResult::FormatNotSupported;
    if (m_dop && !asioconv::DopSafe(ci[0].type))
        return AudioBackendResult::FormatNotSupported;
    m_sampleType = ci[0].type;
    m_validBits  = asioconv::ValidBits(m_sampleType);
    {
        char name[64] = {};
        m_asio->getDriverName(name);
        long inLat = 0, outLat = 0;
        m_asio->getLatencies(&inLat, &outLat);
        char buf[512];
        sprintf_s(buf, "INIT driver=\"%s\" ver=%ld rate=%u outCh=%ld buf(min/max/pref/gran)=%ld/%ld/%ld/%ld type=%ld ch0=\"%s\" ch1=\"%s\" active=%ld/%ld outLat=%ld dop=%d",
                  name, m_asio->getDriverVersion(), format.sampleRate, numOut, minSize, maxSize, prefSize, gran,
                  static_cast<long>(m_sampleType), ci[0].name, ci[1].name,
                  static_cast<long>(ci[0].isActive), static_cast<long>(ci[1].isActive), outLat, m_dop ? 1 : 0);
        Log(buf);
    }

    // バッファ作成（ここでアクティブなインスタンスとして登録）
    ASIOBufferInfo bi[2] = {};
    for (int c = 0; c < 2; ++c) { bi[c].isInput = ASIOFalse; bi[c].channelNum = c; }
    g_active.store(this, std::memory_order_release);
    if (m_asio->createBuffers(bi, 2, prefSize, &g_callbacks) != ASE_OK) {
        g_active.store(nullptr, std::memory_order_release);
        return AudioBackendResult::UnknownError;
    }
    m_buffersCreated = true;
    for (int c = 0; c < 2; ++c) {
        m_buffers[c][0] = bi[c].buffers[0];
        m_buffers[c][1] = bi[c].buffers[1];
    }
    m_bufferFrames = static_cast<uint32_t>(m_nativeDsd ? prefSize / 8 : prefSize);
    m_postOutput = (m_asio->outputReady() == ASE_OK);
    Log(std::string("createBuffers OK, outputReady=") + (m_postOutput ? "yes" : "no"));

    // リング：バッファ8個分の余裕（WASAPI側と同じ考え方）
    m_ringCapacity = static_cast<size_t>(m_bufferFrames) * 2 * 8 + 1;
    m_ring.assign(m_ringCapacity, 0);
    m_tmp.assign(static_cast<size_t>(m_bufferFrames) * 2, 0);
    m_writeIndex.store(0, std::memory_order_relaxed);
    m_readIndex.store(0, std::memory_order_relaxed);

    m_actualFormat.sampleRate = format.sampleRate;
    m_actualFormat.channels = 2;
    m_actualFormat.sampleFormat = (m_validBits <= 16) ? AudioSampleFormat::Int16
                                : (m_validBits <= 24) ? AudioSampleFormat::Int24
                                                      : AudioSampleFormat::Int32;
    return AudioBackendResult::Ok;
}

void AsioOutput::Log(const std::string& line) {
    if (m_logPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, m_logPath.c_str(), L"a") != 0 || !f) return;
    char ts[32] = {};
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    std::fprintf(f, "%s  %s\n", ts, line.c_str());
    std::fclose(f);
}

void AsioOutput::OnBufferSwitch(long index) {
    // ★ ドライバのスレッド。ロック・確保・I/Oはしない。
    m_callbackCount.fetch_add(1, std::memory_order_relaxed);
    if (!m_buffersCreated || index < 0 || index > 1) return;
    const long frames = static_cast<long>(m_bufferFrames);

    size_t got = 0;
    if (m_running.load(std::memory_order_acquire)) {
        const size_t r = m_readIndex.load(std::memory_order_relaxed);
        const size_t w = m_writeIndex.load(std::memory_order_acquire);
        size_t avail = (w + m_ringCapacity - r) % m_ringCapacity;
        avail -= avail % 2;                                   // フレーム単位
        const size_t want = static_cast<size_t>(frames) * 2;
        got = std::min(avail, want);
        const size_t first = std::min(got, m_ringCapacity - r);
        std::memcpy(m_tmp.data(), &m_ring[r], first * sizeof(int32_t));
        if (got > first) std::memcpy(m_tmp.data() + first, &m_ring[0], (got - first) * sizeof(int32_t));
        m_readIndex.store((r + got) % m_ringCapacity, std::memory_order_release);
    }
    // 足りない分は無音（アンダーラン時・停止中）
    if (got < m_tmp.size() && m_running.load(std::memory_order_relaxed))
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
    if (got < m_tmp.size())
        std::fill(m_tmp.begin() + static_cast<ptrdiff_t>(got), m_tmp.end(), 0);

    for (int c = 0; c < 2; ++c)
        asioconv::Convert(m_tmp.data(), c, frames, m_sampleType, m_buffers[c][index]);
    if (m_postOutput) m_asio->outputReady();

    // VUメーター（ASIOはバッファが短いので、このバッファのRMSをそのまま現在値にする）
    if (!m_dop && !m_nativeDsd && got > 0) {
        double sl = 0, sr = 0;
        constexpr double k = 1.0 / 2147483648.0;
        const size_t n = got / 2;
        for (size_t i = 0; i < n; ++i) {
            const double l = m_tmp[i * 2] * k, r = m_tmp[i * 2 + 1] * k;
            sl += l * l; sr += r * r;
        }
        m_levelL.store(static_cast<float>(std::sqrt(sl / n)), std::memory_order_relaxed);
        m_levelR.store(static_cast<float>(std::sqrt(sr / n)), std::memory_order_relaxed);
    } else {
        m_levelL.store(m_levelL.load(std::memory_order_relaxed) * 0.85f, std::memory_order_relaxed);
        m_levelR.store(m_levelR.load(std::memory_order_relaxed) * 0.85f, std::memory_order_relaxed);
    }
    if (got > 0) m_backpressureCv.notify_one();
}

AudioBackendResult AsioOutput::WriteFrames(const uint8_t* data, uint32_t frameCount) {
    // 呼び出し元はAudioProcessThread（1スレッドのみ）。左詰めint32 interleaved。
    const int32_t* in = reinterpret_cast<const int32_t*>(data);
    const size_t total = static_cast<size_t>(frameCount) * 2;
    size_t written = 0;
    std::chrono::steady_clock::time_point stallStart{};

    while (written < total) {
        const size_t w = m_writeIndex.load(std::memory_order_relaxed);
        const size_t r = m_readIndex.load(std::memory_order_acquire);
        const size_t freeSpace = (r + m_ringCapacity - w - 1) % m_ringCapacity;
        if (freeSpace == 0) {
            // 停止中は待たない。0.5秒進まなければ（デバイス喪失など）諦める。
            if (!m_running.load()) return AudioBackendResult::Ok;
            const auto now = std::chrono::steady_clock::now();
            if (stallStart == std::chrono::steady_clock::time_point{}) stallStart = now;
            else if (now - stallStart > std::chrono::milliseconds(500)) return AudioBackendResult::DeviceLost;
            std::unique_lock<std::mutex> lock(m_backpressureMutex);
            m_backpressureCv.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }
        stallStart = std::chrono::steady_clock::time_point{};
        const size_t chunk = std::min(total - written, freeSpace);
        const size_t first = std::min(chunk, m_ringCapacity - w);
        std::memcpy(&m_ring[w], in + written, first * sizeof(int32_t));
        if (chunk > first) std::memcpy(&m_ring[0], in + written + first, (chunk - first) * sizeof(int32_t));
        m_writeIndex.store((w + chunk) % m_ringCapacity, std::memory_order_release);
        written += chunk;
    }
    return AudioBackendResult::Ok;
}

uint32_t AsioOutput::GetQueuedFrames() const {
    if (m_ringCapacity == 0) return 0;
    const size_t w = m_writeIndex.load(std::memory_order_acquire);
    const size_t r = m_readIndex.load(std::memory_order_acquire);
    return static_cast<uint32_t>(((w + m_ringCapacity - r) % m_ringCapacity) / 2);
}

AudioBackendResult AsioOutput::Start() {
    if (!m_asio || !m_buffersCreated) return AudioBackendResult::UnknownError;
    if (m_running) return AudioBackendResult::Ok;
    m_callbackCount.store(0, std::memory_order_relaxed);
    m_underrunCount.store(0, std::memory_order_relaxed);
    m_running = true;
    const ASIOError e = m_asio->start();
    Log("start() -> " + std::to_string(static_cast<long>(e)));
    if (e != ASE_OK) {
        m_running = false;
        return AudioBackendResult::UnknownError;
    }
    return AudioBackendResult::Ok;
}

AudioBackendResult AsioOutput::Stop() {
    if (!m_running) return AudioBackendResult::Ok;
    m_running = false;
    if (m_asio) m_asio->stop();   // 戻った後はコールバックは来ない
    Log("stop: callbacks=" + std::to_string(m_callbackCount.load()) +
        " underruns=" + std::to_string(m_underrunCount.load()));
    m_backpressureCv.notify_all();
    m_writeIndex.store(0, std::memory_order_relaxed);
    m_readIndex.store(0, std::memory_order_relaxed);
    m_levelL.store(0.f, std::memory_order_relaxed);
    m_levelR.store(0.f, std::memory_order_relaxed);
    return AudioBackendResult::Ok;
}

void AsioOutput::DisposeBuffers() {
    Stop();
    if (m_asio && m_buffersCreated) m_asio->disposeBuffers();
    m_buffersCreated = false;
    AsioOutput* self = this;
    g_active.compare_exchange_strong(self, nullptr);
    for (auto& ch : m_buffers) ch[0] = ch[1] = nullptr;
}

void AsioOutput::Shutdown() {
    // ドライバは保持したまま（次の曲ですぐ開けるように）バッファだけ解放
    DisposeBuffers();
}

void AsioOutput::Unload() {
    DisposeBuffers();
    if (m_asio) { m_asio->Release(); m_asio = nullptr; }
    m_loadedName.clear();
    m_driverInDsd = false;   // 読み込み直したドライバはPCMモードから始まる
}

bool AsioOutput::ControlPanel() {
    if (!EnsureLoaded()) return false;
    return m_asio->controlPanel() == ASE_OK;
}

void AsioOutput::SetErrorCallback(ErrorCallback callback, void* userData) {
    m_errorCallback = callback;
    m_errorCallbackUserData = userData;
}
