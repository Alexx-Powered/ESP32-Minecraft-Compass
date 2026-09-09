#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * These centers and boundaries come from the pixel-geometry fit documented in
 * Minecraft指南针_32态最拟合角度与划分范围说明.docx.  The table deliberately
 * keeps the original status numbers so the existing frame bitmap remains the
 * single source of LED data.  Statuses that are geometrically identical map to
 * the first/canonical status in their group (9, 25 and 28).
 */
#define COMPASS_FITTED_GROUP_COUNT 27U
#define COMPASS_FITTED_AVERAGE_STEP_DEG \
    (360.0f / (float)COMPASS_FITTED_GROUP_COUNT)

static const float COMPASS_FITTED_CENTERS_DEG[
    COMPASS_FITTED_GROUP_COUNT] = {
    0.000000f,   29.103479f,  38.703527f,  55.604954f,  63.926133f,
    73.475592f,  75.327053f,  90.000000f, 105.476502f, 108.390424f,
    118.362083f, 126.118646f, 142.972698f, 151.845034f, 180.000000f,
    208.154966f, 217.027302f, 233.881354f, 246.455446f, 250.987605f,
    254.523498f, 270.000000f, 285.283691f, 291.898503f, 305.435104f,
    321.517831f, 331.013647f,
};

static const uint8_t COMPASS_FITTED_STATUS[
    COMPASS_FITTED_GROUP_COUNT] = {
    1U,  2U,  3U,  4U,  5U,  6U,  7U,  9U,  11U,
    12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U,
    21U, 22U, 23U, 25U, 27U, 28U, 30U, 31U, 32U,
};

/* Left-closed/right-open Voronoi intervals on [0, 360 degrees). */
static const float COMPASS_FITTED_LOWER_DEG[
    COMPASS_FITTED_GROUP_COUNT] = {
    345.506823f, 14.551740f,  33.903503f,  47.154240f,  59.765544f,
    68.700863f,  74.401323f,  82.663527f,  97.738251f, 106.933463f,
    113.376253f, 122.240365f, 134.545672f, 147.408866f, 165.922517f,
    194.077483f, 212.591134f, 225.454328f, 240.168400f, 248.721525f,
    252.755551f, 262.261749f, 277.641846f, 288.591097f, 298.666804f,
    313.476467f, 326.265739f,
};

static const float COMPASS_FITTED_UPPER_DEG[
    COMPASS_FITTED_GROUP_COUNT] = {
    14.551740f,  33.903503f,  47.154240f,  59.765544f,  68.700863f,
    74.401323f,  82.663527f,  97.738251f, 106.933463f, 113.376253f,
    122.240365f, 134.545672f, 147.408866f, 165.922517f, 194.077483f,
    212.591134f, 225.454328f, 240.168400f, 248.721525f, 252.755551f,
    262.261749f, 277.641846f, 288.591097f, 298.666804f, 313.476467f,
    326.265739f, 345.506823f,
};

static inline float compass_angle_normalize(float degrees)
{
    degrees = fmodf(degrees, 360.0f);
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

static inline float compass_angle_wrap_signed(float degrees)
{
    degrees = compass_angle_normalize(degrees);
    return degrees > 180.0f ? degrees - 360.0f : degrees;
}

static inline float compass_angle_forward_delta(float from_degrees,
                                                float to_degrees)
{
    return compass_angle_normalize(to_degrees - from_degrees);
}

static inline int compass_fitted_index_for_status(uint8_t status)
{
    for (size_t index = 0U; index < COMPASS_FITTED_GROUP_COUNT; ++index) {
        if (COMPASS_FITTED_STATUS[index] == status) {
            return (int)index;
        }
    }
    return -1;
}

static inline uint8_t compass_status_for_angle(float angle_degrees)
{
    const float normalized = compass_angle_normalize(angle_degrees);
    if (!isfinite(normalized)) {
        return 0U;
    }

    for (size_t index = 0U; index < COMPASS_FITTED_GROUP_COUNT; ++index) {
        const float lower = COMPASS_FITTED_LOWER_DEG[index];
        const float upper = COMPASS_FITTED_UPPER_DEG[index];
        const bool inside = lower < upper
                                ? normalized >= lower && normalized < upper
                                : normalized >= lower || normalized < upper;
        if (inside) {
            return COMPASS_FITTED_STATUS[index];
        }
    }

    /* The intervals cover the circle; this is only a floating-point guard. */
    return COMPASS_FITTED_STATUS[0];
}

static inline uint8_t compass_status_with_hysteresis(float angle_degrees,
                                                     uint8_t current_status,
                                                     float hysteresis_degrees)
{
    const uint8_t candidate = compass_status_for_angle(angle_degrees);
    if (candidate == 0U) {
        return 0U;
    }

    const int current_index = compass_fitted_index_for_status(current_status);
    if (current_index < 0 || candidate == current_status) {
        return candidate;
    }

    const float normalized = compass_angle_normalize(angle_degrees);
    const float signed_delta = compass_angle_wrap_signed(
        normalized - COMPASS_FITTED_CENTERS_DEG[current_index]);
    const float boundary_distance = signed_delta >= 0.0f
        ? compass_angle_forward_delta(
              COMPASS_FITTED_CENTERS_DEG[current_index],
              COMPASS_FITTED_UPPER_DEG[current_index])
        : compass_angle_forward_delta(
              COMPASS_FITTED_LOWER_DEG[current_index],
              COMPASS_FITTED_CENTERS_DEG[current_index]);
    const float extra_margin = hysteresis_degrees > 0.0f
                                   ? hysteresis_degrees
                                   : 0.0f;
    return fabsf(signed_delta) > boundary_distance + extra_margin
               ? candidate
               : current_status;
}
