/* Runs the production geo_config.c and ESP-IDF cJSON on the host. Only NVS,
 * logging and FreeRTOS synchronization are mocked; no geometry is reimplemented. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "geo_config.h"
#include "nvs.h"

static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

static char saved[3][GEO_CONFIG_JSON_MAX_BYTES + 1U];
static char pending[3][GEO_CONFIG_JSON_MAX_BYTES + 1U];
static unsigned commits;
static int fail_commit;

static unsigned slot(const char *key)
{
    if (strcmp(key, "target") == 0) return 0;
    if (strcmp(key, "fence1") == 0) return 1;
    CHECK(strcmp(key, "fence2") == 0);
    return 2;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    (void)mode;
    CHECK(strcmp(name, "mc_geo") == 0);
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *length)
{
    (void)handle;
    const char *text = saved[slot(key)];
    if (!text[0]) return ESP_ERR_NVS_NOT_FOUND;
    const size_t needed = strlen(text) + 1;
    if (value && *length < needed) return ESP_ERR_INVALID_SIZE;
    if (value) memcpy(value, text, needed);
    *length = needed;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle;
    CHECK(strlen(value) <= GEO_CONFIG_JSON_MAX_BYTES);
    strcpy(pending[slot(key)], value);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (fail_commit) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 3; ++i) {
        if (pending[i][0]) strcpy(saved[i], pending[i]);
    }
    ++commits;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{
    (void)handle;
    memset(pending, 0, sizeof(pending));
}

static const char target_json[] =
    "{\"type\":\"Feature\",\"properties\":null,\"geometry\":{"
    "\"type\":\"Point\",\"coordinates\":[116.34,39.98]}}";
static const char disabled_json[] =
    "{\"type\":\"Feature\",\"properties\":{\"enabled\":false},\"geometry\":null}";
static const char legacy_target[] =
    "{\"schema\":1,\"type\":\"target\",\"id\":\"target\",\"name\":\"Old\","
    "\"enabled\":true,\"crs\":\"WGS84\",\"coordinate\":{\"lat\":39.98,\"lon\":116.34}}";
static const char legacy_fence[] =
    "{\"schema\":1,\"type\":\"polygon\",\"id\":\"fence2\",\"name\":\"Old GCJ\","
    "\"enabled\":true,\"crs\":\"GCJ02\",\"boundary\":\"inclusive\",\"vertices\":["
    "{\"lat\":39.98,\"lon\":116.33},{\"lat\":39.98,\"lon\":116.34},"
    "{\"lat\":39.99,\"lon\":116.34}]}";

static geo_fence_config_t parse_fence(const char *json)
{
    geo_fence_config_t fence;
    geo_validation_error_t error;
    const esp_err_t result = geo_config_parse_fence_json(json, strlen(json), 0, &fence, &error);
    if (result != ESP_OK) fprintf(stderr, "%s %s %s\n", error.code, error.path, error.message);
    CHECK(result == ESP_OK);
    return fence;
}

static void reject(const char *json, int target)
{
    geo_validation_error_t error;
    if (target) {
        geo_target_config_t out, before;
        memset(&out, 0xA5, sizeof(out));
        before = out;
        CHECK(geo_config_parse_target_json(json, strlen(json), &out, &error) != ESP_OK);
        CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    } else {
        geo_fence_config_t out, before;
        memset(&out, 0xA5, sizeof(out));
        before = out;
        CHECK(geo_config_parse_fence_json(json, strlen(json), 0, &out, &error) != ESP_OK);
        CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    }
    CHECK(error.code[0] != '\0');
}

static void reject_object(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL);
    reject(json, 0);
    cJSON_free(json);
    cJSON_Delete(root);
}

static void test_invalid(const char *sample)
{
    const char *bad_points[] = {
        "[181,0]", "[0,-91]", "[\"116\",39]", "[116]", "[116,39,0,1]",
        "[116,39,null]", "[1e999,39]", "[null,39]", "{\"lon\":116,\"lat\":39}"
    };
    char text[512];
    for (unsigned i = 0; i < sizeof(bad_points) / sizeof(bad_points[0]); ++i) {
        snprintf(text, sizeof(text), "{\"type\":\"Feature\",\"properties\":{},"
                 "\"geometry\":{\"type\":\"Point\",\"coordinates\":%s}}", bad_points[i]);
        reject(text, 1);
    }
    const char *bad_rings[] = {
        "[]", "[[0,0],[1,0],[1,1],[0,1]]", /* open */
        "[[0,0],[1,0],[0,0]]", /* too few */
        "[[0,0],[1,0],[1,1],[1,0],[0,0]]", /* duplicate */
        "[[0,0],[1,1],[2,2],[0,0]]", /* zero area */
        "[[0,0],[2,2],[0,2],[2,0],[0,0]]", /* bow-tie */
        "[[0,0],[3,0],[3,1],[0,0]]", /* excessive span */
        "[[0,0],[1,0],[1,1],[0.00000001,0]]", /* not exactly closed */
        "[[0,0],[0.00000001,0],[1,1],[0,0]]" /* E6 duplicate */
    };
    for (unsigned i = 0; i < sizeof(bad_rings) / sizeof(bad_rings[0]); ++i) {
        snprintf(text, sizeof(text), "{\"type\":\"Feature\",\"properties\":{},"
                 "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[%s]}}", bad_rings[i]);
        reject(text, 0);
    }
    reject(legacy_target, 1);
    reject(legacy_fence, 0);
    reject("{\"type\":\"FeatureCollection\",\"features\":[]}", 0);
    reject("{\"type\":\"Feature\",\"properties\":{},\"geometry\":null}", 1);
    reject("{\"type\":\"Feature\",\"properties\":null,\"properties\":{}}", 0);
    reject("{\"type\":\"Feature\",\"properties\":{\"name\":\"a\\u0000b\"},\"geometry\":null}", 0);
    snprintf(text, sizeof(text), "%s garbage", target_json);
    reject(text, 1);

    const char *bad_types[] = {"MultiPolygon", "LineString", "Point", "GeometryCollection"};
    for (unsigned i = 0; i < sizeof(bad_types) / sizeof(bad_types[0]); ++i) {
        cJSON *root = cJSON_Parse(sample);
        cJSON *geometry = cJSON_GetObjectItemCaseSensitive(root, "geometry");
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(geometry, "type", cJSON_CreateString(bad_types[i])));
        reject_object(root);
    }
    cJSON *root = cJSON_Parse(sample);
    cJSON *rings = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "geometry"), "coordinates");
    CHECK(cJSON_AddItemToArray(rings, cJSON_Duplicate(cJSON_GetArrayItem(rings, 0), 1)));
    reject_object(root); /* hole must not be silently discarded */
    root = cJSON_Parse(sample);
    CHECK(cJSON_AddStringToObject(root, "crs", "WGS84"));
    reject_object(root);
    root = cJSON_Parse(sample);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "properties"),
                                                "coordinateSystem", cJSON_CreateString("GCJ-02")));
    reject_object(root);
    root = cJSON_Parse(sample);
    cJSON *cursor = root;
    for (unsigned i = 0; i < 20; ++i) cursor = cJSON_AddObjectToObject(cursor, "nested");
    reject_object(root);
    root = cJSON_Parse(sample);
    char long_name[GEO_CONFIG_NAME_MAX_BYTES + 2];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "properties"),
                                                "name", cJSON_CreateString(long_name)));
    reject_object(root);
}

static unsigned allocation_attempt, fail_at, live_allocations;
static void *test_malloc(size_t size)
{
    if (allocation_attempt++ == fail_at) return NULL;
    void *result = malloc(size);
    if (result) ++live_allocations;
    return result;
}
static void test_free(void *ptr)
{
    if (ptr) { CHECK(live_allocations > 0); --live_allocations; free(ptr); }
}

static void test_allocation_failures(const char *sample, const geo_fence_config_t *fence,
                                     const geo_target_config_t *target)
{
    cJSON_Hooks hooks = {test_malloc, test_free};
    cJSON_InitHooks(&hooks);
    unsigned successes = 0;
    for (unsigned i = 0; i < 220; ++i) {
        geo_validation_error_t error;
        geo_fence_config_t output;
        fail_at = i;
        allocation_attempt = 0;
        if (geo_config_parse_fence_json(sample, strlen(sample), 0, &output, &error) == ESP_OK) ++successes;
        CHECK(live_allocations == 0);
        allocation_attempt = 0;
        char *json = geo_config_fence_to_json(fence, 0);
        if (json) ++successes;
        geo_config_json_free(json);
        CHECK(live_allocations == 0);
        allocation_attempt = 0;
        json = geo_config_target_to_json(target);
        if (json) ++successes;
        geo_config_json_free(json);
        CHECK(live_allocations == 0);
    }
    CHECK(successes > 0);
    cJSON_InitHooks(NULL);
}

static void test_vertex_limits(void)
{
    for (unsigned count = 16; count <= 17; ++count) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "Feature");
        cJSON_AddObjectToObject(root, "properties");
        cJSON *geometry = cJSON_AddObjectToObject(root, "geometry");
        cJSON_AddStringToObject(geometry, "type", "Polygon");
        cJSON *rings = cJSON_AddArrayToObject(geometry, "coordinates");
        cJSON *ring = cJSON_CreateArray();
        CHECK(cJSON_AddItemToArray(rings, ring));
        for (unsigned i = 0; i < count; ++i) {
            const double angle = i * 6.283185307179586 / count;
            const double point[] = {116 + 0.01 * cos(angle), 39 + 0.01 * sin(angle)};
            CHECK(cJSON_AddItemToArray(ring, cJSON_CreateDoubleArray(point, 2)));
        }
        CHECK(cJSON_AddItemToArray(ring, cJSON_Duplicate(cJSON_GetArrayItem(ring, 0), 1)));
        char *json = cJSON_PrintUnformatted(root);
        if (count == 16) CHECK(parse_fence(json).vertex_count == 16);
        else reject(json, 0);
        cJSON_free(json);
        cJSON_Delete(root);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file != NULL);
    char sample[GEO_CONFIG_JSON_MAX_BYTES + 2];
    const size_t length = fread(sample, 1, sizeof(sample) - 1, file);
    CHECK(!ferror(file) && length > 0 && length <= GEO_CONFIG_JSON_MAX_BYTES);
    fclose(file);
    sample[length] = '\0';
    geo_fence_config_t fence = parse_fence(sample);
    CHECK(fence.configured && fence.enabled && fence.crs == GEO_CRS_WGS84);
    CHECK(fence.vertex_count == 7);
    CHECK(strcmp(fence.name, "北京航空航天大学(学院路校区)围栏") == 0);
    CHECK(fence.vertices[0].longitude_e6 == 116334672 && fence.vertices[0].latitude_e6 == 39984601);
    geo_validation_error_t error;
    geo_target_config_t target;
    CHECK(geo_config_parse_target_json(target_json, strlen(target_json), &target, &error) == ESP_OK);
    CHECK(target.has_geometry && target.enabled && strcmp(target.name, "Target") == 0);
    char *json = geo_config_fence_to_json(&fence, 0);
    CHECK(json && strstr(json, "Feature") && !strstr(json, "areaSquareMeters"));
    geo_fence_config_t roundtrip = parse_fence(json);
    CHECK(roundtrip.vertex_count == 7 && strcmp(roundtrip.name, fence.name) == 0);
    int64_t area = 0;
    for (unsigned i = 0; i < roundtrip.vertex_count; ++i) {
        geo_point_e6_t a = roundtrip.vertices[i], b = roundtrip.vertices[(i + 1) % roundtrip.vertex_count];
        area += (int64_t)a.longitude_e6 * b.latitude_e6 - (int64_t)b.longitude_e6 * a.latitude_e6;
    }
    CHECK(area > 0);
    geo_config_json_free(json);
    const char altitude[] = "{\"type\":\"Feature\",\"properties\":{},\"geometry\":{\"type\":\"Point\",\"coordinates\":[116.34,39.98,123]}}";
    geo_target_config_t elevated;
    CHECK(geo_config_parse_target_json(altitude, strlen(altitude), &elevated, &error) == ESP_OK);
    CHECK(elevated.coordinate.latitude_e6 == target.coordinate.latitude_e6);

    test_invalid(sample);
    test_vertex_limits();
    test_allocation_failures(sample, &fence, &target);

    geo_config_manager_t manager, reboot;
    CHECK(geo_config_manager_init(&manager, true) == ESP_OK);
    CHECK(manager.current.target.has_geometry && manager.current.target.enabled);
    CHECK(geo_config_manager_set_target(&manager, &target) == ESP_OK);
    CHECK(geo_config_manager_set_fence(&manager, 0, &fence) == ESP_OK);
    CHECK(strstr(saved[0], "Feature") && strstr(saved[1], "Polygon"));
    const unsigned commits_before = commits;
    CHECK(geo_config_manager_init(&reboot, true) == ESP_OK);
    CHECK(commits == commits_before); /* boot never rewrites NVS */
    CHECK(reboot.current.fences[0].vertex_count == 7 && reboot.current.target.has_geometry);
    geo_config_snapshot_t snapshot;
    geo_config_manager_get(&reboot, &snapshot);
    geo_fence_state_t states[GEO_FENCE_COUNT];
    geo_config_evaluate_fences(&snapshot, true, 39.981, 116.34, states);
    CHECK(states[0] == GEO_FENCE_STATE_INSIDE);
    geo_config_evaluate_fences(&snapshot, true, 39.99, 116.34, states);
    CHECK(states[0] == GEO_FENCE_STATE_OUTSIDE);
    geo_config_evaluate_fences(&snapshot, true, 39.984601, 116.334672, states);
    CHECK(states[0] == GEO_FENCE_STATE_INSIDE); /* E6 boundary is inclusive */
    geo_config_evaluate_fences(&snapshot, false, 39.98, 116.34, states);
    CHECK(states[0] == GEO_FENCE_STATE_UNKNOWN);
    double olat, olon, tlat, tlon;
    CHECK(geo_config_resolve_target(&snapshot, 39.981, 116.341, &olat, &olon, &tlat, &tlon));
    CHECK(olat == 39.981 && olon == 116.341 && fabs(tlat - 39.98) < 1e-8);

    fail_commit = 1;
    geo_fence_config_t disabled = parse_fence(disabled_json);
    CHECK(geo_config_manager_set_fence(&reboot, 0, &disabled) != ESP_OK);
    CHECK(reboot.current.fences[0].enabled && reboot.current.revision == snapshot.revision);
    fail_commit = 0;
    CHECK(geo_config_manager_set_fence(&reboot, 1, &disabled) == ESP_OK);
    CHECK(geo_config_parse_target_json(disabled_json, strlen(disabled_json), &elevated, &error) == ESP_OK);
    CHECK(!elevated.enabled && !elevated.has_geometry);
    CHECK(geo_config_manager_set_target(&reboot, &elevated) == ESP_OK);
    CHECK(geo_config_manager_init(&reboot, true) == ESP_OK);
    CHECK(!reboot.current.target.enabled && !reboot.current.target.has_geometry);
    CHECK(reboot.current.fences[0].enabled && !reboot.current.fences[1].enabled);
    CHECK(strstr(saved[0], "\"geometry\":null") && strstr(saved[2], "\"geometry\":null"));

    strcpy(saved[0], legacy_target);
    strcpy(saved[2], legacy_fence);
    const unsigned old_commits = commits;
    CHECK(geo_config_manager_init(&reboot, true) == ESP_OK);
    CHECK(commits == old_commits && strcmp(saved[0], legacy_target) == 0);
    CHECK(reboot.current.target.has_geometry && strcmp(reboot.current.target.name, "Old") == 0);
    CHECK(reboot.current.fences[1].crs == GEO_CRS_GCJ02 && reboot.current.fences[1].enabled);
    CHECK(geo_config_fence_to_json(&reboot.current.fences[1], 1) == NULL);
    CHECK(geo_config_manager_set_fence(&reboot, 1, &fence) == ESP_OK);
    CHECK(reboot.current.fences[1].crs == GEO_CRS_WGS84);
    CHECK(geo_config_manager_init(&manager, false) == ESP_OK);
    CHECK(geo_config_manager_set_target(&manager, &target) == ESP_ERR_INVALID_STATE);
    printf("PASS: %u checks; supplied Beihang polygon = 7 vertices; parsing, roundtrip, geometry, allocation failures, NVS and legacy migration.\n", checks);
    return 0;
}
