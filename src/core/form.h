/* A keyboard form: labelled one-line fields, Up/Down/Tab to move, Enter on
 * the last field submits, Esc cancels. */
#ifndef ZOS_FORM_H
#define ZOS_FORM_H

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

typedef struct {
    const char *label;
    char *buf;              /* in: initial value, out: submitted value */
    size_t cap;
    const char *placeholder;
    bool password;
    bool digits;            /* only 0-9 */
} zos_field_t;

typedef struct zos_form zos_form_t;
typedef void (*zos_form_cb)(zos_form_t *f, void *ud);

/* Builds a full-screen form on `parent`. The form takes over `g` while it
 * has focus; call zos_form_focus() after showing it. */
zos_form_t *zos_form_create(lv_obj_t *parent, lv_group_t *g, const char *title, zos_field_t *fields, int n,
                            zos_form_cb submit, zos_form_cb cancel, void *ud);
lv_obj_t *zos_form_obj(zos_form_t *f);
void zos_form_focus(zos_form_t *f);
void zos_form_message(zos_form_t *f, const char *msg, bool error);

#endif
