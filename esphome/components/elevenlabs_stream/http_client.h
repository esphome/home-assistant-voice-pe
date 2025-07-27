// http_client.h
#pragma once
#include <string>
#include <functional>

namespace esphome {
namespace elevenlabs_stream {

class HttpClient {
public:
    // Performs a GET request to the given URL with optional headers and returns the response as a string.
    // Returns true on success, false on failure.
    static bool get(const std::string& url,
                    std::string& response_out);
};

} // namespace elevenlabs_stream
} // namespace esphome
