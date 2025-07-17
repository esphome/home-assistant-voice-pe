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
    explicit WsBigReassembler(size_t maxBytes = 1*1024*1024)
        : kMax(maxBytes)
    {
        buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(kMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        assert(buf_ && "PSRAM alloc failed");
    }
    ~WsBigReassembler() { if (buf_) free(buf_); }

    bool add(const esp_websocket_event_data_t* e) {
        if (e->payload_offset + e->data_len > kMax) return abort();
        memcpy(buf_ + e->payload_offset, e->data_ptr, e->data_len);
        ranges_[e->payload_offset] = e->data_len;

        if (total_ == npos && e->payload_len) total_ = e->payload_len;
        if (e->fin) finSeen_ = true;
        return finSeen_ && isContiguous();
    }
    std::vector<uint8_t> take() {
        std::vector<uint8_t> out;
        if (!isContiguous()) return out;
        
        ESP_LOGD("ws_big_reassembler", "Taking complete message of size %zu", total_);
        
        // Allocate result buffer from PSRAM
        uint8_t* resultBuffer = static_cast<uint8_t*>(heap_caps_malloc(total_, MALLOC_CAP_SPIRAM));
        if (!resultBuffer) {
            ESP_LOGE("ws_big_reassembler", "Failed to allocate result buffer of size %zu", total_);
            return out;
        }
        
        // Copy the complete message to result buffer
        memcpy(resultBuffer, buf_, total_);
        
        // Create vector from the buffer
        out.assign(resultBuffer, resultBuffer + total_);
        
        // Free the temporary buffer
        heap_caps_free(resultBuffer);
        
        reset();
        return out;
    }

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
    void reset() { ranges_.clear(); total_ = npos; finSeen_ = false; }

    const size_t kMax;
    uint8_t* buf_;
    std::map<size_t,size_t> ranges_;
    size_t total_ = npos;
    bool finSeen_ = false;
};
