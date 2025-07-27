// ws_big_reassembler.h  (C++17, ESP-IDF 5.x)
#pragma once
#include <map>
#include <vector>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_websocket_client.h>
#include "esphome/core/log.h"

class WsBigReassembler {
    static constexpr size_t npos = SIZE_MAX;
public:
    explicit WsBigReassembler(size_t maxBytes = 256*1024)  // Reduced from 1MB to 256KB
        : kMax(maxBytes)
    {
        buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(kMax, MALLOC_CAP_SPIRAM));
        assert(buf_ && "PSRAM alloc failed");
    }
    ~WsBigReassembler() { if (buf_) free(buf_); }

    bool add(const esp_websocket_event_data_t* e) {
        if (e->payload_offset + e->data_len > kMax) return abort();
        memcpy(buf_ + e->payload_offset, e->data_ptr, e->data_len);
        ranges_[e->payload_offset] = e->data_len;

        if (total_ == npos && e->payload_len) total_ = e->payload_len;
        if (e->fin) finSeen_ = true;
        return this->isReady();
    }

    // Helper methods for direct buffer access
    bool isReady() const { return finSeen_ && isContiguous(); }
    const uint8_t* getBuffer() const { return isReady() ? buf_ : nullptr; }
    size_t getSize() const { return isReady() ? total_ : 0; }
    void reset() { ranges_.clear(); total_ = npos; finSeen_ = false; }

private:
    bool isContiguous() const {
        size_t next = 0;
        for (auto& [off, len] : ranges_) {
            if (off != next) return false;
            next += len;
        }
        return total_ != npos && next == total_;
    }
    bool abort() { reset(); return false; }

    const size_t kMax;
    uint8_t* buf_;
    std::map<size_t,size_t> ranges_;
    size_t total_ = npos;
    bool finSeen_ = false;
};
