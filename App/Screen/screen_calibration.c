#include "screen_calibration.h"
#include <math.h>
#include <string.h>

static void transform(const ScreenCalibration *c, uint16_t rx, uint16_t ry, float *x, float *y)
{
    const float u = rx / 4095.0f, v = ry / 4095.0f;
    *x = c->x[0] * u + c->x[1] * v + c->x[2];
    *y = c->y[0] * u + c->y[1] * v + c->y[2];
}

bool ScreenCal_Fit(ScreenCalibration *cal, const ScreenCalPoint p[SCREEN_CAL_POINTS])
{
    float a[3][5] = {{0}};
    uint16_t minx = 4095, maxx = 0, miny = 4095, maxy = 0;
    for (unsigned n = 0; n < SCREEN_CAL_POINTS; ++n) {
        if (p[n].raw_x > 4095 || p[n].raw_y > 4095) return false;
        if (p[n].raw_x < minx) minx = p[n].raw_x;
        if (p[n].raw_x > maxx) maxx = p[n].raw_x;
        if (p[n].raw_y < miny) miny = p[n].raw_y;
        if (p[n].raw_y > maxy) maxy = p[n].raw_y;
        const float q[3] = {p[n].raw_x / 4095.0f, p[n].raw_y / 4095.0f, 1.0f};
        for (unsigned i = 0; i < 3; ++i) {
            for (unsigned j = 0; j < 3; ++j) a[i][j] += q[i] * q[j];
            a[i][3] += q[i] * p[n].x;
            a[i][4] += q[i] * p[n].y;
        }
    }
    if (maxx - minx < 400 || maxy - miny < 400) return false;
    for (unsigned k = 0; k < 3; ++k) {
        unsigned pivot = k;
        for (unsigned j = k + 1; j < 3; ++j)
            if (fabsf(a[j][k]) > fabsf(a[pivot][k])) pivot = j;
        if (fabsf(a[pivot][k]) < 0.00001f) return false;
        for (unsigned j = 0; j < 5; ++j) {
            float t = a[k][j]; a[k][j] = a[pivot][j]; a[pivot][j] = t;
        }
        const float d = a[k][k];
        for (unsigned j = k; j < 5; ++j) a[k][j] /= d;
        for (unsigned row = 0; row < 3; ++row) {
            if (row == k) continue;
            const float f = a[row][k];
            for (unsigned j = k; j < 5; ++j) a[row][j] -= f * a[k][j];
        }
    }
    ScreenCalibration candidate = {.valid = true};
    for (unsigned i = 0; i < 3; ++i) {
        candidate.x[i] = a[i][3]; candidate.y[i] = a[i][4];
        if (!isfinite(candidate.x[i]) || !isfinite(candidate.y[i])) return false;
    }
    for (unsigned n = 0; n < SCREEN_CAL_POINTS; ++n) {
        float x, y;
        transform(&candidate, p[n].raw_x, p[n].raw_y, &x, &y);
        const float dx = x - p[n].x, dy = y - p[n].y;
        if (dx * dx + dy * dy > 64.0f) return false; /* 8 px maximum residual. */
    }
    memcpy(cal, &candidate, sizeof(candidate));
    return true;
}

bool ScreenCal_Map(const ScreenCalibration *cal, uint16_t rx, uint16_t ry,
                   int16_t width, int16_t height, int16_t *x, int16_t *y)
{
    if (!cal->valid || width <= 0 || height <= 0 || rx > 4095 || ry > 4095) return false;
    float fx, fy;
    transform(cal, rx, ry, &fx, &fy);
    if (!isfinite(fx) || !isfinite(fy) || fx < -8 || fy < -8 || fx > width + 7 || fy > height + 7)
        return false;
    int ix = (int)lroundf(fx), iy = (int)lroundf(fy);
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix >= width) ix = width - 1;
    if (iy >= height) iy = height - 1;
    *x = (int16_t)ix; *y = (int16_t)iy;
    return true;
}
