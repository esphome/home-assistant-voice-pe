
#pragma once
#include <string>
#include <functional>
#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_crt_bundle.h>
#include "esphome/core/log.h"
#include <map>
#include <vector>
#include <algorithm>
#include <esp_heap_caps.h>

namespace esphome {
namespace elevenlabs_stream {



class WebsocketMessageAssembler {
    static constexpr size_t npos = SIZE_MAX;
public:
    explicit WebsocketMessageAssembler(size_t maxBytes = 256*1024);
    ~WebsocketMessageAssembler();

    bool add(const esp_websocket_event_data_t* e);
    bool isReady() const;
    const uint8_t* getBuffer() const;
    size_t getSize() const;
    void reset();
private:
    bool isContiguous() const;
    bool abort();
    const size_t kMax;
    uint8_t* buf_;
    std::map<size_t,size_t> ranges_;
    size_t total_ = npos;
    bool finSeen_ = false;
};


class WebsocketClient {
public:
    WebsocketClient();
    ~WebsocketClient();

    bool connect(const std::string& url,
                 std::function<void(const uint8_t*, size_t)> on_message,
                 std::function<void()> on_connected,
                 std::function<void()> on_disconnected,
                 std::function<void(const std::string&)> on_error);
    void disconnect();
    bool send_message(const std::string& message);
    bool send_binary(const uint8_t* data, size_t length);
    bool is_connected() const;

private:
    static void websocket_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    esp_websocket_client_handle_t websocket_client_ = nullptr;
    bool websocket_connected_ = false;
    std::function<void(const uint8_t*, size_t)> on_message_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void(const std::string&)> on_error_;
    WebsocketMessageAssembler reassembler_{512*1024};
};

} // namespace elevenlabs_stream
} // namespace esphome
