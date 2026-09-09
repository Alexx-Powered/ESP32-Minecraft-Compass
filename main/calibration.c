#include "calibration.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_config.h"

static const char *TAG = "calibration";
static const char *NVS_NAMESPACE = "mc_compass";
static const char *NVS_KEY = "cal2d";

typedef struct {
    /* A bounded rolling window prevents unbounded RAM use while the user turns
     * the device.  Fits are evaluated only after a meaningful new batch. */
    float x[APP_CALIBRATION_SAMPLE_CAPACITY];
    float y[APP_CALIBRATION_SAMPLE_CAPACITY];
    size_t count;
    size_t last_evaluated_count;
    int64_t first_timestamp_us;
    bool have_last_sample;
    float last_x;
    float last_y;
    calibration_data_t pending;
    float pending_mean_radius;
    uint8_t stable_fit_count;
} calibration_collector_t;

typedef struct {
    calibration_data_t data;
    float mean_radius;
    float span_x;
    float span_y;
    float inlier_ratio;
    uint8_t occupied_bins;
    uint8_t max_empty_bins;
    bool trusted;
} fit_result_t;

static calibration_collector_t s_collector;
static float s_radii[APP_CALIBRATION_SAMPLE_CAPACITY];
static float s_sort_buffer[APP_CALIBRATION_SAMPLE_CAPACITY];
static bool s_inlier[APP_CALIBRATION_SAMPLE_CAPACITY];

static float clamp01(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static int compare_float(const void *left, const void *right)
{
    const float a = *(const float *)left;
    const float b = *(const float *)right;
    return (a > b) - (a < b);
}

static float median_of(const float *values, size_t count)
{
    if (values == NULL || count == 0U) {
        return 0.0f;
    }

    memcpy(s_sort_buffer, values, count * sizeof(values[0]));
    qsort(s_sort_buffer, count, sizeof(s_sort_buffer[0]), compare_float);
    if ((count & 1U) != 0U) {
        return s_sort_buffer[count / 2U];
    }
    return 0.5f * (s_sort_buffer[count / 2U - 1U] +
                   s_sort_buffer[count / 2U]);
}

static uint32_t record_checksum(const calibration_data_t *record)
{
    /* Store a versioned, self-checking blob so a torn/old NVS write cannot
     * replace a trusted calibration at boot. */
    calibration_data_t copy = *record;
    copy.checksum = 0U;

    const uint8_t *bytes = (const uint8_t *)&copy;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < offsetof(calibration_data_t, checksum); ++i) {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}

static void finalize_record(calibration_data_t *record)
{
    record->magic = CALIBRATION_RECORD_MAGIC;
    record->version = CALIBRATION_RECORD_VERSION;
    record->size = (uint16_t)sizeof(*record);
    record->checksum = record_checksum(record);
}

static bool record_is_valid(const calibration_data_t *record)
{
    /* Integrity checks are followed by finite-value and quality bounds; the
     * compass falls back to identity calibration if any guard fails. */
    if (record == NULL || record->magic != CALIBRATION_RECORD_MAGIC ||
        record->version != CALIBRATION_RECORD_VERSION ||
        record->size != sizeof(*record) ||
        record->checksum != record_checksum(record)) {
        return false;
    }

    const float values[] = {
        record->offset_x, record->offset_y,
        record->m00, record->m01, record->m10, record->m11,
        record->confidence, record->axis_ratio, record->radial_cv,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (!isfinite(values[i])) {
            return false;
        }
    }

    const float determinant = record->m00 * record->m11 -
                              record->m01 * record->m10;
    return fabsf(record->offset_x) < 5000.0f &&
           fabsf(record->offset_y) < 5000.0f &&
           fabsf(record->m00) < 10.0f &&
           fabsf(record->m01) < 10.0f &&
           fabsf(record->m10) < 10.0f &&
           fabsf(record->m11) < 10.0f &&
           determinant > 0.01f && determinant < 100.0f &&
           record->confidence >= 0.70f && record->confidence <= 1.0f &&
           record->axis_ratio >= 1.0f &&
           record->axis_ratio <= APP_CALIBRATION_MAX_AXIS_RATIO &&
           record->radial_cv >= 0.0f &&
           record->radial_cv <= APP_CALIBRATION_MAX_RADIAL_CV;
}

static calibration_data_t identity_record(void)
{
    calibration_data_t record = {
        .m00 = 1.0f,
        .m11 = 1.0f,
        .axis_ratio = 1.0f,
    };
    finalize_record(&record);
    return record;
}

static esp_err_t load_record(calibration_data_t *record)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(*record);
    err = nvs_get_blob(handle, NVS_KEY, record, &size);
    nvs_close(handle);
    if (err == ESP_OK && size != sizeof(*record)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t save_record(const calibration_data_t *record)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY, record, sizeof(*record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t calibration_manager_init(calibration_manager_t *manager,
                                   bool storage_available)
{
    /* Load the previous trusted transform before the background collector
     * starts, which keeps heading usable immediately after boot. */
    if (manager == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manager, 0, sizeof(*manager));
    manager->lock = xSemaphoreCreateMutex();
    if (manager->lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    manager->current = identity_record();
    manager->storage_available = storage_available;
    manager->applied_at_us = esp_timer_get_time();

    if (!storage_available) {
        ESP_LOGW(TAG, "NVS unavailable; starting with identity calibration");
        return ESP_OK;
    }

    calibration_data_t stored = {0};
    esp_err_t err = load_record(&stored);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "No saved 2D calibration; background collection enabled");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Calibration load failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    if (!record_is_valid(&stored)) {
        ESP_LOGW(TAG, "Saved calibration failed integrity/quality checks");
        return ESP_OK;
    }

    manager->current = stored;
    manager->valid = true;
    manager->latest_fit_confidence = stored.confidence;
    ESP_LOGI(TAG,
             "Loaded calibration #%lu: confidence=%.3f axis=%.2f residual=%.3f",
             (unsigned long)stored.sequence, stored.confidence,
             stored.axis_ratio, stored.radial_cv);
    return ESP_OK;
}

void calibration_manager_get(calibration_manager_t *manager,
                             calibration_snapshot_t *snapshot)
{
    if (manager == NULL || snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (xSemaphoreTake(manager->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    snapshot->data = manager->current;
    snapshot->valid = manager->valid;
    snapshot->applied_at_us = manager->applied_at_us;
    snapshot->collected_samples = manager->collected_samples;
    snapshot->coverage_percent = manager->coverage_percent;
    snapshot->latest_fit_confidence = manager->latest_fit_confidence;
    xSemaphoreGive(manager->lock);
}

void calibration_apply(const calibration_snapshot_t *snapshot,
                       float raw_x, float raw_y,
                       float *corrected_x, float *corrected_y)
{
    /* Hard-iron offset removal followed by the 2-D whitening matrix turns an
     * elliptical locus into a near-circle for atan2f(). */
    if (snapshot == NULL || corrected_x == NULL || corrected_y == NULL) {
        return;
    }

    const float centered_x = raw_x - snapshot->data.offset_x;
    const float centered_y = raw_y - snapshot->data.offset_y;
    *corrected_x = snapshot->data.m00 * centered_x +
                   snapshot->data.m01 * centered_y;
    *corrected_y = snapshot->data.m10 * centered_x +
                   snapshot->data.m11 * centered_y;
}

static void manager_update_progress(calibration_manager_t *manager,
                                    uint16_t samples,
                                    uint8_t coverage_percent,
                                    float confidence)
{
    if (xSemaphoreTake(manager->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    manager->collected_samples = samples;
    manager->coverage_percent = coverage_percent;
    manager->latest_fit_confidence = confidence;
    xSemaphoreGive(manager->lock);
}

static bool manager_save_interval_elapsed(calibration_manager_t *manager,
                                          int64_t now_us)
{
    bool elapsed = false;
    if (xSemaphoreTake(manager->lock, portMAX_DELAY) == pdTRUE) {
        elapsed = manager->last_saved_at_us == 0 ||
                  now_us - manager->last_saved_at_us >=
                      APP_CALIBRATION_MIN_SAVE_INTERVAL_US;
        xSemaphoreGive(manager->lock);
    }
    return elapsed;
}

static void manager_commit(calibration_manager_t *manager,
                           const calibration_data_t *candidate)
{
    /* NVS commits are intentionally rare (wear guard); RAM is updated even if
     * storage is unavailable so the current session still benefits. */
    calibration_data_t record = *candidate;
    calibration_snapshot_t previous;
    calibration_manager_get(manager, &previous);
    record.sequence = previous.data.sequence + 1U;
    finalize_record(&record);

    esp_err_t save_err = ESP_ERR_INVALID_STATE;
    if (manager->storage_available) {
        save_err = save_record(&record);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not persist calibration: %s",
                     esp_err_to_name(save_err));
        }
    }

    const int64_t now_us = esp_timer_get_time();
    if (xSemaphoreTake(manager->lock, portMAX_DELAY) == pdTRUE) {
        manager->current = record;
        manager->valid = true;
        manager->applied_at_us = now_us;
        manager->last_saved_at_us = save_err == ESP_OK
                                        ? now_us
                                        : manager->last_saved_at_us;
        manager->collected_samples = 0U;
        manager->coverage_percent = 0U;
        manager->latest_fit_confidence = record.confidence;
        xSemaphoreGive(manager->lock);
    }

    ESP_LOGI(TAG,
             "Applied calibration #%lu: confidence=%.3f axis=%.2f residual=%.3f%s",
             (unsigned long)record.sequence, record.confidence,
             record.axis_ratio, record.radial_cv,
             save_err == ESP_OK ? " (saved)" : " (RAM only)");
}

static bool build_model(const calibration_collector_t *collector,
                        const bool *include,
                        calibration_data_t *model,
                        float *span_x_out,
                        float *span_y_out,
                        float *mean_radius_out,
                        size_t *included_count_out)
{
    /* Estimate the ellipse center from trimmed extrema, then whiten its
     * covariance.  The same routine is reused before and after MAD rejection. */
    size_t included_count = 0U;

    for (size_t i = 0; i < collector->count; ++i) {
        if (include != NULL && !include[i]) {
            continue;
        }
        included_count++;
    }
    if (included_count < 16U) {
        return false;
    }

    /*
     * Exact extrema make a min/max hard-iron estimate very sensitive to one
     * magnetic spike.  Use the inner 95% range instead; the later MAD pass
     * still performs the full radial outlier rejection.
     */
    const size_t trim_count = included_count / 40U;
    size_t value_count = 0U;
    for (size_t i = 0; i < collector->count; ++i) {
        if (include == NULL || include[i]) {
            s_sort_buffer[value_count++] = collector->x[i];
        }
    }
    qsort(s_sort_buffer, value_count, sizeof(s_sort_buffer[0]), compare_float);
    const float min_x = s_sort_buffer[trim_count];
    const float max_x = s_sort_buffer[value_count - 1U - trim_count];

    value_count = 0U;
    for (size_t i = 0; i < collector->count; ++i) {
        if (include == NULL || include[i]) {
            s_sort_buffer[value_count++] = collector->y[i];
        }
    }
    qsort(s_sort_buffer, value_count, sizeof(s_sort_buffer[0]), compare_float);
    const float min_y = s_sort_buffer[trim_count];
    const float max_y = s_sort_buffer[value_count - 1U - trim_count];

    const float span_x = max_x - min_x;
    const float span_y = max_y - min_y;
    if (!isfinite(span_x) || !isfinite(span_y) ||
        span_x < 1.0f || span_y < 1.0f) {
        return false;
    }

    const double offset_x = 0.5 * ((double)min_x + (double)max_x);
    const double offset_y = 0.5 * ((double)min_y + (double)max_y);
    double covariance_xx = 0.0;
    double covariance_xy = 0.0;
    double covariance_yy = 0.0;

    for (size_t i = 0; i < collector->count; ++i) {
        if (include != NULL && !include[i]) {
            continue;
        }
        const double x = (double)collector->x[i] - offset_x;
        const double y = (double)collector->y[i] - offset_y;
        covariance_xx += x * x;
        covariance_xy += x * y;
        covariance_yy += y * y;
    }
    covariance_xx /= (double)included_count;
    covariance_xy /= (double)included_count;
    covariance_yy /= (double)included_count;

    const double difference = covariance_xx - covariance_yy;
    const double root = sqrt(difference * difference +
                             4.0 * covariance_xy * covariance_xy);
    const double lambda_1 = 0.5 * (covariance_xx + covariance_yy + root);
    const double lambda_2 = 0.5 * (covariance_xx + covariance_yy - root);
    if (!isfinite(lambda_1) || !isfinite(lambda_2) || lambda_2 <= 1e-6) {
        return false;
    }

    const double axis_ratio = sqrt(lambda_1 / lambda_2);
    const double angle = 0.5 * atan2(2.0 * covariance_xy, difference);
    const double c = cos(angle);
    const double s = sin(angle);
    const double target_variance = sqrt(lambda_1 * lambda_2);
    const double scale_1 = sqrt(target_variance / lambda_1);
    const double scale_2 = sqrt(target_variance / lambda_2);

    memset(model, 0, sizeof(*model));
    model->offset_x = (float)offset_x;
    model->offset_y = (float)offset_y;
    model->m00 = (float)(scale_1 * c * c + scale_2 * s * s);
    model->m01 = (float)((scale_1 - scale_2) * c * s);
    model->m10 = model->m01;
    model->m11 = (float)(scale_1 * s * s + scale_2 * c * c);
    model->axis_ratio = (float)axis_ratio;

    double radius_sum = 0.0;
    for (size_t i = 0; i < collector->count; ++i) {
        if (include != NULL && !include[i]) {
            continue;
        }
        const float x = collector->x[i] - model->offset_x;
        const float y = collector->y[i] - model->offset_y;
        const float corrected_x = model->m00 * x + model->m01 * y;
        const float corrected_y = model->m10 * x + model->m11 * y;
        radius_sum += hypotf(corrected_x, corrected_y);
    }

    *span_x_out = span_x;
    *span_y_out = span_y;
    *mean_radius_out = (float)(radius_sum / (double)included_count);
    *included_count_out = included_count;
    return isfinite(*mean_radius_out) && *mean_radius_out > 1.0f;
}

static uint8_t maximum_empty_run(const bool *occupied)
{
    uint8_t longest = 0U;
    uint8_t current = 0U;
    for (size_t i = 0; i < APP_CALIBRATION_ANGLE_BINS * 2U; ++i) {
        if (!occupied[i % APP_CALIBRATION_ANGLE_BINS]) {
            current++;
            if (current > longest) {
                longest = current;
            }
        } else {
            current = 0U;
        }
    }
    if (longest > APP_CALIBRATION_ANGLE_BINS) {
        longest = APP_CALIBRATION_ANGLE_BINS;
    }
    return longest;
}

static fit_result_t fit_collector(const calibration_collector_t *collector)
{
    /* First fit supplies radial distances; a MAD-based pass rejects magnetic
     * spikes before coverage, residual and confidence are evaluated. */
    fit_result_t result = {0};
    calibration_data_t initial;
    float initial_span_x = 0.0f;
    float initial_span_y = 0.0f;
    float initial_mean_radius = 0.0f;
    size_t initial_count = 0U;

    if (!build_model(collector, NULL, &initial,
                     &initial_span_x, &initial_span_y,
                     &initial_mean_radius, &initial_count)) {
        return result;
    }

    for (size_t i = 0; i < collector->count; ++i) {
        const float x = collector->x[i] - initial.offset_x;
        const float y = collector->y[i] - initial.offset_y;
        s_radii[i] = hypotf(initial.m00 * x + initial.m01 * y,
                            initial.m10 * x + initial.m11 * y);
    }
    const float median_radius = median_of(s_radii, collector->count);
    for (size_t i = 0; i < collector->count; ++i) {
        s_sort_buffer[i] = fabsf(s_radii[i] - median_radius);
    }
    qsort(s_sort_buffer, collector->count, sizeof(s_sort_buffer[0]),
          compare_float);
    const float median_deviation =
        (collector->count & 1U) != 0U
            ? s_sort_buffer[collector->count / 2U]
            : 0.5f * (s_sort_buffer[collector->count / 2U - 1U] +
                      s_sort_buffer[collector->count / 2U]);
    const float robust_sigma = 1.4826f * median_deviation;
    const float inlier_limit = fmaxf(4.0f * robust_sigma,
                                     fmaxf(0.18f * median_radius, 8.0f));

    size_t inlier_count = 0U;
    for (size_t i = 0; i < collector->count; ++i) {
        s_inlier[i] = isfinite(s_radii[i]) &&
                      fabsf(s_radii[i] - median_radius) <= inlier_limit;
        if (s_inlier[i]) {
            inlier_count++;
        }
    }

    calibration_data_t final_model;
    if (!build_model(collector, s_inlier, &final_model,
                     &result.span_x, &result.span_y,
                     &result.mean_radius, &inlier_count)) {
        return result;
    }

    bool occupied[APP_CALIBRATION_ANGLE_BINS] = {0};
    double radius_sum = 0.0;
    double radius_squared_sum = 0.0;
    uint8_t occupied_count = 0U;
    for (size_t i = 0; i < collector->count; ++i) {
        if (!s_inlier[i]) {
            continue;
        }

        const float x = collector->x[i] - final_model.offset_x;
        const float y = collector->y[i] - final_model.offset_y;
        const float corrected_x = final_model.m00 * x + final_model.m01 * y;
        const float corrected_y = final_model.m10 * x + final_model.m11 * y;
        const float radius = hypotf(corrected_x, corrected_y);
        radius_sum += radius;
        radius_squared_sum += (double)radius * (double)radius;

        float angle = atan2f(y, x);
        if (angle < 0.0f) {
            angle += 2.0f * (float)M_PI;
        }
        size_t bin = (size_t)(angle *
                              ((float)APP_CALIBRATION_ANGLE_BINS /
                               (2.0f * (float)M_PI)));
        if (bin >= APP_CALIBRATION_ANGLE_BINS) {
            bin = 0U;
        }
        occupied[bin] = true;
    }
    for (size_t i = 0; i < APP_CALIBRATION_ANGLE_BINS; ++i) {
        if (occupied[i]) {
            occupied_count++;
        }
    }

    const double mean_radius = radius_sum / (double)inlier_count;
    double variance = radius_squared_sum / (double)inlier_count -
                      mean_radius * mean_radius;
    if (variance < 0.0) {
        variance = 0.0;
    }
    final_model.radial_cv = mean_radius > 0.0
                                ? (float)(sqrt(variance) / mean_radius)
                                : INFINITY;
    result.mean_radius = (float)mean_radius;
    result.inlier_ratio = (float)inlier_count / (float)collector->count;
    result.occupied_bins = occupied_count;
    result.max_empty_bins = maximum_empty_run(occupied);

    const float coverage_score =
        (float)occupied_count / (float)APP_CALIBRATION_ANGLE_BINS;
    const float gap_score = clamp01(
        1.0f - (float)result.max_empty_bins /
                   (2.0f * (float)APP_CALIBRATION_MAX_EMPTY_BINS));
    const float inlier_score = clamp01(
        (result.inlier_ratio - 0.65f) / 0.35f);
    const float residual_score = clamp01(
        1.0f - final_model.radial_cv /
                   (1.5f * APP_CALIBRATION_MAX_RADIAL_CV));
    const float axis_score = clamp01(
        1.0f - (final_model.axis_ratio - 1.0f) /
                   (APP_CALIBRATION_MAX_AXIS_RATIO - 1.0f));
    const float span_score = clamp01(
        fminf(result.span_x, result.span_y) /
        (2.0f * APP_CALIBRATION_MIN_AXIS_SPAN_MG));

    final_model.confidence = 0.35f * coverage_score +
                             0.15f * gap_score +
                             0.20f * inlier_score +
                             0.20f * residual_score +
                             0.05f * axis_score +
                             0.05f * span_score;
    result.data = final_model;
    result.trusted =
        collector->count >= APP_CALIBRATION_MIN_SAMPLES &&
        result.span_x >= APP_CALIBRATION_MIN_AXIS_SPAN_MG &&
        result.span_y >= APP_CALIBRATION_MIN_AXIS_SPAN_MG &&
        result.occupied_bins >= APP_CALIBRATION_MIN_OCCUPIED_BINS &&
        result.max_empty_bins <= APP_CALIBRATION_MAX_EMPTY_BINS &&
        result.inlier_ratio >= APP_CALIBRATION_MIN_INLIER_RATIO &&
        final_model.axis_ratio <= APP_CALIBRATION_MAX_AXIS_RATIO &&
        final_model.radial_cv <= APP_CALIBRATION_MAX_RADIAL_CV &&
        final_model.confidence >= APP_CALIBRATION_CONFIDENCE_THRESHOLD;
    return result;
}

static uint8_t estimate_coverage(const calibration_collector_t *collector)
{
    if (collector->count < 16U) {
        return 0U;
    }

    float min_x = INFINITY;
    float max_x = -INFINITY;
    float min_y = INFINITY;
    float max_y = -INFINITY;
    for (size_t i = 0; i < collector->count; ++i) {
        min_x = fminf(min_x, collector->x[i]);
        max_x = fmaxf(max_x, collector->x[i]);
        min_y = fminf(min_y, collector->y[i]);
        max_y = fmaxf(max_y, collector->y[i]);
    }
    if (max_x - min_x < 0.5f * APP_CALIBRATION_MIN_AXIS_SPAN_MG ||
        max_y - min_y < 0.5f * APP_CALIBRATION_MIN_AXIS_SPAN_MG) {
        return 0U;
    }

    const float center_x = 0.5f * (min_x + max_x);
    const float center_y = 0.5f * (min_y + max_y);
    bool occupied[APP_CALIBRATION_ANGLE_BINS] = {0};
    uint8_t count = 0U;
    for (size_t i = 0; i < collector->count; ++i) {
        float angle = atan2f(collector->y[i] - center_y,
                             collector->x[i] - center_x);
        if (angle < 0.0f) {
            angle += 2.0f * (float)M_PI;
        }
        size_t bin = (size_t)(angle *
                              ((float)APP_CALIBRATION_ANGLE_BINS /
                               (2.0f * (float)M_PI)));
        if (bin >= APP_CALIBRATION_ANGLE_BINS) {
            bin = 0U;
        }
        occupied[bin] = true;
    }
    for (size_t i = 0; i < APP_CALIBRATION_ANGLE_BINS; ++i) {
        count += occupied[i] ? 1U : 0U;
    }
    return (uint8_t)((100U * count) / APP_CALIBRATION_ANGLE_BINS);
}

static void reset_collector(calibration_collector_t *collector)
{
    memset(collector, 0, sizeof(*collector));
}

static bool sample_is_plausible(const mag_sample_t *sample)
{
    /* Reject impossible/near-zero field vectors before they can bias a fit. */
    if (!isfinite(sample->x_mg) || !isfinite(sample->y_mg) ||
        !isfinite(sample->z_mg)) {
        return false;
    }
    const float magnitude = sqrtf(sample->x_mg * sample->x_mg +
                                  sample->y_mg * sample->y_mg +
                                  sample->z_mg * sample->z_mg);
    return magnitude >= APP_CALIBRATION_MIN_FIELD_MG &&
           magnitude <= APP_CALIBRATION_MAX_FIELD_MG;
}

static bool add_sample(calibration_collector_t *collector,
                       const mag_sample_t *sample)
{
    /* Keep only distinct points inside one bounded turn window.  A very old
     * sample or a full buffer causes the collector to start a fresh candidate. */
    if (!sample_is_plausible(sample)) {
        return false;
    }

    if (collector->count > 0U &&
        sample->timestamp_us - collector->first_timestamp_us >
            APP_CALIBRATION_MAX_WINDOW_US) {
        reset_collector(collector);
    }

    if (collector->have_last_sample &&
        hypotf(sample->x_mg - collector->last_x,
               sample->y_mg - collector->last_y) <
            APP_CALIBRATION_MIN_SAMPLE_DELTA_MG) {
        return false;
    }

    if (collector->count >= APP_CALIBRATION_SAMPLE_CAPACITY) {
        return false;
    }
    if (collector->count == 0U) {
        collector->first_timestamp_us = sample->timestamp_us;
    }
    collector->x[collector->count] = sample->x_mg;
    collector->y[collector->count] = sample->y_mg;
    collector->count++;
    collector->last_x = sample->x_mg;
    collector->last_y = sample->y_mg;
    collector->have_last_sample = true;
    return true;
}

static bool fits_agree(const calibration_collector_t *collector,
                       const fit_result_t *fit)
{
    const float offset_delta = hypotf(
        fit->data.offset_x - collector->pending.offset_x,
        fit->data.offset_y - collector->pending.offset_y);
    const float radius = fmaxf(fit->mean_radius, 100.0f);
    const float matrix_delta = fmaxf(
        fmaxf(fabsf(fit->data.m00 - collector->pending.m00),
              fabsf(fit->data.m01 - collector->pending.m01)),
        fmaxf(fabsf(fit->data.m10 - collector->pending.m10),
              fabsf(fit->data.m11 - collector->pending.m11)));
    return offset_delta <=
               APP_CALIBRATION_OFFSET_AGREEMENT_FRACTION * radius &&
           matrix_delta <= APP_CALIBRATION_MATRIX_AGREEMENT &&
           fabsf(fit->data.axis_ratio - collector->pending.axis_ratio) <=
               0.35f;
}

void calibration_task(void *argument)
{
    /* Lowest-priority task: it blocks indefinitely for samples, performs the
     * expensive fit only every APP_CALIBRATION_EVALUATION_STEP accepted points,
     * and requires several agreeing trusted fits before changing NVS/RAM. */
    calibration_task_config_t *config = (calibration_task_config_t *)argument;
    if (config == NULL || config->manager == NULL ||
        config->sample_queue == NULL) {
        ESP_LOGE(TAG, "Invalid calibration task configuration");
        vTaskDelete(NULL);
        return;
    }

    reset_collector(&s_collector);
    mag_sample_t sample;
    while (true) {
        if (xQueueReceive(config->sample_queue, &sample, portMAX_DELAY) !=
            pdTRUE) {
            continue;
        }
        if (!add_sample(&s_collector, &sample)) {
            if (s_collector.count >= APP_CALIBRATION_SAMPLE_CAPACITY) {
                ESP_LOGW(TAG, "Calibration window full; starting a new window");
                reset_collector(&s_collector);
                manager_update_progress(config->manager, 0U, 0U, 0.0f);
            }
            continue;
        }

        if ((s_collector.count % 10U) == 0U) {
            manager_update_progress(
                config->manager,
                (uint16_t)s_collector.count,
                estimate_coverage(&s_collector),
                0.0f);
        }

        const int64_t elapsed_us = sample.timestamp_us -
                                   s_collector.first_timestamp_us;
        const bool enough_new_samples =
            s_collector.count - s_collector.last_evaluated_count >=
                APP_CALIBRATION_EVALUATION_STEP;
        if (s_collector.count < APP_CALIBRATION_MIN_SAMPLES ||
            elapsed_us < APP_CALIBRATION_MIN_WINDOW_US ||
            !enough_new_samples) {
            continue;
        }

        s_collector.last_evaluated_count = s_collector.count;
        const fit_result_t fit = fit_collector(&s_collector);
        const uint8_t coverage_percent = (uint8_t)(
            (100U * fit.occupied_bins) / APP_CALIBRATION_ANGLE_BINS);
        manager_update_progress(config->manager,
                                (uint16_t)s_collector.count,
                                coverage_percent,
                                fit.data.confidence);

        ESP_LOGI(TAG,
                 "Candidate: n=%u cover=%u%% gap=%u inliers=%.2f axis=%.2f residual=%.3f confidence=%.3f %s",
                 (unsigned)s_collector.count, (unsigned)coverage_percent,
                 (unsigned)fit.max_empty_bins, fit.inlier_ratio,
                 fit.data.axis_ratio, fit.data.radial_cv,
                 fit.data.confidence, fit.trusted ? "trusted" : "rejected");

        if (!fit.trusted) {
            s_collector.stable_fit_count = 0U;
        } else if (s_collector.stable_fit_count == 0U ||
                   !fits_agree(&s_collector, &fit)) {
            s_collector.pending = fit.data;
            s_collector.pending_mean_radius = fit.mean_radius;
            s_collector.stable_fit_count = 1U;
        } else {
            s_collector.pending = fit.data;
            s_collector.pending_mean_radius = fit.mean_radius;
            s_collector.stable_fit_count++;
        }

        if (s_collector.stable_fit_count >=
            APP_CALIBRATION_REQUIRED_STABLE_FITS) {
            const int64_t now_us = esp_timer_get_time();
            if (manager_save_interval_elapsed(config->manager, now_us)) {
                manager_commit(config->manager, &s_collector.pending);
            } else {
                ESP_LOGI(TAG, "Trusted calibration deferred by NVS wear guard");
            }
            reset_collector(&s_collector);
        } else if (s_collector.count >= APP_CALIBRATION_SAMPLE_CAPACITY) {
            reset_collector(&s_collector);
        }
    }
}
