#pragma once
#include <vector>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <algorithm>

// 単一生産者・単一消費者用ロックフリーリングバッファ（s32 PCM専用）。
// interleaved(L,R,L,R,...)のまま扱う。float変換を挟まないため
// ビットパーフェクト経路として最短のパイプラインになる。
//
// クラス名をAudioRingBufferとしているのは、既存のsrc/RingBuffer.h
// （CD/VUメーター用、別実装）とのクラス名衝突を避けるため。
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacitySamples)
        : m_capacity(capacitySamples + 1)
        , m_buffer(m_capacity)
        , m_head(0)
        , m_tail(0) {
    }

    size_t Write(const int32_t* samples, size_t count) {
        size_t written = 0;
        while (written < count) {
            size_t head = m_head.load(std::memory_order_relaxed);
            size_t tail = m_tail.load(std::memory_order_acquire);
            size_t freeSpace = (tail + m_capacity - head - 1) % m_capacity;
            if (freeSpace == 0) break;

            size_t toWrite = std::min(count - written, freeSpace);
            size_t firstChunk = std::min(toWrite, m_capacity - head);
            memcpy(&m_buffer[head], samples + written, firstChunk * sizeof(int32_t));
            if (toWrite > firstChunk) {
                memcpy(&m_buffer[0], samples + written + firstChunk,
                       (toWrite - firstChunk) * sizeof(int32_t));
            }
            m_head.store((head + toWrite) % m_capacity, std::memory_order_release);
            written += toWrite;
        }
        return written;
    }

    size_t Read(int32_t* outSamples, size_t count) {
        size_t readCount = 0;
        while (readCount < count) {
            size_t tail = m_tail.load(std::memory_order_relaxed);
            size_t head = m_head.load(std::memory_order_acquire);
            size_t available = (head + m_capacity - tail) % m_capacity;
            if (available == 0) break;

            size_t toRead = std::min(count - readCount, available);
            size_t firstChunk = std::min(toRead, m_capacity - tail);
            memcpy(outSamples + readCount, &m_buffer[tail], firstChunk * sizeof(int32_t));
            if (toRead > firstChunk) {
                memcpy(outSamples + readCount + firstChunk, &m_buffer[0],
                       (toRead - firstChunk) * sizeof(int32_t));
            }
            m_tail.store((tail + toRead) % m_capacity, std::memory_order_release);
            readCount += toRead;
        }
        return readCount;
    }

    size_t AvailableToRead() const {
        size_t head = m_head.load(std::memory_order_acquire);
        size_t tail = m_tail.load(std::memory_order_relaxed);
        return (head + m_capacity - tail) % m_capacity;
    }

    void Clear() {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

private:
    size_t m_capacity;
    std::vector<int32_t> m_buffer;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};
