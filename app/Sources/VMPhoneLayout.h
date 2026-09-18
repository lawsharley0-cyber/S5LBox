/* Original host presentation geometry; no guest pixels or artwork. MIT. */
#ifndef S5LBOX_VM_PHONE_LAYOUT_H
#define S5LBOX_VM_PHONE_LAYOUT_H
#include <stdbool.h>
typedef struct { double x, y, width, height; } vm_phone_rect_t;
typedef struct {
    vm_phone_rect_t body, screen, receiver, home, power, menu;
    bool receiver_visible;
} vm_phone_layout_t;
/* Input is the host's usable safe-area rectangle. Never crops or stretches
 * the 320x480 guest. Landscape puts controls beside the portrait guest. */
bool vm_phone_layout(double width, double height, bool classic,
                     vm_phone_layout_t *out);
#endif
