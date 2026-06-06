#include "PCMOutputPipe.h"
#include <QDebug>

PCMOutputPipe::PCMOutputPipe(RingBuffer *buffer, QObject *parent)
    : QThread(parent)
    , m_buffer(buffer)
    , m_pipeName(generatePipeName())
{
    qDebug() << "[PCMOutputPipe] pipe name:" << m_pipeName;
}

PCMOutputPipe::~PCMOutputPipe()
{
    stopOutput();
    wait();
}

void PCMOutputPipe::stopOutput()
{
    m_running.store(false);
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_hPipe);
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
}

bool PCMOutputPipe::sendWavHeader()
{
#pragma pack(push, 1)
    struct WavHeader {
        char     riff[4]       = {'R','I','F','F'};
        uint32_t riffSize      = 0xFFFFFFFF;  // 無限ストリーム
        char     wave[4]       = {'W','A','V','E'};
        char     fmt[4]        = {'f','m','t',' '};
        uint32_t fmtSize       = 16;
        uint16_t audioFormat   = 1;
        uint16_t channels      = 2;
        uint32_t sampleRate    = 44100;
        uint32_t byteRate      = 176400;
        uint16_t blockAlign    = 4;
        uint16_t bitsPerSample = 16;
        char     data[4]       = {'d','a','t','a'};
        uint32_t dataSize      = 0xFFFFFFFF;  // 無限ストリーム
    };
#pragma pack(pop)

    WavHeader hdr;
    DWORD written = 0;
    BOOL ok = WriteFile(m_hPipe, &hdr, sizeof(hdr), &written, nullptr);
    qDebug() << "[PCMOutputPipe] WAV header sent: ok=" << ok
             << "written=" << written << "err=" << (ok ? 0 : GetLastError());
    return ok && written == sizeof(hdr);
}

void PCMOutputPipe::run()
{
    QByteArray pipeNameBytes = m_pipeName.toLatin1();
    m_hPipe = CreateNamedPipeA(
        pipeNameBytes.constData(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,
        65536 * 4,
        65536,
        0,
        nullptr
    );

    if (m_hPipe == INVALID_HANDLE_VALUE) {
        emit pipeError(QString("Pipe作成失敗: %1").arg(GetLastError()));
        return;
    }

    qDebug() << "[PCMOutputPipe] Pipe created, waiting for mpv...";
    // ★ Pipe作成完了をMainWindowに通知→mpvがloadfileできる
    emit pipeCreated(m_pipeName);

    if (!ConnectNamedPipe(m_hPipe, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            emit pipeError(QString("mpv接続待ちエラー: %1").arg(err));
            CloseHandle(m_hPipe);
            m_hPipe = INVALID_HANDLE_VALUE;
            return;
        }
    }

    qDebug() << "[PCMOutputPipe] mpv connected";

    if (!sendWavHeader()) {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
        return;
    }

    // WAVヘッダ送信後にCDReader起動を通知
    emit pipeReady();
    m_running.store(true);
    qDebug() << "[PCMOutputPipe] streaming start";

    // ★ バッファ500ms分（37セクタ × 2352 = 87024バイト）
    constexpr size_t BUF_SIZE = 2352 * 37;
    uint8_t outBuf[BUF_SIZE];

    while (m_running.load()) {
        size_t avail = m_buffer->availableToRead();
        if (avail == 0) {
            msleep(2);
            continue;
        }

        size_t toRead = (avail < BUF_SIZE) ? avail : BUF_SIZE;
        toRead = (toRead / 2352) * 2352;
        if (toRead == 0) {
            msleep(2);
            continue;
        }

        size_t got = m_buffer->read(outBuf, toRead);
        if (got == 0) continue;

        DWORD written = 0;
        BOOL ok = WriteFile(m_hPipe, outBuf, static_cast<DWORD>(got), &written, nullptr);
        if (!ok) {
            qDebug() << "[PCMOutputPipe] WriteFile failed:" << GetLastError();
            break;
        }
    }

    DisconnectNamedPipe(m_hPipe);
    CloseHandle(m_hPipe);
    m_hPipe = INVALID_HANDLE_VALUE;
    qDebug() << "[PCMOutputPipe] Done.";
}
