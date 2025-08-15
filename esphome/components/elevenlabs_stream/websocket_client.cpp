
#include "websocket_client.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace elevenlabs_stream {

    static const char* TAG = "WebsocketClient";

// WebsocketMessageAssembler implementation
WebsocketMessageAssembler::WebsocketMessageAssembler(size_t maxBytes)
    : kMax(maxBytes)
{
    buf_ = static_cast<uint8_t*>(heap_caps_malloc(kMax, MALLOC_CAP_SPIRAM));
    assert(buf_ && "PSRAM alloc failed");
}
WebsocketMessageAssembler::~WebsocketMessageAssembler() { if (buf_) free(buf_); }
bool WebsocketMessageAssembler::add(const esp_websocket_event_data_t* e) {
    if (e->payload_offset + e->data_len > kMax) return abort();
    memcpy(buf_ + e->payload_offset, e->data_ptr, e->data_len);
    ranges_[e->payload_offset] = e->data_len;
    if (total_ == npos && e->payload_len) total_ = e->payload_len;
    if (e->fin) finSeen_ = true;
    return this->isReady();
}
bool WebsocketMessageAssembler::isReady() const { return finSeen_ && isContiguous(); }
const uint8_t* WebsocketMessageAssembler::getBuffer() const { return isReady() ? buf_ : nullptr; }
size_t WebsocketMessageAssembler::getSize() const { return isReady() ? total_ : 0; }
void WebsocketMessageAssembler::reset() { ranges_.clear(); total_ = npos; finSeen_ = false; }
bool WebsocketMessageAssembler::isContiguous() const {
    size_t next = 0;
    for (auto& [off, len] : ranges_) {
        if (off != next) return false;
        next += len;
    }
    return total_ != npos && next == total_;
}
bool WebsocketMessageAssembler::abort() { reset(); return false; }

// WebsocketClient implementation
WebsocketClient::WebsocketClient() {}
WebsocketClient::~WebsocketClient() { disconnect(); }
bool WebsocketClient::connect(const std::string &url,
                              std::function<void(const uint8_t *, size_t)> on_message,
                              std::function<void()> on_connected,
                              std::function<void()> on_disconnected,
                              std::function<void(const std::string &)> on_error) {
    if (url.empty()) {
        ESP_LOGE(TAG, "No URL provided");
        return false;
    }
    if (this->websocket_client_) {
        this->disconnect();
    }
    on_message_ = on_message;
    on_connected_ = on_connected;
    on_disconnected_ = on_disconnected;
    on_error_ = on_error;

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = url.c_str();
    ws_cfg.buffer_size = 4096;
    ws_cfg.task_stack = 8192;
    ws_cfg.task_prio = 1;
    ws_cfg.disable_auto_reconnect = true;
    ws_cfg.user_context = this;
    ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.reconnect_timeout_ms = 5000;
    ws_cfg.cert_pem = nullptr;
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.use_global_ca_store = false;
    ws_cfg.skip_cert_common_name_check = false;

    this->websocket_client_ = esp_websocket_client_init(&ws_cfg);
    if (!this->websocket_client_) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return false;
    }
    esp_err_t reg_err = esp_websocket_register_events(this->websocket_client_, WEBSOCKET_EVENT_ANY,
                                                     &WebsocketClient::websocket_event_handler, this);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebSocket events");
        esp_websocket_client_destroy(this->websocket_client_);
        this->websocket_client_ = nullptr;
        return false;
    }
    esp_err_t err = esp_websocket_client_start(this->websocket_client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client");
        esp_websocket_client_destroy(this->websocket_client_);
        this->websocket_client_ = nullptr;
        return false;
    }
    return true;
}
void WebsocketClient::disconnect() {
    if (this->websocket_client_) {
        esp_websocket_client_stop(this->websocket_client_);
        esp_websocket_client_destroy(this->websocket_client_);
        this->websocket_client_ = nullptr;
        this->websocket_connected_ = false;
    }
}
bool WebsocketClient::send_message(const std::string &message) {
    if (!this->websocket_connected_ || !this->websocket_client_ || message.empty()) {
        return false;
    }
    int sent = esp_websocket_client_send_text(this->websocket_client_, message.c_str(), message.length(), portMAX_DELAY);
    return sent >= 0;
}
bool WebsocketClient::send_binary(const uint8_t *data, size_t length) {
    if (!this->websocket_connected_ || !this->websocket_client_ || !data || length == 0) {
        return false;
    }
    int sent = esp_websocket_client_send_bin(this->websocket_client_, (const char *) data, length, portMAX_DELAY);
    return sent >= 0;
}
bool WebsocketClient::is_connected() const { 
    bool result = this->websocket_connected_ && this->websocket_client_ != nullptr;
    
    // Add periodic logging to debug connection state issues
    static uint32_t last_log_time = 0;
    uint32_t now = millis();
    if (now - last_log_time > 5000) { // Log every 5 seconds
        ESP_LOGD(TAG, "WebSocket connection state: internal_flag=%s, client_exists=%s, result=%s",
                 this->websocket_connected_ ? "connected" : "disconnected",
                 this->websocket_client_ ? "yes" : "no",
                 result ? "connected" : "disconnected");
        last_log_time = now;
    }
    
    return result;
}
void WebsocketClient::websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                              void *event_data) {
    WebsocketClient *client = static_cast<WebsocketClient *>(handler_args);
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            client->websocket_connected_ = true;
            ESP_LOGI(TAG, "WebSocket connected state set to: %s", client->websocket_connected_ ? "true" : "false");
            if (client->on_connected_)
                client->on_connected_();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            client->websocket_connected_ = false;
            ESP_LOGI(TAG, "WebSocket connected state set to: %s", client->websocket_connected_ ? "true" : "false");
            if (client->on_disconnected_)
                client->on_disconnected_();
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error occurred");
            if (client->on_error_)
                client->on_error_("WebSocket connection error");
            // Don't set disconnected here - let explicit disconnect events handle it
            break;
        case WEBSOCKET_EVENT_DATA:
        {
            esp_websocket_event_data_t *data = (esp_websocket_event_data_t *) event_data;
            
            if (data->op_code == 0x08) { // Close frame
                ESP_LOGW(TAG, "WS_EVENT: WebSocket close frame received");
                ESP_LOGI(TAG, "WebSocket disconnected (close frame)");
                client->websocket_connected_ = false;
                ESP_LOGI(TAG, "WebSocket connected state set to: %s (close frame)", client->websocket_connected_ ? "true" : "false");
                if (client->on_disconnected_)
                    client->on_disconnected_();
                break;
            }

            if (client->reassembler_.add(data)) {
                if (client->on_message_) {
                    client->on_message_(client->reassembler_.getBuffer(), client->reassembler_.getSize());
                }
                client->reassembler_.reset();
            }
        }
            break;
        default:
            break;
    }
}

} // namespace elevenlabs_stream
} // namespace esphome
