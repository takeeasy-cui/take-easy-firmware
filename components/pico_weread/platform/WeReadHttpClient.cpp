/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 原生 ESP-IDF HTTPS 流，保留证书校验与可取消的读取。
 * Native ESP-IDF HTTPS streaming with certificate verification and cancellable reads.
 * 冻结：禁用自动重定向，不记录载荷或 Cookie。/ Frozen: no automatic redirects, payload or Cookie logs.
 */
#include "WeReadHttpClient.h"
#include "HalStorage.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
extern "C" {
#include "read_pico_transfer.h"
}
#include <cctype>
#include <algorithm>
#include <cstring>
#include <climits>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace WeReadHttpClient {
bool parseHttpsUrl(const char* url, HttpsUrlView& view) {
    view = {};
    if (!url || strncmp(url, "https://", 8) || strpbrk(url, "\r\n#")) return false;
    view.host = url + 8;
    view.path = strchr(view.host, '/');
    if (!view.path || view.path == view.host || view.host[0] == '.' || view.path[-1] == '.') return false;
    view.hostLength = view.path - view.host;
    if (view.hostLength >= 128) return false;
    for (size_t i = 0; i < view.hostLength; ++i) {
        const unsigned char c = view.host[i];
        if ((!std::isalnum(c) && c != '.' && c != '-') || (c == '.' && i && view.host[i - 1] == '.')) return false;
    }
    return true;
}
bool extractHttpsHost(const char* url, char* out, size_t cap) {
    HttpsUrlView view;
    if (!out || !parseHttpsUrl(url, view) || view.hostLength >= cap) return false;
    for (size_t i = 0; i < view.hostLength; ++i) out[i] = std::tolower(static_cast<unsigned char>(view.host[i]));
    out[view.hostLength] = 0;
    return true;
}
bool networkReady() {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    return status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready;
}
Session::~Session() { reset(); }
void Session::reset() {
    if (client_) esp_http_client_cleanup(client_);
    client_ = nullptr; host_[0] = 0; complete_ = false;
}
bool Session::reusable() { return client_ && complete_; }
void Session::clearStats() { newConnections_ = reusedRequests_ = 0; }

struct HeaderContext { const HeaderCallback* callback; };
static esp_err_t on_event(esp_http_client_event_t* event) {
    auto* ctx = static_cast<HeaderContext*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER && ctx && *ctx->callback && event->header_key && event->header_value)
        (*ctx->callback)(event->header_key, event->header_value);
    return ESP_OK;
}
Result request(Session& session, const char* url, const RequestOptions& options, const DataCallback& onData,
               const HeaderCallback& onHeader, int& status) {
    status = -1;
    HttpsUrlView parsed;
    if (pico_weread_cancelled()) return Result::Aborted;
    if (!parseHttpsUrl(url, parsed) || !options.method ||
        (strcmp(options.method, "GET") && strcmp(options.method, "POST")) ||
        !options.readBuffer || !options.readBufferSize || options.bodySize > INT_MAX ||
        (options.bodySize && !options.body) || (options.headerCount && !options.headers)) return Result::NetworkError;
    // 暂时断网先等待自动重连；取消始终优先。/ Allow brief reconnects; cancellation always wins.
    const int64_t networkDeadline = esp_timer_get_time() + 15000000;
    while (!networkReady()) {
        if (pico_weread_cancelled()) return Result::Aborted;
        if (esp_timer_get_time() >= networkDeadline) return Result::NetworkError;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    char host[128];
    if (!extractHttpsHost(url, host, sizeof(host))) return Result::NetworkError;
    HeaderContext header{&onHeader};
    const bool reuse = session.reusable() && !strcmp(host, session.host_);
    if (!reuse) {
        session.reset();
        esp_http_client_config_t config = {};
        config.url = url;
        config.method = !strcmp(options.method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET;
        config.timeout_ms = 15000;
        config.buffer_size = 2048;
        config.buffer_size_tx = 2048;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.disable_auto_redirect = true;
        config.keep_alive_enable = true;
        config.event_handler = on_event;
        config.user_data = &header;
        session.client_ = esp_http_client_init(&config);
        if (!session.client_) return Result::NetworkError;
        strcpy(session.host_, host);
        ++session.newConnections_;
    } else {
        if (esp_http_client_set_url(session.client_, url) != ESP_OK ||
            esp_http_client_set_method(session.client_, !strcmp(options.method, "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET) != ESP_OK ||
            esp_http_client_set_user_data(session.client_, &header) != ESP_OK) {
            session.reset(); return Result::NetworkError;
        }
        esp_http_client_set_timeout_ms(session.client_, 15000);
        ++session.reusedRequests_;
    }
    session.complete_ = false;
    auto finish = [&session, &options](Result result) {
        // 仅复用同域且已读完的响应；清除每次请求的鉴权头和栈上回调地址。
        // Reuse fully drained same-host responses only; clear credentials and the stack callback pointer.
        if (result == Result::Ok) {
            for (size_t i = 0; i < options.headerCount; ++i)
                esp_http_client_delete_header(session.client_, options.headers[i].name);
            esp_http_client_set_user_data(session.client_, nullptr);
            session.complete_ = true;
        } else session.reset();
        return result;
    };
    for (size_t i = 0; i < options.headerCount; ++i) {
        const Header& h = options.headers[i];
        if (!h.name || !h.value || strpbrk(h.name, "\r\n") || strpbrk(h.value, "\r\n") ||
            esp_http_client_set_header(session.client_, h.name, h.value) != ESP_OK)
            return finish(Result::NetworkError);
    }
    if (esp_http_client_open(session.client_, options.bodySize) != ESP_OK) return finish(Result::NetworkError);
    size_t sent = 0;
    while (sent < options.bodySize) {
        if (pico_weread_cancelled()) return finish(Result::Aborted);
        int n = esp_http_client_write(session.client_, reinterpret_cast<const char*>(options.body + sent), options.bodySize - sent);
        if (n <= 0) return finish(Result::NetworkError);
        sent += n;
    }
    if (pico_weread_cancelled()) return finish(Result::Aborted);
    if (esp_http_client_fetch_headers(session.client_) < 0) return finish(Result::NetworkError);
    status = esp_http_client_get_status_code(session.client_);
    esp_http_client_set_timeout_ms(session.client_, 1000);
    const int64_t idleMs = std::clamp(options.timeoutMs, 15000, 60000);
    int64_t deadline = esp_timer_get_time() + idleMs * 1000;
    uint64_t received = 0;
    // SDK接收完成时仍可能留有fetch_headers缓存，必须读空缓存。/ Drain fetch_headers cache even when the SDK reports complete reception.
    while (true) {
        if (pico_weread_cancelled()) return finish(Result::Aborted);
        if (esp_timer_get_time() >= deadline) return finish(Result::NetworkError);
        const int n = esp_http_client_read(session.client_, reinterpret_cast<char*>(options.readBuffer), options.readBufferSize);
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (n < 0) return finish(Result::NetworkError);
        if (!n) break;
        deadline = esp_timer_get_time() + idleMs * 1000;
        received += n;
        if (received > 64 * 1024 * 1024) return finish(Result::NetworkError);
        if (onData && !onData(options.readBuffer, n)) return finish(Result::Aborted);
    }
    return finish(esp_http_client_is_complete_data_received(session.client_) ? Result::Ok : Result::NetworkError);
}
Result request(const char* url, const RequestOptions& options, const DataCallback& onData,
               const HeaderCallback& onHeader, int& status) {
    Session session;
    return request(session, url, options, onData, onHeader, status);
}
}
