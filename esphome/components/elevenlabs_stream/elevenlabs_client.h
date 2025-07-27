// elevenlabs_client.h
// Handles all direct ElevenLabs API integration: signed URL, HTTP, WebSocket config, connection, and protocol details.
// See ElevenLabs API docs: https://docs.elevenlabs.io/api-reference/convai
#pragma once

#include <string>
#include <functional>
#include <esp_http_client.h>
#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>

namespace esphome {
namespace elevenlabs_stream {

class ElevenLabsClient {
public:
  explicit ElevenLabsClient(const std::string& agent_id, const std::string& api_key = "");
  ~ElevenLabsClient();

  // Gets a signed URL from ElevenLabs API
  bool get_signed_url(std::string& signed_url_out);

  // Connects to ElevenLabs WebSocket using signed URL
  bool connect(const std::string& signed_url,
               std::function<void(const uint8_t*, size_t)> on_message,
               std::function<void()> on_connected,
               std::function<void()> on_disconnected,
               std::function<void(const std::string&)> on_error);

  // Disconnects from ElevenLabs WebSocket
  void disconnect();

  // Sends a text message over WebSocket
  bool send_message(const std::string& message);

  // Sends binary data over WebSocket
  bool send_binary(const uint8_t* data, size_t length);

  // Returns connection state
  bool is_connected() const;

private:
  std::string agent_id_;
  std::string api_key_;
  esp_websocket_client_handle_t websocket_client_ = nullptr;
  bool websocket_connected_ = false;

  // Internal event handlers
  static void websocket_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);

  // Callbacks
  std::function<void(const uint8_t*, size_t)> on_message_;
  std::function<void()> on_connected_;
  std::function<void()> on_disconnected_;
  std::function<void(const std::string&)> on_error_;
};

} // namespace elevenlabs_stream
} // namespace esphome
