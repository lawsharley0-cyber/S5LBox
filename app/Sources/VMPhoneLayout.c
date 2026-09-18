#include "VMPhoneLayout.h"
#include <math.h>
#include <string.h>

static vm_phone_rect_t rect(double x, double y, double w, double h) {
    vm_phone_rect_t r = { x, y, w, h }; return r;
}

bool vm_phone_layout(double width, double height, bool classic,
                     vm_phone_layout_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!isfinite(width) || !isfinite(height) || width < 240 || height < 180)
        return false;
    const double margin = 8;
    if (width > height) {
        const double h = fmin(height - 2 * margin,
                              (width - 112) * 1.5);
        const double w = h / 1.5;
        const double x = (width - w - 88) / 2;
        const double y = (height - h) / 2;
        out->body = rect(x - 8, y - 6, w + 104, h + 12);
        out->screen = rect(x, y, w, h);
        out->home = rect(x + w + 20, height / 2 - 26, 52, 52);
        out->power = rect(x + w + 24, y + 4, 44, 44);
        out->menu = rect(x + w + 24, y + h - 48, 44, 44);
    } else if (classic && fmin((width - 16) / 360, (height - 16) / 664) >= 0.66) {
        const double scale = fmin((width - 16) / 360, (height - 16) / 664);
        const double w = 360 * scale, h = 664 * scale;
        const double x = (width - w) / 2, y = (height - h) / 2;
        const double key = fmax(44, 58 * scale);
        const double controlsY = y + 614 * scale;
        out->body = rect(x, y, w, h);
        out->screen = rect(x + 18 * scale, y + 82 * scale,
                           324 * scale, 486 * scale);
        out->receiver = rect(width / 2 - 27 * scale, y + 38 * scale,
                             54 * scale, 6 * scale);
        out->receiver_visible = true;
        out->home = rect(width / 2 - key / 2, controlsY - key / 2, key, key);
        out->power = rect(x + w - 54, y + 15 * scale, 44, 44);
        out->menu = rect(x + w - 56, controlsY - 22, 44, 44);
    } else {
        const double h = fmin(height - 84, (width - 16) * 1.5);
        const double w = h / 1.5;
        const double y = (height - h - 68) / 2;
        out->screen = rect((width - w) / 2, y, w, h);
        out->body = rect(0, 0, width, height);
        out->home = rect(width / 2 - 26, y + h + 10, 52, 52);
        out->power = rect(width / 2 - 104, y + h + 14, 44, 44);
        out->menu = rect(width / 2 + 60, y + h + 14, 44, 44);
    }
    return true;
}
