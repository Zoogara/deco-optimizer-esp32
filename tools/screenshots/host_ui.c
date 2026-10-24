/* Compiles the firmware's ui.c in place (so the renderer runs exactly the shipped widget code) and adds one
 * helper to reach its private tileview. */
#include "../../main/ui.c"

void host_ui_goto_tile(int x)
{
    lv_obj_t *tv = lv_obj_get_child(lv_scr_act(), 0);     /* ui_init() creates the tileview first */
    lv_obj_set_tile_id(tv, x, 0, LV_ANIM_OFF);
}
