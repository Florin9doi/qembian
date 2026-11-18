/* link-only stubs for present_test.c: platform/GL entry points egl.c references but the present path
   never calls (a call aborts the test) */
#include <stdlib.h>
#define STUB(n) int n(void) { abort(); }
STUB(sy_allocz) STUB(sy_big_alloc) STUB(sy_big_free) STUB(sy_bitmap_copy) STUB(sy_bitmap_info)
STUB(sy_display_open) STUB(sy_display_wait_vsync) STUB(sy_dsa_destroy) STUB(sy_free) STUB(sy_gl_create)
STUB(sy_gl_destroy) STUB(sy_gl_made_current) STUB(sy_gl_tex_from_surface) STUB(sy_hw_surface_free)
STUB(sy_hw_surface_new) STUB(sy_hw_terminate) STUB(sy_ms) STUB(sy_thread) STUB(sy_thread_peek) STUB(sy_window_rect)
const char* sygl_names[1];
