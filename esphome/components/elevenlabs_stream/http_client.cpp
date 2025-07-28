// http_client.cpp
#include "http_client.h"
#include <esp_http_client.h>
#include <esp_task_wdt.h>
#include <string>
#include <map>
#include "esp_crt_bundle.h"

namespace esphome {
namespace elevenlabs_stream {

bool HttpClient::get(const std::string& url,
                     const std::map<std::string, std::string>& headers,
                     std::string& response_out) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 3000;
    config.method = HTTP_METHOD_GET;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.is_async = false;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.use_global_ca_store = false;
    config.skip_cert_common_name_check = false;
    config.disable_auto_redirect = true;

    response_out.clear();
    auto http_event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        std::string *response_ptr = static_cast<std::string *>(evt->user_data);
        switch (evt->event_id) {
            case HTTP_EVENT_ON_CONNECTED:
            case HTTP_EVENT_ON_FINISH:
            case HTTP_EVENT_ERROR:
            case HTTP_EVENT_DISCONNECTED:
                esp_task_wdt_reset();
                break;
            case HTTP_EVENT_ON_DATA:
                if (evt->data_len > 0 && response_ptr) {
                    esp_task_wdt_reset();
                    std::string response_chunk(static_cast<const char*>(evt->data), evt->data_len);
                    (*response_ptr) += response_chunk;
                }
                break;
            default:
                break;
        }
        return ESP_OK;
    };
    config.event_handler = http_event_handler;
    config.user_data = &response_out;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }
    // Set headers
    for (const auto& kv : headers) {
        esp_http_client_set_header(client, kv.first.c_str(), kv.second.c_str());
    }
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK;
}

} // namespace elevenlabs_stream
} // namespace esphome
