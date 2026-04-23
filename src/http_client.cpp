#include "http_client.hpp"

#include <curl/curl.h>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* str = static_cast<std::string*>(userdata);
    str->append(ptr, size * nmemb);
    return size * nmemb;
}

struct StreamState {
    StreamCallback  cb;
    std::string     buffer;  // accumulates incomplete lines
};

static size_t stream_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* state  = static_cast<StreamState*>(userdata);
    size_t bytes = size * nmemb;
    state->buffer.append(ptr, bytes);

    // Dispatch every complete newline-delimited chunk
    std::string::size_type pos;
    while ((pos = state->buffer.find('\n')) != std::string::npos) {
        std::string line = state->buffer.substr(0, pos);
        state->buffer.erase(0, pos + 1);
        if (!line.empty() && !state->cb(line))
            return 0;  // signal libcurl to abort
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct HttpClient::Impl {
    HttpConfig cfg;

    CURL* make_handle() const {
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");

        if (!cfg.bearer_token.empty()) {
            std::string header = "Authorization: Bearer " + cfg.bearer_token;
            // We set the header below via slist; store it here temporarily
            (void)header;
        }
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);           // streaming: no timeout
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

        if (!cfg.ca_cert_path.empty())
            curl_easy_setopt(curl, CURLOPT_CAINFO, cfg.ca_cert_path.c_str());

        if (!cfg.verify_tls)
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

        return curl;
    }

    curl_slist* make_headers(const std::string& content_type = "") const {
        curl_slist* headers = nullptr;
        if (!cfg.bearer_token.empty()) {
            std::string auth = "Authorization: Bearer " + cfg.bearer_token;
            headers = curl_slist_append(headers, auth.c_str());
        }
        headers = curl_slist_append(headers, "Accept: application/json");
        if (!content_type.empty()) {
            std::string ct = "Content-Type: " + content_type;
            headers = curl_slist_append(headers, ct.c_str());
        }
        return headers;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

HttpClient::HttpClient(HttpConfig cfg) : impl_(new Impl{std::move(cfg)}) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

HttpClient::~HttpClient() {
    delete impl_;
    curl_global_cleanup();
}

HttpResponse HttpClient::get(const std::string& path) const {
    CURL* curl = impl_->make_handle();
    curl_slist* headers = impl_->make_headers();

    std::string url = impl_->cfg.base_url + path;
    HttpResponse resp;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        resp.error = curl_easy_strerror(rc);
    else
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

HttpResponse HttpClient::post(const std::string& path,
                               const std::string& body,
                               const std::string& content_type) const {
    CURL* curl = impl_->make_handle();
    curl_slist* headers = impl_->make_headers(content_type);

    std::string url = impl_->cfg.base_url + path;
    HttpResponse resp;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        resp.error = curl_easy_strerror(rc);
    else
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

void HttpClient::watch(const std::string& path, StreamCallback cb) const {
    CURL* curl = impl_->make_handle();
    curl_slist* headers = impl_->make_headers();

    std::string url = impl_->cfg.base_url + path;
    StreamState state{std::move(cb), {}};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

    curl_easy_perform(curl);  // blocks until stream ends / cb returns false

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
