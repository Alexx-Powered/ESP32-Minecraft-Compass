#include "config_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "app_config.h"
#include "app_types.h"

static const char *TAG = "config_portal";
static const char *NETWORK_NVS_NAMESPACE = "mc_net";
static const char *NETWORK_NVS_PASSWORD_KEY = "ap_psk";

/* A stalled client must not occupy the single HTTPD request worker forever. */
#define CONFIG_PORTAL_BODY_RECV_TIMEOUT_US (10LL * 1000LL * 1000LL)
#define CONFIG_PORTAL_BODY_MAX_TIMEOUT_RETRIES 3U

static config_portal_config_t s_config;
static httpd_handle_t s_http_server;
static char s_ap_ssid[33];

extern const uint8_t config_page_html_start[]
    asm("_binary_config_page_html_start");
extern const uint8_t config_page_html_end[]
    asm("_binary_config_page_html_end");

static void set_no_store_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
}

static esp_err_t send_json_error(httpd_req_t *request,
                                 const char *status,
                                 const char *code,
                                 const char *path,
                                 const char *message)
{
    /* cJSON escapes quotes, backslashes and control bytes in all dynamic
     * validation fields.  Keep a static fallback for an out-of-memory error
     * while constructing the error response itself. */
    static const char fallback[] =
        "{\"ok\":false,\"error\":{"
        "\"code\":\"NO_MEMORY\",\"path\":\"$\","
        "\"message\":\"Unable to encode error\"}}";
    char *response = NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *error_object = cJSON_CreateObject();
    if (root != NULL && error_object != NULL &&
        cJSON_AddBoolToObject(root, "ok", false) != NULL &&
        cJSON_AddStringToObject(error_object, "code",
                                code != NULL ? code : "UNKNOWN") != NULL &&
        cJSON_AddStringToObject(error_object, "path",
                                path != NULL ? path : "$") != NULL &&
        cJSON_AddStringToObject(error_object, "message",
                                message != NULL ? message : "Request failed") !=
            NULL &&
        cJSON_AddItemToObject(root, "error", error_object) != 0) {
        error_object = NULL; /* ownership moved to root */
        response = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(error_object);
    cJSON_Delete(root);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    set_no_store_headers(request);
    if (response == NULL) {
        return httpd_resp_sendstr(request, fallback);
    }
    const esp_err_t err = httpd_resp_sendstr(request, response);
    cJSON_free(response);
    return err;
}

static esp_err_t send_json_body_error(httpd_req_t *request, esp_err_t error)
{
    switch (error) {
        case ESP_ERR_TIMEOUT:
            return send_json_error(request, "408 Request Timeout",
                                   "BODY_TIMEOUT", "$",
                                   "Request body receive timed out");
        case ESP_ERR_INVALID_SIZE:
            return send_json_error(request, "413 Payload Too Large",
                                   "BODY_SIZE", "$",
                                   "Request body must contain at most 3072 bytes");
        case ESP_ERR_NO_MEM:
            return send_json_error(request, "500 Internal Server Error",
                                   "NO_MEMORY", "$",
                                   "Unable to allocate request body");
        default:
            return send_json_error(request, "400 Bad Request",
                                   "BODY_RECEIVE", "$",
                                   "Request body could not be received");
    }
}

static esp_err_t send_json(httpd_req_t *request, const char *json)
{
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    set_no_store_headers(request);
    return httpd_resp_sendstr(request, json);
}

static esp_err_t send_geojson(httpd_req_t *request, const char *json)
{
    httpd_resp_set_type(request, "application/geo+json; charset=utf-8");
    set_no_store_headers(request);
    return httpd_resp_sendstr(request, json);
}

static bool request_origin_is_allowed(httpd_req_t *request)
{
    const size_t length = httpd_req_get_hdr_value_len(request, "Origin");
    if (length == 0U) {
        return true;
    }
    if (length >= 96U) {
        return false;
    }
    char origin[96];
    if (httpd_req_get_hdr_value_str(request, "Origin",
                                    origin, sizeof(origin)) != ESP_OK) {
        return false;
    }
    return strcmp(origin, "http://192.168.4.1") == 0;
}

static bool request_content_type_is_json(httpd_req_t *request)
{
    const size_t length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (length == 0U || length >= 96U) {
        return false;
    }
    char content_type[96];
    if (httpd_req_get_hdr_value_str(request, "Content-Type",
                                    content_type, sizeof(content_type)) !=
        ESP_OK) {
        return false;
    }
    static const char *const media_types[] = {
        "application/geo+json", "application/json",
    };
    for (size_t i = 0U; i < sizeof(media_types) / sizeof(media_types[0]); ++i) {
        const size_t prefix_length = strlen(media_types[i]);
        if (strncasecmp(content_type, media_types[i], prefix_length) == 0) {
            const char suffix = content_type[prefix_length];
            if (suffix == '\0' || suffix == ';' || suffix == ' ' || suffix == '\t') {
                return true;
            }
        }
    }
    return false;
}

static esp_err_t receive_json_body(httpd_req_t *request,
                                   char **body,
                                   size_t *body_length)
{
    *body = NULL;
    *body_length = 0U;
    if (request->content_len == 0U ||
        request->content_len > APP_CONFIG_HTTP_BODY_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *buffer = malloc(request->content_len + 1U);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Enforce both a wall-clock deadline and a total timeout-retry budget;
     * receiving a partial byte does not reset either bound. */
    const int64_t deadline_us = esp_timer_get_time() +
                                CONFIG_PORTAL_BODY_RECV_TIMEOUT_US;
    unsigned timeout_retries = 0U;
    size_t total = 0U;
    while (total < request->content_len) {
        if (esp_timer_get_time() >= deadline_us) {
            free(buffer);
            return ESP_ERR_TIMEOUT;
        }
        const int received = httpd_req_recv(request, buffer + total,
                                            request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            timeout_retries++;
            if (timeout_retries >= CONFIG_PORTAL_BODY_MAX_TIMEOUT_RETRIES ||
                esp_timer_get_time() >= deadline_us) {
                free(buffer);
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (received <= 0) {
            free(buffer);
            return ESP_FAIL;
        }
        total += (size_t)received;
    }
    buffer[total] = '\0';
    *body = buffer;
    *body_length = total;
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    const size_t length =
        (size_t)(config_page_html_end - config_page_html_start);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    set_no_store_headers(request);
    httpd_resp_set_hdr(
        request, "Content-Security-Policy",
        "default-src 'self'; style-src 'self' 'unsafe-inline'; "
        "script-src 'self' 'unsafe-inline'; connect-src 'self'; "
        "img-src 'self' data:; object-src 'none'; base-uri 'none'; "
        "frame-ancestors 'none'");
    return httpd_resp_send(request, (const char *)config_page_html_start,
                           (ssize_t)length);
}

static esp_err_t target_get_handler(httpd_req_t *request)
{
    geo_config_snapshot_t snapshot;
    geo_config_manager_get(s_config.geo_manager, &snapshot);
    if (snapshot.target.crs != GEO_CRS_WGS84) {
        return send_json_error(request, "409 Conflict", "LEGACY_CRS", "$",
                               "Saved target uses GCJ02; import WGS84 GeoJSON to replace it");
    }
    char *json = geo_config_target_to_json(&snapshot.target);
    if (json == NULL) {
        return send_json_error(request, "500 Internal Server Error",
                               "NO_MEMORY", "$", "Unable to encode target");
    }
    const esp_err_t err = send_geojson(request, json);
    geo_config_json_free(json);
    return err;
}

static esp_err_t fence_get(httpd_req_t *request, size_t fence_index)
{
    geo_config_snapshot_t snapshot;
    geo_config_manager_get(s_config.geo_manager, &snapshot);
    if (snapshot.fences[fence_index].crs != GEO_CRS_WGS84) {
        return send_json_error(request, "409 Conflict", "LEGACY_CRS", "$",
                               "Saved fence uses GCJ02; import WGS84 GeoJSON to replace it");
    }
    char *json = geo_config_fence_to_json(&snapshot.fences[fence_index],
                                          fence_index);
    if (json == NULL) {
        return send_json_error(request, "500 Internal Server Error",
                               "NO_MEMORY", "$", "Unable to encode fence");
    }
    const esp_err_t err = send_geojson(request, json);
    geo_config_json_free(json);
    return err;
}

static esp_err_t fence1_get_handler(httpd_req_t *request)
{
    return fence_get(request, 0U);
}

static esp_err_t fence2_get_handler(httpd_req_t *request)
{
    return fence_get(request, 1U);
}

static bool validate_write_request(httpd_req_t *request)
{
    if (!request_origin_is_allowed(request)) {
        (void)send_json_error(request, "403 Forbidden", "ORIGIN_REJECTED",
                              "$", "Request origin is not allowed");
        return false;
    }
    if (!request_content_type_is_json(request)) {
        (void)send_json_error(request, "415 Unsupported Media Type",
                              "CONTENT_TYPE", "$",
                              "Content-Type must be application/geo+json or application/json");
        return false;
    }
    return true;
}

static esp_err_t target_put_handler(httpd_req_t *request)
{
    if (!validate_write_request(request)) {
        return ESP_OK;
    }
    esp_err_t err;
    char *body = NULL;
    size_t body_length = 0U;
    err = receive_json_body(request, &body, &body_length);
    if (err != ESP_OK) {
        return send_json_body_error(request, err);
    }

    geo_target_config_t target;
    geo_validation_error_t validation;
    err = geo_config_parse_target_json(body, body_length, &target, &validation);
    free(body);
    if (err != ESP_OK) {
        return send_json_error(request, "422 Unprocessable Content",
                               validation.code, validation.path,
                               validation.message);
    }
    err = geo_config_manager_set_target(s_config.geo_manager, &target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Target NVS save failed: %s", esp_err_to_name(err));
        return send_json_error(request, "503 Service Unavailable",
                               "NVS_WRITE", "$",
                               "Target was not saved; previous value is active");
    }
    ESP_LOGI(TAG, "Target updated: '%s' %s (%ld, %ld)",
             target.name, geo_config_crs_name(target.crs),
             (long)target.coordinate.latitude_e6,
             (long)target.coordinate.longitude_e6);
    char *json = geo_config_target_to_json(&target);
    if (json == NULL) {
        return send_json_error(request, "500 Internal Server Error",
                               "NO_MEMORY", "$", "Target saved but encode failed");
    }
    err = send_geojson(request, json);
    geo_config_json_free(json);
    return err;
}

static esp_err_t fence_put(httpd_req_t *request, size_t fence_index)
{
    if (!validate_write_request(request)) {
        return ESP_OK;
    }
    esp_err_t err;
    char *body = NULL;
    size_t body_length = 0U;
    err = receive_json_body(request, &body, &body_length);
    if (err != ESP_OK) {
        return send_json_body_error(request, err);
    }

    geo_fence_config_t fence;
    geo_validation_error_t validation;
    err = geo_config_parse_fence_json(body, body_length, fence_index,
                                      &fence, &validation);
    free(body);
    if (err != ESP_OK) {
        return send_json_error(request, "422 Unprocessable Content",
                               validation.code, validation.path,
                               validation.message);
    }
    err = geo_config_manager_set_fence(s_config.geo_manager, fence_index,
                                       &fence);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fence%u NVS save failed: %s",
                 (unsigned)(fence_index + 1U), esp_err_to_name(err));
        return send_json_error(request, "503 Service Unavailable",
                               "NVS_WRITE", "$",
                               "Fence was not saved; previous value is active");
    }
    ESP_LOGI(TAG, "Fence%u updated: '%s' %s %u vertices enabled=%d",
             (unsigned)(fence_index + 1U), fence.name,
             geo_config_crs_name(fence.crs), (unsigned)fence.vertex_count,
             fence.enabled);
    char *json = geo_config_fence_to_json(&fence, fence_index);
    if (json == NULL) {
        return send_json_error(request, "500 Internal Server Error",
                               "NO_MEMORY", "$", "Fence saved but encode failed");
    }
    err = send_geojson(request, json);
    geo_config_json_free(json);
    return err;
}

static esp_err_t fence1_put_handler(httpd_req_t *request)
{
    return fence_put(request, 0U);
}

static esp_err_t fence2_put_handler(httpd_req_t *request)
{
    return fence_put(request, 1U);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    gps_fix_t gps = {0};
    if (s_config.gps_queue != NULL) {
        (void)xQueuePeek(s_config.gps_queue, &gps, 0);
    }
    const int64_t now_us = esp_timer_get_time();
    const bool gps_valid = gps.valid &&
        now_us - gps.last_fix_us <= APP_GPS_FIX_MAX_AGE_US;

    geo_config_snapshot_t snapshot;
    geo_config_manager_get(s_config.geo_manager, &snapshot);
    geo_fence_state_t states[GEO_FENCE_COUNT];
    geo_config_evaluate_fences(&snapshot, gps_valid,
                               gps.latitude_deg, gps.longitude_deg, states);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_json_error(request, "500 Internal Server Error",
                               "NO_MEMORY", "$", "Unable to encode status");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "ssid", s_ap_ssid);
    cJSON_AddNumberToObject(root, "revision", snapshot.revision);
    cJSON_AddBoolToObject(root, "gpsValid", gps_valid);
    if (gps_valid) {
        cJSON_AddNumberToObject(root, "latitude", gps.latitude_deg);
        cJSON_AddNumberToObject(root, "longitude", gps.longitude_deg);
    }
    cJSON_AddBoolToObject(root, "targetEnabled",
                          snapshot.target.configured && snapshot.target.enabled);
    cJSON *fences = cJSON_AddArrayToObject(root, "fences");
    for (size_t index = 0U; fences != NULL &&
         index < GEO_FENCE_COUNT; ++index) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id",
                               index == 0U ? "fence1" : "fence2");
        cJSON_AddStringToObject(item, "state",
                               geo_fence_state_name(states[index]));
        cJSON_AddItemToArray(fences, item);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return send_json_error(request, "500 Internal Server Error",
                               "NO_MEMORY", "$", "Unable to encode status");
    }
    const esp_err_t err = send_json(request, json);
    cJSON_free(json);
    return err;
}

static esp_err_t not_found_handler(httpd_req_t *request,
                                   httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    set_no_store_headers(request);
    return httpd_resp_sendstr(request, "Open /");
}

static esp_err_t register_http_handlers(void)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler},
        {.uri = "/api/v1/status", .method = HTTP_GET,
         .handler = status_get_handler},
        {.uri = "/api/v1/target", .method = HTTP_GET,
         .handler = target_get_handler},
        {.uri = "/api/v1/target", .method = HTTP_PUT,
         .handler = target_put_handler},
        {.uri = "/api/v1/fence/1", .method = HTTP_GET,
         .handler = fence1_get_handler},
        {.uri = "/api/v1/fence/1", .method = HTTP_PUT,
         .handler = fence1_put_handler},
        {.uri = "/api/v1/fence/2", .method = HTTP_GET,
         .handler = fence2_get_handler},
        {.uri = "/api/v1/fence/2", .method = HTTP_PUT,
         .handler = fence2_put_handler},
    };
    for (size_t index = 0U;
         index < sizeof(handlers) / sizeof(handlers[0]); ++index) {
        const esp_err_t err = httpd_register_uri_handler(s_http_server,
                                                          &handlers[index]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND,
                                      not_found_handler);
}

static void load_or_create_password(char *password, size_t password_size)
{
    /* Use the configured WPA2 password. Keep the existing NVS key so an old
     * random password is replaced once without erasing other user settings. */
    (void)snprintf(password, password_size, "%s", APP_CONFIG_AP_PASSWORD);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NETWORK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        char stored_password[65] = {0};
        size_t length = sizeof(stored_password);
        const esp_err_t read_err = nvs_get_str(handle, NETWORK_NVS_PASSWORD_KEY,
                                               stored_password, &length);
        if (read_err != ESP_OK || strcmp(stored_password, password) != 0) {
            err = nvs_set_str(handle, NETWORK_NVS_PASSWORD_KEY, password);
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AP password not persisted this boot: %s",
                     esp_err_to_name(err));
        }
        nvs_close(handle);
        return;
    }
    ESP_LOGW(TAG, "Could not open network NVS; AP password is session-only");
}

esp_err_t config_portal_start(const config_portal_config_t *config,
                              char *ssid,
                              size_t ssid_size,
                              char *password,
                              size_t password_size)
{
    if (config == NULL || config->geo_manager == NULL ||
        ssid == NULL || ssid_size < sizeof(s_ap_ssid) ||
        password == NULL || password_size < 17U) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }
    if (esp_netif_create_default_wifi_ap() == NULL) {
        return ESP_FAIL;
    }
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6];
    err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        return err;
    }
    (void)snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
                   APP_CONFIG_AP_SSID_PREFIX, mac[4], mac[5]);
    load_or_create_password(password, password_size);
    (void)snprintf(ssid, ssid_size, "%s", s_ap_ssid);

    wifi_config_t wifi_config = {
        .ap = {
            .channel = APP_CONFIG_AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = APP_CONFIG_AP_MAX_CLIENTS,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    memcpy(wifi_config.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    wifi_config.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(wifi_config.ap.password, password, strlen(password));

    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.lru_purge_enable = true;
    http_config.max_open_sockets = 3;
    http_config.max_uri_handlers = 10;
    http_config.stack_size = 8192;
    err = httpd_start(&s_http_server, &http_config);
    if (err != ESP_OK) {
        return err;
    }
    err = register_http_handlers();
    if (err != ESP_OK) {
        (void)httpd_stop(s_http_server);
        s_http_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "SoftAP ready: SSID=%s password=%s", s_ap_ssid, password);
    ESP_LOGI(TAG, "Open http://192.168.4.1 (local-only configuration page)");
    return ESP_OK;
}
