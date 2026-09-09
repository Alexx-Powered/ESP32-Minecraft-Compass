#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define GEO_CONFIG_SCHEMA_VERSION 1U
#define GEO_FENCE_COUNT 2U
#define GEO_FENCE_MAX_VERTICES 16U
#define GEO_CONFIG_NAME_MAX_BYTES 128U
#define GEO_CONFIG_JSON_MAX_BYTES 3072U

typedef enum {
    GEO_CRS_WGS84 = 0,
    GEO_CRS_GCJ02 = 1,
} geo_crs_t;

typedef struct {
    int32_t latitude_e6;
    int32_t longitude_e6;
} geo_point_e6_t;

typedef struct {
    bool configured;
    bool enabled;
    bool has_geometry;
    char name[GEO_CONFIG_NAME_MAX_BYTES + 1U];
    geo_crs_t crs;
    geo_point_e6_t coordinate;
} geo_target_config_t;

typedef struct {
    bool configured;
    bool enabled;
    char name[GEO_CONFIG_NAME_MAX_BYTES + 1U];
    geo_crs_t crs;
    uint8_t vertex_count;
    geo_point_e6_t vertices[GEO_FENCE_MAX_VERTICES];
    int32_t minimum_latitude_e6;
    int32_t maximum_latitude_e6;
    int32_t minimum_longitude_e6;
    int32_t maximum_longitude_e6;
} geo_fence_config_t;

typedef struct {
    geo_target_config_t target;
    geo_fence_config_t fences[GEO_FENCE_COUNT];
    uint32_t revision;
} geo_config_snapshot_t;

typedef struct {
    SemaphoreHandle_t lock;
    bool storage_available;
    geo_config_snapshot_t current;
} geo_config_manager_t;

typedef enum {
    GEO_FENCE_STATE_UNCONFIGURED = -2,
    GEO_FENCE_STATE_DISABLED = -1,
    GEO_FENCE_STATE_UNKNOWN = 0,
    GEO_FENCE_STATE_OUTSIDE = 1,
    GEO_FENCE_STATE_INSIDE = 2,
} geo_fence_state_t;

typedef struct {
    char code[32];
    char path[64];
    char message[128];
} geo_validation_error_t;

esp_err_t geo_config_manager_init(geo_config_manager_t *manager,
                                  bool storage_available);
void geo_config_manager_get(geo_config_manager_t *manager,
                            geo_config_snapshot_t *snapshot);
esp_err_t geo_config_manager_set_target(geo_config_manager_t *manager,
                                        const geo_target_config_t *target);
esp_err_t geo_config_manager_set_fence(geo_config_manager_t *manager,
                                       size_t fence_index,
                                       const geo_fence_config_t *fence);

/* Public input/output uses RFC 7946 WGS84 GeoJSON Feature objects. The HTTP
 * endpoint selects the slot; a Feature's optional id is only source metadata.
 * Legacy custom JSON is accepted solely by the private NVS loading path. */
esp_err_t geo_config_parse_target_json(const char *json,
                                       size_t json_length,
                                       geo_target_config_t *target,
                                       geo_validation_error_t *error);
esp_err_t geo_config_parse_fence_json(const char *json,
                                      size_t json_length,
                                      size_t fence_index,
                                      geo_fence_config_t *fence,
                                      geo_validation_error_t *error);
char *geo_config_target_to_json(const geo_target_config_t *target);
char *geo_config_fence_to_json(const geo_fence_config_t *fence,
                               size_t fence_index);
void geo_config_json_free(char *json);

bool geo_config_resolve_target(const geo_config_snapshot_t *snapshot,
                               double gps_latitude_wgs84,
                               double gps_longitude_wgs84,
                               double *origin_latitude,
                               double *origin_longitude,
                               double *target_latitude,
                               double *target_longitude);
void geo_config_evaluate_fences(const geo_config_snapshot_t *snapshot,
                                bool gps_valid,
                                double gps_latitude_wgs84,
                                double gps_longitude_wgs84,
                                geo_fence_state_t states[GEO_FENCE_COUNT]);
const char *geo_config_crs_name(geo_crs_t crs);
const char *geo_fence_state_name(geo_fence_state_t state);
