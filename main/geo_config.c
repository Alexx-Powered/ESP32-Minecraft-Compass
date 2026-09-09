#include "geo_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

#include "app_config.h"

static const char *TAG = "geo_config";
static const char *NVS_NAMESPACE = "mc_geo";
static const char *NVS_TARGET_KEY = "target";
static const char *NVS_FENCE_KEYS[GEO_FENCE_COUNT] = {"fence1", "fence2"};

#define GEO_MAX_POLYGON_SPAN_E6 2000000
#define GEO_JSON_MAX_DEPTH 16U

static void set_error(geo_validation_error_t *error,
                      const char *code,
                      const char *path,
                      const char *message)
{
    if (error == NULL) {
        return;
    }
    (void)snprintf(error->code, sizeof(error->code), "%s", code);
    (void)snprintf(error->path, sizeof(error->path), "%s", path);
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static bool utf8_is_valid(const char *text)
{
    if (text == NULL) {
        return false;
    }

    const uint8_t *cursor = (const uint8_t *)text;
    while (*cursor != 0U) {
        if (*cursor <= 0x7FU) {
            cursor++;
            continue;
        }

        uint32_t codepoint = 0U;
        size_t continuation_count = 0U;
        if ((*cursor & 0xE0U) == 0xC0U) {
            codepoint = *cursor & 0x1FU;
            continuation_count = 1U;
            if (codepoint < 2U) {
                return false;
            }
        } else if ((*cursor & 0xF0U) == 0xE0U) {
            codepoint = *cursor & 0x0FU;
            continuation_count = 2U;
        } else if ((*cursor & 0xF8U) == 0xF0U) {
            codepoint = *cursor & 0x07U;
            continuation_count = 3U;
        } else {
            return false;
        }

        cursor++;
        for (size_t index = 0U; index < continuation_count; ++index) {
            if ((cursor[index] & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (cursor[index] & 0x3FU);
        }
        if ((continuation_count == 2U && codepoint < 0x800U) ||
            (continuation_count == 3U && codepoint < 0x10000U) ||
            codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        cursor += continuation_count;
    }
    return true;
}

static bool key_is_allowed(const char *key,
                           const char *const *allowed,
                           size_t allowed_count)
{
    for (size_t index = 0U; index < allowed_count; ++index) {
        if (strcmp(key, allowed[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool object_keys_are_valid(const cJSON *object,
                                  const char *const *allowed,
                                  size_t allowed_count,
                                  const char *path,
                                  geo_validation_error_t *error)
{
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string == NULL ||
            !key_is_allowed(item->string, allowed, allowed_count)) {
            char field_path[64];
            (void)snprintf(field_path, sizeof(field_path), "%s.%s", path,
                           item->string == NULL ? "?" : item->string);
            set_error(error, "JSON_UNKNOWN_FIELD", field_path,
                      "JSON contains an unsupported field");
            return false;
        }
        for (const cJSON *other = item->next;
             other != NULL; other = other->next) {
            if (other->string != NULL &&
                strcmp(item->string, other->string) == 0) {
                char field_path[64];
                (void)snprintf(field_path, sizeof(field_path), "%s.%s", path,
                               item->string);
                set_error(error, "JSON_DUPLICATE_FIELD", field_path,
                          "JSON field is duplicated");
                return false;
            }
        }
    }
    return true;
}

/* Foreign GeoJSON metadata is allowed, but must not introduce ambiguous keys
 * or non-finite values. A pre-scan bounds nesting before cJSON allocates it. */
static bool json_tree_is_valid(const cJSON *node,
                              geo_validation_error_t *error)
{
    if ((cJSON_IsString(node) && !utf8_is_valid(node->valuestring)) ||
        (cJSON_IsNumber(node) && !isfinite(node->valuedouble))) {
        set_error(error, "JSON_VALUE", "$", "Invalid UTF-8 or non-finite number");
        return false;
    }
    for (const cJSON *item = node->child; item != NULL; item = item->next) {
        if (cJSON_IsObject(node)) {
            if (!utf8_is_valid(item->string)) {
                set_error(error, "JSON_KEY", "$", "Invalid UTF-8 field name");
                return false;
            }
            for (const cJSON *other = item->next; other != NULL;
                 other = other->next) {
                if (other->string != NULL &&
                    strcmp(item->string, other->string) == 0) {
                    set_error(error, "JSON_DUPLICATE_FIELD", "$",
                              "JSON field is duplicated");
                    return false;
                }
            }
        }
        if (!json_tree_is_valid(item, error)) {
            return false;
        }
    }
    return true;
}

static bool json_text_is_bounded(const char *json, size_t length)
{
    bool in_string = false;
    unsigned depth = 0U;
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char value = (unsigned char)json[i];
        if (value == 0U) {
            return false;
        }
        if (in_string && value == '\\') {
            /* cJSON represents strings as NUL-terminated C strings. Reject an
             * escaped U+0000 so a label/key cannot be silently truncated. */
            if (i + 5U < length && json[i + 1U] == 'u' &&
                memcmp(json + i + 2U, "0000", 4U) == 0) {
                return false;
            }
            ++i;
        } else if (value == '"') {
            in_string = !in_string;
        } else if (in_string && value < 0x20U) {
            return false;
        } else if (!in_string && (value == '{' || value == '[')) {
            if (++depth > GEO_JSON_MAX_DEPTH) {
                return false;
            }
        } else if (!in_string && (value == '}' || value == ']')) {
            if (depth == 0U) {
                return false;
            }
            --depth;
        }
    }
    return depth == 0U && !in_string;
}

static cJSON *parse_json_root(const char *json,
                              size_t json_length,
                              geo_validation_error_t *error)
{
    if (json == NULL || json_length == 0U ||
        json_length > GEO_CONFIG_JSON_MAX_BYTES) {
        set_error(error, "JSON_SIZE", "$",
                  "JSON must contain 1 to 3072 bytes");
        return NULL;
    }
    if (!json_text_is_bounded(json, json_length)) {
        set_error(error, "JSON_SYNTAX", "$",
                  "Invalid JSON text, embedded NUL, or nesting exceeds 16");
        return NULL;
    }

    char *terminated = malloc(json_length + 1U);
    if (terminated == NULL) {
        set_error(error, "NO_MEMORY", "$", "Not enough memory to parse JSON");
        return NULL;
    }
    memcpy(terminated, json, json_length);
    terminated[json_length] = '\0';

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        terminated, json_length + 1U, &parse_end, true);
    const bool complete = parse_end == terminated + json_length;
    free(terminated);
    if (root == NULL || !complete) {
        cJSON_Delete(root);
        set_error(error, "JSON_SYNTAX", "$", "JSON syntax is invalid");
        return NULL;
    }
    if (!json_tree_is_valid(root, error)) {
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_error(error, "JSON_TYPE", "$", "JSON root must be an object");
        return NULL;
    }
    return root;
}

static bool require_schema(const cJSON *root,
                           geo_validation_error_t *error)
{
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) ||
        schema->valuedouble != (double)GEO_CONFIG_SCHEMA_VERSION) {
        set_error(error, "SCHEMA_VERSION", "$.schema",
                  "schema must be the integer 1");
        return false;
    }
    return true;
}

static bool require_exact_string(const cJSON *root,
                                 const char *key,
                                 const char *expected,
                                 const char *path,
                                 geo_validation_error_t *error)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strcmp(item->valuestring, expected) != 0) {
        set_error(error, "FIELD_VALUE", path,
                  "field has an unexpected value");
        return false;
    }
    return true;
}

static bool parse_name(const cJSON *root,
                       bool enabled,
                       char output[GEO_CONFIG_NAME_MAX_BYTES + 1U],
                       geo_validation_error_t *error)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || name->valuestring == NULL) {
        set_error(error, "NAME_TYPE", "$.name", "name must be a string");
        return false;
    }
    const size_t byte_length = strlen(name->valuestring);
    if (byte_length > GEO_CONFIG_NAME_MAX_BYTES ||
        (enabled && byte_length == 0U) || !utf8_is_valid(name->valuestring)) {
        set_error(error, "NAME_INVALID", "$.name",
                  "enabled names must be valid UTF-8 and 1 to 128 bytes");
        return false;
    }
    memcpy(output, name->valuestring, byte_length + 1U);
    return true;
}

static bool parse_enabled(const cJSON *root,
                          bool *enabled,
                          geo_validation_error_t *error)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (!cJSON_IsBool(item)) {
        set_error(error, "ENABLED_TYPE", "$.enabled",
                  "enabled must be true or false");
        return false;
    }
    *enabled = cJSON_IsTrue(item);
    return true;
}

static bool parse_crs(const cJSON *root,
                      geo_crs_t *crs,
                      geo_validation_error_t *error)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "crs");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        set_error(error, "CRS_TYPE", "$.crs", "crs must be a string");
        return false;
    }
    if (strcmp(item->valuestring, "WGS84") == 0) {
        *crs = GEO_CRS_WGS84;
        return true;
    }
    if (strcmp(item->valuestring, "GCJ02") == 0) {
        *crs = GEO_CRS_GCJ02;
        return true;
    }
    set_error(error, "CRS_VALUE", "$.crs",
              "crs must be WGS84 or GCJ02");
    return false;
}

static bool parse_coordinate(const cJSON *object,
                             const char *path,
                             geo_point_e6_t *point,
                             geo_validation_error_t *error)
{
    static const char *const allowed[] = {"lat", "lon"};
    if (!cJSON_IsObject(object) ||
        !object_keys_are_valid(object, allowed,
                               sizeof(allowed) / sizeof(allowed[0]),
                               path, error)) {
        if (!cJSON_IsObject(object)) {
            set_error(error, "COORDINATE_TYPE", path,
                      "coordinate must be an object");
        }
        return false;
    }

    const cJSON *latitude = cJSON_GetObjectItemCaseSensitive(object, "lat");
    const cJSON *longitude = cJSON_GetObjectItemCaseSensitive(object, "lon");
    char latitude_path[64];
    char longitude_path[64];
    (void)snprintf(latitude_path, sizeof(latitude_path), "%s.lat", path);
    (void)snprintf(longitude_path, sizeof(longitude_path), "%s.lon", path);

    if (!cJSON_IsNumber(latitude) || !isfinite(latitude->valuedouble) ||
        latitude->valuedouble < -90.0 || latitude->valuedouble > 90.0) {
        set_error(error, "GEO_RANGE", latitude_path,
                  "latitude must be a finite number from -90 to 90");
        return false;
    }
    if (!cJSON_IsNumber(longitude) || !isfinite(longitude->valuedouble) ||
        longitude->valuedouble < -180.0 || longitude->valuedouble > 180.0) {
        set_error(error, "GEO_RANGE", longitude_path,
                  "longitude must be a finite number from -180 to 180");
        return false;
    }

    point->latitude_e6 = (int32_t)llround(latitude->valuedouble * 1000000.0);
    point->longitude_e6 = (int32_t)llround(longitude->valuedouble * 1000000.0);
    return true;
}

static int orientation(geo_point_e6_t a,
                       geo_point_e6_t b,
                       geo_point_e6_t c)
{
    const int64_t value =
        (int64_t)(b.longitude_e6 - a.longitude_e6) *
            (c.latitude_e6 - a.latitude_e6) -
        (int64_t)(b.latitude_e6 - a.latitude_e6) *
            (c.longitude_e6 - a.longitude_e6);
    return (value > 0) - (value < 0);
}

static bool point_on_segment(geo_point_e6_t point,
                             geo_point_e6_t a,
                             geo_point_e6_t b)
{
    if (orientation(a, b, point) != 0) {
        return false;
    }
    const int32_t minimum_latitude =
        a.latitude_e6 < b.latitude_e6 ? a.latitude_e6 : b.latitude_e6;
    const int32_t maximum_latitude =
        a.latitude_e6 > b.latitude_e6 ? a.latitude_e6 : b.latitude_e6;
    const int32_t minimum_longitude =
        a.longitude_e6 < b.longitude_e6 ? a.longitude_e6 : b.longitude_e6;
    const int32_t maximum_longitude =
        a.longitude_e6 > b.longitude_e6 ? a.longitude_e6 : b.longitude_e6;
    return point.latitude_e6 >= minimum_latitude &&
           point.latitude_e6 <= maximum_latitude &&
           point.longitude_e6 >= minimum_longitude &&
           point.longitude_e6 <= maximum_longitude;
}

static bool segments_intersect(geo_point_e6_t a,
                               geo_point_e6_t b,
                               geo_point_e6_t c,
                               geo_point_e6_t d)
{
    const int o1 = orientation(a, b, c);
    const int o2 = orientation(a, b, d);
    const int o3 = orientation(c, d, a);
    const int o4 = orientation(c, d, b);
    if (o1 != o2 && o3 != o4) {
        return true;
    }
    return (o1 == 0 && point_on_segment(c, a, b)) ||
           (o2 == 0 && point_on_segment(d, a, b)) ||
           (o3 == 0 && point_on_segment(a, c, d)) ||
           (o4 == 0 && point_on_segment(b, c, d));
}

static bool validate_fence_geometry(geo_fence_config_t *fence,
                                    const char *path,
                                    geo_validation_error_t *error)
{
    if (fence->vertex_count < 3U ||
        fence->vertex_count > GEO_FENCE_MAX_VERTICES) {
        set_error(error, "VERTEX_COUNT", path,
                  "an enabled polygon needs 3 to 16 vertices");
        return false;
    }

    fence->minimum_latitude_e6 = fence->maximum_latitude_e6 =
        fence->vertices[0].latitude_e6;
    fence->minimum_longitude_e6 = fence->maximum_longitude_e6 =
        fence->vertices[0].longitude_e6;
    for (size_t i = 0U; i < fence->vertex_count; ++i) {
        const geo_point_e6_t point = fence->vertices[i];
        if (point.latitude_e6 < fence->minimum_latitude_e6) {
            fence->minimum_latitude_e6 = point.latitude_e6;
        }
        if (point.latitude_e6 > fence->maximum_latitude_e6) {
            fence->maximum_latitude_e6 = point.latitude_e6;
        }
        if (point.longitude_e6 < fence->minimum_longitude_e6) {
            fence->minimum_longitude_e6 = point.longitude_e6;
        }
        if (point.longitude_e6 > fence->maximum_longitude_e6) {
            fence->maximum_longitude_e6 = point.longitude_e6;
        }
        for (size_t j = i + 1U; j < fence->vertex_count; ++j) {
            if (point.latitude_e6 == fence->vertices[j].latitude_e6 &&
                point.longitude_e6 == fence->vertices[j].longitude_e6) {
                set_error(error, "DUPLICATE_VERTEX", path,
                          "polygon vertices must be unique");
                return false;
            }
        }
    }

    const int64_t latitude_span =
        (int64_t)fence->maximum_latitude_e6 - fence->minimum_latitude_e6;
    const int64_t longitude_span =
        (int64_t)fence->maximum_longitude_e6 - fence->minimum_longitude_e6;
    if (latitude_span <= 0 || longitude_span <= 0) {
        set_error(error, "POLYGON_AREA", path,
                  "polygon must span both latitude and longitude");
        return false;
    }
    if (latitude_span > GEO_MAX_POLYGON_SPAN_E6 ||
        longitude_span > GEO_MAX_POLYGON_SPAN_E6) {
        set_error(error, "POLYGON_SPAN", path,
                  "v1 polygons must fit inside a 2 degree bounding box");
        return false;
    }

    int64_t signed_double_area = 0;
    for (size_t i = 0U; i < fence->vertex_count; ++i) {
        const size_t next = (i + 1U) % fence->vertex_count;
        signed_double_area +=
            (int64_t)fence->vertices[i].longitude_e6 *
                fence->vertices[next].latitude_e6 -
            (int64_t)fence->vertices[next].longitude_e6 *
                fence->vertices[i].latitude_e6;
    }
    if (signed_double_area == 0) {
        set_error(error, "POLYGON_AREA", path,
                  "polygon area must be greater than zero");
        return false;
    }

    for (size_t i = 0U; i < fence->vertex_count; ++i) {
        const size_t i_next = (i + 1U) % fence->vertex_count;
        for (size_t j = i + 1U; j < fence->vertex_count; ++j) {
            const size_t j_next = (j + 1U) % fence->vertex_count;
            if (i == j || i_next == j || j_next == i) {
                continue;
            }
            if (segments_intersect(fence->vertices[i],
                                   fence->vertices[i_next],
                                   fence->vertices[j],
                                   fence->vertices[j_next])) {
                set_error(error, "POLYGON_SELF_INTERSECTION", path,
                          "polygon edges must not cross");
                return false;
            }
        }
    }
    return true;
}

/* Read-only compatibility for records saved by firmware before GeoJSON. */
static esp_err_t parse_legacy_target_json(const char *json,
                                       size_t json_length,
                                       geo_target_config_t *target,
                                       geo_validation_error_t *error)
{
    if (target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }

    cJSON *root = parse_json_root(json, json_length, error);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char *const allowed[] = {
        "schema", "type", "id", "name", "enabled", "crs", "coordinate",
    };
    bool enabled = false;
    geo_target_config_t candidate = {0};
    const cJSON *coordinate = cJSON_GetObjectItemCaseSensitive(root,
                                                               "coordinate");
    const bool valid =
        object_keys_are_valid(root, allowed,
                              sizeof(allowed) / sizeof(allowed[0]), "$", error) &&
        require_schema(root, error) &&
        require_exact_string(root, "type", "target", "$.type", error) &&
        require_exact_string(root, "id", "target", "$.id", error) &&
        parse_enabled(root, &enabled, error) &&
        parse_name(root, enabled, candidate.name, error) &&
        parse_crs(root, &candidate.crs, error) &&
        parse_coordinate(coordinate, "$.coordinate",
                         &candidate.coordinate, error);
    cJSON_Delete(root);
    if (!valid) {
        return ESP_ERR_INVALID_ARG;
    }
    candidate.configured = true;
    candidate.enabled = enabled;
    candidate.has_geometry = true;
    *target = candidate;
    return ESP_OK;
}

static esp_err_t parse_legacy_fence_json(const char *json,
                                      size_t json_length,
                                      size_t fence_index,
                                      geo_fence_config_t *fence,
                                      geo_validation_error_t *error)
{
    if (fence == NULL || fence_index >= GEO_FENCE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }

    cJSON *root = parse_json_root(json, json_length, error);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char *const allowed[] = {
        "schema", "type", "id", "name", "enabled", "crs",
        "boundary", "vertices",
    };
    char expected_id[8];
    (void)snprintf(expected_id, sizeof(expected_id), "fence%u",
                   (unsigned)(fence_index + 1U));
    bool enabled = false;
    geo_fence_config_t candidate = {0};
    const cJSON *vertices = cJSON_GetObjectItemCaseSensitive(root, "vertices");

    bool valid =
        object_keys_are_valid(root, allowed,
                              sizeof(allowed) / sizeof(allowed[0]), "$", error) &&
        require_schema(root, error) &&
        require_exact_string(root, "type", "polygon", "$.type", error) &&
        require_exact_string(root, "id", expected_id, "$.id", error) &&
        require_exact_string(root, "boundary", "inclusive",
                             "$.boundary", error) &&
        parse_enabled(root, &enabled, error) &&
        parse_name(root, enabled, candidate.name, error) &&
        parse_crs(root, &candidate.crs, error);

    if (valid && !cJSON_IsArray(vertices)) {
        set_error(error, "VERTICES_TYPE", "$.vertices",
                  "vertices must be an array");
        valid = false;
    }
    const int array_size = valid ? cJSON_GetArraySize(vertices) : 0;
    if (valid && (array_size < 0 ||
                  array_size > (int)GEO_FENCE_MAX_VERTICES ||
                  (enabled && array_size < 3) ||
                  (!enabled && array_size != 0 && array_size < 3))) {
        set_error(error, "VERTEX_COUNT", "$.vertices",
                  "vertices must be empty when disabled or contain 3 to 16 points");
        valid = false;
    }

    if (valid) {
        candidate.vertex_count = (uint8_t)array_size;
        for (int index = 0; index < array_size; ++index) {
            char path[64];
            (void)snprintf(path, sizeof(path), "$.vertices[%d]", index);
            if (!parse_coordinate(cJSON_GetArrayItem(vertices, index), path,
                                  &candidate.vertices[index], error)) {
                valid = false;
                break;
            }
        }
    }
    if (valid && candidate.vertex_count >= 3U &&
        !validate_fence_geometry(&candidate, "$.vertices", error)) {
        valid = false;
    }
    cJSON_Delete(root);
    if (!valid) {
        return ESP_ERR_INVALID_ARG;
    }
    candidate.configured = true;
    candidate.enabled = enabled;
    *fence = candidate;
    return ESP_OK;
}

static bool parse_feature(const cJSON *root,
                          const char *geometry_type,
                          const char *default_name,
                          char name[GEO_CONFIG_NAME_MAX_BYTES + 1U],
                          bool *enabled,
                          const cJSON **geometry,
                          geo_validation_error_t *error)
{
    if (!require_exact_string(root, "type", "Feature", "$.type", error)) {
        return false;
    }
    /* RFC 7946 has a fixed WGS84 CRS. sourceCoordinateSystem is provenance,
     * not the CRS of geometry.coordinates, and is intentionally ignored. */
    if (cJSON_GetObjectItemCaseSensitive(root, "crs") != NULL) {
        set_error(error, "GEOJSON_CRS", "$.crs",
                  "Only WGS84 GeoJSON is accepted; remove legacy crs after conversion");
        return false;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id != NULL && !cJSON_IsString(id) && !cJSON_IsNumber(id)) {
        set_error(error, "GEOJSON_ID", "$.id", "Feature id must be a string or number");
        return false;
    }
    const cJSON *properties = cJSON_GetObjectItemCaseSensitive(root, "properties");
    if (!cJSON_IsObject(properties) && !cJSON_IsNull(properties)) {
        set_error(error, "GEOJSON_PROPERTIES", "$.properties",
                  "Feature properties must be an object or null");
        return false;
    }
    const cJSON *crs = cJSON_GetObjectItemCaseSensitive(properties, "coordinateSystem");
    if (crs != NULL && (!cJSON_IsString(crs) ||
                       strcmp(crs->valuestring, "WGS84") != 0)) {
        set_error(error, "GEOJSON_CRS", "$.properties.coordinateSystem",
                  "Coordinates must already be WGS84");
        return false;
    }
    const cJSON *enable_item = cJSON_GetObjectItemCaseSensitive(properties, "enabled");
    if (enable_item != NULL && !cJSON_IsBool(enable_item)) {
        set_error(error, "ENABLED_TYPE", "$.properties.enabled",
                  "enabled must be true or false when supplied");
        return false;
    }
    *enabled = enable_item == NULL || cJSON_IsTrue(enable_item);
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(properties, "name");
    if (name_item != NULL && !cJSON_IsNull(name_item) && !cJSON_IsString(name_item)) {
        set_error(error, "NAME_TYPE", "$.properties.name", "name must be a string or null");
        return false;
    }
    const char *label = cJSON_IsString(name_item) ? name_item->valuestring : default_name;
    const size_t length = strlen(label);
    if (length > GEO_CONFIG_NAME_MAX_BYTES || (*enabled && length == 0U)) {
        set_error(error, "NAME_INVALID", "$.properties.name",
                  "enabled names must contain 1 to 128 UTF-8 bytes");
        return false;
    }
    memcpy(name, label, length + 1U);
    *geometry = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    if (cJSON_IsNull(*geometry) && !*enabled) {
        return true;
    }
    if (!cJSON_IsObject(*geometry)) {
        set_error(error, "GEOJSON_GEOMETRY", "$.geometry",
                  "A geometry is required; null is allowed only when enabled is false");
        return false;
    }
    return require_exact_string(*geometry, "type", geometry_type,
                                 "$.geometry.type", error);
}

static bool parse_position(const cJSON *position, const char *path,
                           geo_point_e6_t *point, geo_validation_error_t *error)
{
    const int count = cJSON_IsArray(position) ? cJSON_GetArraySize(position) : 0;
    if (count != 2 && count != 3) {
        set_error(error, "GEOJSON_POSITION", path,
                  "Position must be [longitude, latitude] with optional altitude");
        return false;
    }
    const cJSON *longitude = cJSON_GetArrayItem(position, 0);
    const cJSON *latitude = cJSON_GetArrayItem(position, 1);
    const cJSON *altitude = cJSON_GetArrayItem(position, 2);
    if (!cJSON_IsNumber(longitude) || !isfinite(longitude->valuedouble) ||
        longitude->valuedouble < -180.0 || longitude->valuedouble > 180.0 ||
        !cJSON_IsNumber(latitude) || !isfinite(latitude->valuedouble) ||
        latitude->valuedouble < -90.0 || latitude->valuedouble > 90.0 ||
        (count == 3 && (!cJSON_IsNumber(altitude) || !isfinite(altitude->valuedouble)))) {
        set_error(error, "GEO_RANGE", path,
                  "Expected finite longitude [-180,180], latitude [-90,90], optional altitude");
        return false;
    }
    /* Navigation is two-dimensional; altitude is validated but not stored. */
    point->longitude_e6 = (int32_t)llround(longitude->valuedouble * 1000000.0);
    point->latitude_e6 = (int32_t)llround(latitude->valuedouble * 1000000.0);
    return true;
}

static bool positions_match(const cJSON *first, const cJSON *last)
{
    const int count = cJSON_GetArraySize(first);
    if (count != cJSON_GetArraySize(last)) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (cJSON_GetArrayItem(first, i)->valuedouble !=
            cJSON_GetArrayItem(last, i)->valuedouble) {
            return false;
        }
    }
    return true;
}

esp_err_t geo_config_parse_target_json(const char *json, size_t json_length,
                                       geo_target_config_t *target,
                                       geo_validation_error_t *error)
{
    if (target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    cJSON *root = parse_json_root(json, json_length, error);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    geo_target_config_t candidate = {.crs = GEO_CRS_WGS84};
    const cJSON *geometry = NULL;
    bool valid = parse_feature(root, "Point", "Target", candidate.name,
                                &candidate.enabled, &geometry, error);
    /* "role" is optional GeoJSON application metadata. When a generator
     * supplies it, bind it to this endpoint so a Fence-labelled Feature is
     * not silently accepted as a navigation target. Other provenance fields
     * remain accepted metadata and do not alter WGS84 coordinates. */
    const cJSON *properties = cJSON_GetObjectItemCaseSensitive(root,
                                                                "properties");
    const cJSON *role = cJSON_IsObject(properties)
        ? cJSON_GetObjectItemCaseSensitive(properties, "role") : NULL;
    if (valid && role != NULL &&
        (!cJSON_IsString(role) || strcmp(role->valuestring, "Target") != 0)) {
        set_error(error, "TARGET_ROLE", "$.properties.role",
                  "Target role must be the string Target when supplied");
        valid = false;
    }
    if (valid && !cJSON_IsNull(geometry)) {
        valid = parse_position(cJSON_GetObjectItemCaseSensitive(geometry, "coordinates"),
                               "$.geometry.coordinates", &candidate.coordinate, error);
        candidate.has_geometry = valid;
    }
    cJSON_Delete(root);
    if (!valid) {
        return ESP_ERR_INVALID_ARG;
    }
    candidate.configured = true;
    *target = candidate;
    return ESP_OK;
}

esp_err_t geo_config_parse_fence_json(const char *json, size_t json_length,
                                      size_t fence_index, geo_fence_config_t *fence,
                                      geo_validation_error_t *error)
{
    if (fence == NULL || fence_index >= GEO_FENCE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    cJSON *root = parse_json_root(json, json_length, error);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    geo_fence_config_t candidate = {.crs = GEO_CRS_WGS84};
    char default_name[16];
    (void)snprintf(default_name, sizeof(default_name), "Fence %u", (unsigned)fence_index + 1U);
    const cJSON *geometry = NULL;
    bool valid = parse_feature(root, "Polygon", default_name, candidate.name,
                                &candidate.enabled, &geometry, error);
    if (valid && !cJSON_IsNull(geometry)) {
        const cJSON *rings = cJSON_GetObjectItemCaseSensitive(geometry, "coordinates");
        if (!cJSON_IsArray(rings) || cJSON_GetArraySize(rings) != 1) {
            set_error(error, "GEOJSON_RINGS", "$.geometry.coordinates",
                      "Only a single exterior Polygon ring is supported; holes are not supported");
            valid = false;
        }
        const cJSON *ring = valid ? cJSON_GetArrayItem(rings, 0) : NULL;
        const int count = cJSON_IsArray(ring) ? cJSON_GetArraySize(ring) : 0;
        if (valid && (count < 4 || count > (int)GEO_FENCE_MAX_VERTICES + 1)) {
            set_error(error, "VERTEX_COUNT", "$.geometry.coordinates[0]",
                      "Polygon needs 3 to 16 vertices plus one repeated closing position");
            valid = false;
        }
        if (valid) {
            candidate.vertex_count = (uint8_t)(count - 1);
            geo_point_e6_t closing_point;
            for (int i = 0; valid && i < count; ++i) {
                char path[64];
                (void)snprintf(path, sizeof(path), "$.geometry.coordinates[0][%d]", i);
                valid = parse_position(cJSON_GetArrayItem(ring, i), path,
                                       i == count - 1 ? &closing_point : &candidate.vertices[i],
                                       error);
            }
            /* Check exact input positions before E6 rounding; near-coincident
             * endpoints must not accidentally turn an open ring into a closed one. */
            if (valid && !positions_match(cJSON_GetArrayItem(ring, 0),
                                         cJSON_GetArrayItem(ring, count - 1))) {
                set_error(error, "GEOJSON_RING_OPEN", "$.geometry.coordinates[0]",
                          "The final position must repeat the first position exactly");
                valid = false;
            }
            if (valid) {
                valid = validate_fence_geometry(&candidate, "$.geometry.coordinates[0]", error);
            }
        }
    }
    cJSON_Delete(root);
    if (!valid) {
        return ESP_ERR_INVALID_ARG;
    }
    candidate.configured = true;
    *fence = candidate;
    return ESP_OK;
}

const char *geo_config_crs_name(geo_crs_t crs)
{
    return crs == GEO_CRS_GCJ02 ? "GCJ02" : "WGS84";
}

static cJSON *position_to_json(geo_point_e6_t point)
{
    const double values[2] = {(double)point.longitude_e6 / 1000000.0,
                              (double)point.latitude_e6 / 1000000.0};
    return cJSON_CreateDoubleArray(values, 2);
}

/* Takes ownership of geometry on both success and failure. Export only the
 * navigation fields; source/area/id metadata never selects a slot or a CRS. */
static char *feature_to_json(const char *name, bool enabled, cJSON *geometry)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *properties = root == NULL ? NULL : cJSON_AddObjectToObject(root, "properties");
    char *json = NULL;
    if (geometry != NULL && properties != NULL &&
        cJSON_AddStringToObject(root, "type", "Feature") != NULL &&
        cJSON_AddStringToObject(properties, "name", name) != NULL &&
        cJSON_AddBoolToObject(properties, "enabled", enabled) != NULL &&
        cJSON_AddStringToObject(properties, "coordinateSystem", "WGS84") != NULL &&
        cJSON_AddItemToObject(root, "geometry", geometry)) {
        geometry = NULL;
        json = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(geometry);
    cJSON_Delete(root);
    return json;
}

char *geo_config_target_to_json(const geo_target_config_t *target)
{
    if (target == NULL || target->crs != GEO_CRS_WGS84 ||
        (target->enabled && !target->has_geometry)) {
        return NULL;
    }
    if (!target->has_geometry) {
        return feature_to_json(target->name, target->enabled, cJSON_CreateNull());
    }
    cJSON *geometry = cJSON_CreateObject();
    cJSON *position = position_to_json(target->coordinate);
    if (geometry == NULL || position == NULL ||
        cJSON_AddStringToObject(geometry, "type", "Point") == NULL ||
        !cJSON_AddItemToObject(geometry, "coordinates", position)) {
        cJSON_Delete(position);
        cJSON_Delete(geometry);
        return NULL;
    }
    return feature_to_json(target->name, target->enabled, geometry);
}

char *geo_config_fence_to_json(const geo_fence_config_t *fence,
                               size_t fence_index)
{
    if (fence == NULL || fence_index >= GEO_FENCE_COUNT || fence->crs != GEO_CRS_WGS84 ||
        fence->vertex_count > GEO_FENCE_MAX_VERTICES ||
        (fence->vertex_count > 0U && fence->vertex_count < 3U) ||
        (fence->enabled && fence->vertex_count == 0U)) {
        return NULL;
    }
    if (fence->vertex_count == 0U) {
        return feature_to_json(fence->name, false, cJSON_CreateNull());
    }
    cJSON *geometry = cJSON_CreateObject();
    cJSON *rings = geometry == NULL ? NULL : cJSON_AddArrayToObject(geometry, "coordinates");
    cJSON *ring = cJSON_CreateArray();
    if (rings == NULL || ring == NULL ||
        cJSON_AddStringToObject(geometry, "type", "Polygon") == NULL ||
        !cJSON_AddItemToArray(rings, ring)) {
        cJSON_Delete(ring);
        cJSON_Delete(geometry);
        return NULL;
    }
    int64_t signed_area = 0;
    for (size_t i = 0U; i < fence->vertex_count; ++i) {
        const geo_point_e6_t a = fence->vertices[i];
        const geo_point_e6_t b = fence->vertices[(i + 1U) % fence->vertex_count];
        signed_area += (int64_t)a.longitude_e6 * b.latitude_e6 -
                       (int64_t)b.longitude_e6 * a.latitude_e6;
    }
    /* Input accepts either winding for compatibility. Export follows RFC 7946
     * (counterclockwise exterior ring), including the repeated closing point. */
    for (size_t i = 0U; i <= fence->vertex_count; ++i) {
        const size_t index = signed_area < 0
            ? (fence->vertex_count - i) % fence->vertex_count : i % fence->vertex_count;
        cJSON *position = position_to_json(fence->vertices[index]);
        if (position == NULL || !cJSON_AddItemToArray(ring, position)) {
            cJSON_Delete(position);
            cJSON_Delete(geometry);
            return NULL;
        }
    }
    return feature_to_json(fence->name, fence->enabled, geometry);
}

void geo_config_json_free(char *json)
{
    cJSON_free(json);
}

static esp_err_t load_json_slot(const char *key, char **json)
{
    *json = NULL;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t length = 0U;
    err = nvs_get_str(handle, key, NULL, &length);
    if (err == ESP_OK &&
        (length <= 1U || length > GEO_CONFIG_JSON_MAX_BYTES + 1U)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    char *buffer = NULL;
    if (err == ESP_OK) {
        buffer = malloc(length);
        if (buffer == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        err = nvs_get_str(handle, key, buffer, &length);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }
    *json = buffer;
    return ESP_OK;
}

static esp_err_t save_json_slot(const char *key, const char *json)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, json);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t geo_config_manager_init(geo_config_manager_t *manager,
                                  bool storage_available)
{
    if (manager == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(manager, 0, sizeof(*manager));
    manager->lock = xSemaphoreCreateMutex();
    if (manager->lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    manager->storage_available = storage_available;
    manager->current.revision = 1U;
    manager->current.target.configured = true;
    manager->current.target.enabled = true;
    manager->current.target.has_geometry = true;
    manager->current.target.crs = GEO_CRS_WGS84;
    manager->current.target.coordinate.latitude_e6 =
        (int32_t)llround((double)APP_TARGET_LATITUDE_DEG * 1000000.0);
    manager->current.target.coordinate.longitude_e6 =
        (int32_t)llround((double)APP_TARGET_LONGITUDE_DEG * 1000000.0);
    (void)snprintf(manager->current.target.name,
                   sizeof(manager->current.target.name), "Default target");

    if (!storage_available) {
        ESP_LOGW(TAG, "NVS unavailable; geo configuration is read-only");
        return ESP_OK;
    }

    char *json = NULL;
    esp_err_t err = load_json_slot(NVS_TARGET_KEY, &json);
    if (err == ESP_OK) {
        geo_target_config_t target;
        geo_validation_error_t validation;
        /* Read-only compatibility: upgrading must not erase or rewrite a
         * previously saved custom-format slot. New writes are GeoJSON only. */
        if (geo_config_parse_target_json(json, strlen(json), &target,
                                         &validation) == ESP_OK ||
            parse_legacy_target_json(json, strlen(json), &target,
                                     &validation) == ESP_OK) {
            manager->current.target = target;
            ESP_LOGI(TAG, "Loaded target '%s' (%s)", target.name,
                     geo_config_crs_name(target.crs));
        } else {
            ESP_LOGW(TAG, "Saved target rejected: %s %s",
                     validation.path, validation.message);
        }
        free(json);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Target load failed: %s", esp_err_to_name(err));
    }

    for (size_t index = 0U; index < GEO_FENCE_COUNT; ++index) {
        json = NULL;
        err = load_json_slot(NVS_FENCE_KEYS[index], &json);
        if (err == ESP_OK) {
            geo_fence_config_t fence;
            geo_validation_error_t validation;
            if (geo_config_parse_fence_json(json, strlen(json), index,
                                            &fence, &validation) == ESP_OK ||
                parse_legacy_fence_json(json, strlen(json), index,
                                        &fence, &validation) == ESP_OK) {
                manager->current.fences[index] = fence;
                ESP_LOGI(TAG, "Loaded fence%u '%s' (%u vertices, %s)",
                         (unsigned)(index + 1U), fence.name,
                         (unsigned)fence.vertex_count,
                         geo_config_crs_name(fence.crs));
            } else {
                ESP_LOGW(TAG, "Saved fence%u rejected: %s %s",
                         (unsigned)(index + 1U), validation.path,
                         validation.message);
            }
            free(json);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Fence%u load failed: %s",
                     (unsigned)(index + 1U), esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

void geo_config_manager_get(geo_config_manager_t *manager,
                            geo_config_snapshot_t *snapshot)
{
    if (manager == NULL || snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (xSemaphoreTake(manager->lock, portMAX_DELAY) == pdTRUE) {
        *snapshot = manager->current;
        xSemaphoreGive(manager->lock);
    }
}

esp_err_t geo_config_manager_set_target(geo_config_manager_t *manager,
                                        const geo_target_config_t *target)
{
    if (manager == NULL || target == NULL || !target->configured ||
        !manager->storage_available) {
        return ESP_ERR_INVALID_STATE;
    }
    if (target->crs != GEO_CRS_WGS84 ||
        (target->enabled && !target->has_geometry)) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = geo_config_target_to_json(target);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = save_json_slot(NVS_TARGET_KEY, json);
    geo_config_json_free(json);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(manager->lock, portMAX_DELAY) == pdTRUE) {
        manager->current.target = *target;
        manager->current.revision++;
        xSemaphoreGive(manager->lock);
    }
    return ESP_OK;
}

esp_err_t geo_config_manager_set_fence(geo_config_manager_t *manager,
                                       size_t fence_index,
                                       const geo_fence_config_t *fence)
{
    if (manager == NULL || fence == NULL ||
        fence_index >= GEO_FENCE_COUNT || !fence->configured ||
        !manager->storage_available) {
        return ESP_ERR_INVALID_STATE;
    }
    geo_fence_config_t candidate = *fence;
    geo_validation_error_t validation;
    if (candidate.crs != GEO_CRS_WGS84 ||
        candidate.vertex_count > GEO_FENCE_MAX_VERTICES ||
        (candidate.vertex_count < 3U &&
         (candidate.enabled || candidate.vertex_count != 0U)) ||
        (candidate.vertex_count >= 3U &&
         !validate_fence_geometry(&candidate, "$.geometry.coordinates[0]",
                                  &validation))) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = geo_config_fence_to_json(&candidate, fence_index);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = save_json_slot(NVS_FENCE_KEYS[fence_index], json);
    geo_config_json_free(json);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(manager->lock, portMAX_DELAY) == pdTRUE) {
        manager->current.fences[fence_index] = candidate;
        manager->current.revision++;
        xSemaphoreGive(manager->lock);
    }
    return ESP_OK;
}

static bool outside_mainland_china(double latitude, double longitude)
{
    return longitude < 72.004 || longitude > 137.8347 ||
           latitude < 0.8293 || latitude > 55.8271;
}

static double gcj_transform_latitude(double x, double y)
{
    const double pi = 3.14159265358979323846;
    double result = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y +
                    0.1 * x * y + 0.2 * sqrt(fabs(x));
    result += (20.0 * sin(6.0 * x * pi) +
               20.0 * sin(2.0 * x * pi)) * 2.0 / 3.0;
    result += (20.0 * sin(y * pi) +
               40.0 * sin(y / 3.0 * pi)) * 2.0 / 3.0;
    result += (160.0 * sin(y / 12.0 * pi) +
               320.0 * sin(y * pi / 30.0)) * 2.0 / 3.0;
    return result;
}

static double gcj_transform_longitude(double x, double y)
{
    const double pi = 3.14159265358979323846;
    double result = 300.0 + x + 2.0 * y + 0.1 * x * x +
                    0.1 * x * y + 0.1 * sqrt(fabs(x));
    result += (20.0 * sin(6.0 * x * pi) +
               20.0 * sin(2.0 * x * pi)) * 2.0 / 3.0;
    result += (20.0 * sin(x * pi) +
               40.0 * sin(x / 3.0 * pi)) * 2.0 / 3.0;
    result += (150.0 * sin(x / 12.0 * pi) +
               300.0 * sin(x / 30.0 * pi)) * 2.0 / 3.0;
    return result;
}

static void wgs84_to_gcj02(double latitude,
                           double longitude,
                           double *output_latitude,
                           double *output_longitude)
{
    if (outside_mainland_china(latitude, longitude)) {
        *output_latitude = latitude;
        *output_longitude = longitude;
        return;
    }

    const double pi = 3.14159265358979323846;
    const double semi_major_axis = 6378245.0;
    const double eccentricity_squared = 0.00669342162296594323;
    double latitude_delta = gcj_transform_latitude(longitude - 105.0,
                                                    latitude - 35.0);
    double longitude_delta = gcj_transform_longitude(longitude - 105.0,
                                                      latitude - 35.0);
    const double latitude_radians = latitude / 180.0 * pi;
    const double sine = sin(latitude_radians);
    const double magic = 1.0 - eccentricity_squared * sine * sine;
    const double square_root_magic = sqrt(magic);
    latitude_delta = latitude_delta * 180.0 /
        ((semi_major_axis * (1.0 - eccentricity_squared)) /
         (magic * square_root_magic) * pi);
    longitude_delta = longitude_delta * 180.0 /
        (semi_major_axis / square_root_magic * cos(latitude_radians) * pi);
    *output_latitude = latitude + latitude_delta;
    *output_longitude = longitude + longitude_delta;
}

bool geo_config_resolve_target(const geo_config_snapshot_t *snapshot,
                               double gps_latitude_wgs84,
                               double gps_longitude_wgs84,
                               double *origin_latitude,
                               double *origin_longitude,
                               double *target_latitude,
                               double *target_longitude)
{
    if (snapshot == NULL || origin_latitude == NULL ||
        origin_longitude == NULL || target_latitude == NULL ||
        target_longitude == NULL || !snapshot->target.configured ||
        !snapshot->target.enabled || !snapshot->target.has_geometry ||
        !isfinite(gps_latitude_wgs84) ||
        !isfinite(gps_longitude_wgs84)) {
        return false;
    }
    *origin_latitude = gps_latitude_wgs84;
    *origin_longitude = gps_longitude_wgs84;
    if (snapshot->target.crs == GEO_CRS_GCJ02) {
        wgs84_to_gcj02(gps_latitude_wgs84, gps_longitude_wgs84,
                       origin_latitude, origin_longitude);
    }
    *target_latitude =
        (double)snapshot->target.coordinate.latitude_e6 / 1000000.0;
    *target_longitude =
        (double)snapshot->target.coordinate.longitude_e6 / 1000000.0;
    return true;
}

static bool fence_contains_point(const geo_fence_config_t *fence,
                                 geo_point_e6_t point)
{
    if (point.latitude_e6 < fence->minimum_latitude_e6 ||
        point.latitude_e6 > fence->maximum_latitude_e6 ||
        point.longitude_e6 < fence->minimum_longitude_e6 ||
        point.longitude_e6 > fence->maximum_longitude_e6) {
        return false;
    }

    bool inside = false;
    size_t previous = fence->vertex_count - 1U;
    for (size_t current = 0U; current < fence->vertex_count;
         previous = current++) {
        const geo_point_e6_t a = fence->vertices[previous];
        const geo_point_e6_t b = fence->vertices[current];
        if (point_on_segment(point, a, b)) {
            return true;
        }
        const bool crosses =
            (a.latitude_e6 > point.latitude_e6) !=
            (b.latitude_e6 > point.latitude_e6);
        if (crosses) {
            const double longitude_at_ray =
                (double)(b.longitude_e6 - a.longitude_e6) *
                    (double)(point.latitude_e6 - a.latitude_e6) /
                    (double)(b.latitude_e6 - a.latitude_e6) +
                (double)a.longitude_e6;
            if ((double)point.longitude_e6 < longitude_at_ray) {
                inside = !inside;
            }
        }
    }
    return inside;
}

void geo_config_evaluate_fences(const geo_config_snapshot_t *snapshot,
                                bool gps_valid,
                                double gps_latitude_wgs84,
                                double gps_longitude_wgs84,
                                geo_fence_state_t states[GEO_FENCE_COUNT])
{
    if (states == NULL) {
        return;
    }
    for (size_t index = 0U; index < GEO_FENCE_COUNT; ++index) {
        states[index] = GEO_FENCE_STATE_UNKNOWN;
    }
    if (snapshot == NULL) {
        return;
    }

    for (size_t index = 0U; index < GEO_FENCE_COUNT; ++index) {
        const geo_fence_config_t *fence = &snapshot->fences[index];
        if (!fence->configured) {
            states[index] = GEO_FENCE_STATE_UNCONFIGURED;
            continue;
        }
        if (!fence->enabled) {
            states[index] = GEO_FENCE_STATE_DISABLED;
            continue;
        }
        if (!gps_valid || !isfinite(gps_latitude_wgs84) ||
            !isfinite(gps_longitude_wgs84)) {
            states[index] = GEO_FENCE_STATE_UNKNOWN;
            continue;
        }

        double latitude = gps_latitude_wgs84;
        double longitude = gps_longitude_wgs84;
        if (fence->crs == GEO_CRS_GCJ02) {
            wgs84_to_gcj02(gps_latitude_wgs84, gps_longitude_wgs84,
                           &latitude, &longitude);
        }
        const geo_point_e6_t point = {
            .latitude_e6 = (int32_t)llround(latitude * 1000000.0),
            .longitude_e6 = (int32_t)llround(longitude * 1000000.0),
        };
        states[index] = fence_contains_point(fence, point)
                            ? GEO_FENCE_STATE_INSIDE
                            : GEO_FENCE_STATE_OUTSIDE;
    }
}

const char *geo_fence_state_name(geo_fence_state_t state)
{
    switch (state) {
        case GEO_FENCE_STATE_UNCONFIGURED:
            return "unconfigured";
        case GEO_FENCE_STATE_DISABLED:
            return "disabled";
        case GEO_FENCE_STATE_OUTSIDE:
            return "outside";
        case GEO_FENCE_STATE_INSIDE:
            return "inside";
        case GEO_FENCE_STATE_UNKNOWN:
        default:
            return "unknown";
    }
}
