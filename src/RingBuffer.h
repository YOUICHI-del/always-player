#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <condition_variable>

// ─────────────────────────────────────────────
// RingBuffer
// CD-DA PCMストリームを読み取りスレッドと
// 出力スレッドの間で受け渡す固定長リングバッファ
// 44.1kHz / 16bit / 2ch = 176,400 bytes/sec
// 8秒分 = 約1.4MB
// ─────────────────────────────────────────────
class RingBuffer {
public:
    explicit RingBuffer(size_t size)
        : m_buffer(size, 0)
        , m_capacity(size)
        , m_writePos(0)
        , m_readPos(0)
        , m_used(0)
    {}

    // データを書き込む（書けた分だけ返す）
    size_t write(const uint8_t *data, size_t bytes)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        size_t canWrite = m_capacity - m_used;
        size_t toWrite  = (bytes < canWrite) ? bytes : canWrite;
        if (toWrite == 0) return 0;

        size_t part1 = m_capacity - m_writePos;
        if (toWrite <= part1) {
            memcpy(m_buffer.data() + m_writePos, data, toWrite);
        } else {
            memcpy(m_buffer.data() + m_writePos, data, part1);
            memcpy(m_buffer.data(), data + part1, toWrite - part1);
        }
        m_writePos = (m_writePos + toWrite) % m_capacity;
        m_used += toWrite;
        m_cv.notify_one();
        return toWrite;
    }

    // データを読み出す（読めた分だけ返す）
    size_t read(uint8_t *out, size_t bytes)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        size_t toRead = (bytes < m_used) ? bytes : m_used;
        if (toRead == 0) return 0;

        size_t part1 = m_capacity - m_readPos;
        if (toRead <= part1) {
            memcpy(out, m_buffer.data() + m_readPos, toRead);
        } else {
            memcpy(out, m_buffer.data() + m_readPos, part1);
            memcpy(out + part1, m_buffer.data(), toRead - part1);
        }
        m_readPos = (m_readPos + toRead) % m_capacity;
        m_used -= toRead;
        return toRead;
    }

    // 読み出し可能バイト数
    size_t availableToRead() const
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_used;
    }

    // 書き込み可能バイト数
    size_t availableToWrite() const
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_capacity - m_used;
    }

    // バッファをリセット（シーク時に使用）
    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_writePos = 0;
        m_readPos  = 0;
        m_used     = 0;
    }

    // バッファが一定量溜まるまで待つ（再生開始前の待機）
    void waitUntilReady(size_t minBytes, int timeoutMs = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this, minBytes]{ return m_used >= minBytes; });
    }

private:
    std::vector<uint8_t>    m_buffer;
    size_t                  m_capacity;
    size_t                  m_writePos;
    size_t                  m_readPos;
    size_t                  m_used;
    mutable std::mutex      m_mtx;
    std::condition_variable m_cv;
};
