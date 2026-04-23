#pragma once

#include <functional>
#include <string>

struct HttpConfig {
    std::string base_url;
    std::string bearer_token;
    std::string ca_cert_path;   // path to CA bundle (in-cluster: /var/run/secrets/…)
    bool        verify_tls = true;
};

struct HttpResponse {
    long        status_code = 0;
    std::string body;
    std::string error;

    bool ok() const { return status_code >= 200 && status_code < 300; }
};

// Callback invoked for each newline-delimited chunk during a streaming GET.
// Return false to abort the stream.
using StreamCallback = std::function<bool(const std::string& line)>;

class HttpClient {
public:
    explicit HttpClient(HttpConfig cfg);
    ~HttpClient();

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse get(const std::string& path) const;

    HttpResponse post(const std::string& path,
                      const std::string& body,
                      const std::string& content_type = "application/json") const;

    // Streaming GET — calls cb for every newline-delimited chunk.
    // Blocks until the stream ends or cb returns false.
    void watch(const std::string& path, StreamCallback cb) const;

private:
    struct Impl;
    Impl* impl_;
};
