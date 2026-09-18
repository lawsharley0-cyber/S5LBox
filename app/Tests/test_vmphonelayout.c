#include "VMPhoneLayout.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static bool inside(vm_phone_rect_t r, double w, double h) {
    return r.x >= -0.001 && r.y >= -0.001 && r.width > 0 && r.height > 0 &&
        r.x + r.width <= w + 0.001 && r.y + r.height <= h + 0.001;
}
static bool overlap(vm_phone_rect_t a, vm_phone_rect_t b) {
    return a.x < b.x + b.width && b.x < a.x + a.width &&
        a.y < b.y + b.height && b.y < a.y + a.height;
}
int main(void) {
    const double sizes[][2] = {{320, 480}, {393, 759}, {440, 850},
        {852, 350}, {956, 370}, {768, 940}, {1024, 720}, {280,315}, {240,240}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        for (unsigned classic = 0; classic < 2; ++classic) {
            vm_phone_layout_t p;
            double w = sizes[i][0], h = sizes[i][1];
            CHECK(vm_phone_layout(w, h, classic != 0, &p));
            CHECK(inside(p.screen, w, h));
            CHECK(fabs(p.screen.width / p.screen.height - 2.0 / 3) < 1e-8);
            CHECK(inside(p.home, w, h));
            CHECK(inside(p.power, w, h));
            CHECK(inside(p.menu, w, h));
            CHECK(p.home.width >= 44 && p.home.height >= 44);
            CHECK(p.power.width >= 44 && p.power.height >= 44);
            CHECK(p.menu.width >= 44 && p.menu.height >= 44);
            CHECK(!overlap(p.screen, p.home));
            CHECK(!overlap(p.screen, p.power));
            CHECK(!overlap(p.screen, p.menu));
            CHECK(!overlap(p.home, p.menu));
            CHECK(!overlap(p.home, p.power));
        }
    }
    vm_phone_layout_t p;
    CHECK(!vm_phone_layout(0, 480, true, &p));
    CHECK(!vm_phone_layout(320, NAN, true, &p));
    CHECK(!vm_phone_layout(INFINITY, 480, true, &p));
    CHECK(!vm_phone_layout(320, 480, true, NULL));
    printf("phone layout: %d failures\n", failures);
    return failures ? 1 : 0;
}
