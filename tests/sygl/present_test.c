/* Host test of sygl's window present (src/sygl/egl.c sy_present/blit) with the platform layer stubbed.
   Bug 2026-11-17: with d_display.panel = 1 (the panel shows only rects posted with DisplayDriver ctl 9), the
   N95's Carousel drew its frames into the video RAM only and the screen kept the home screen: every present
   must post the rect it wrote. */
#include "egl.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 fb[SYGL_SCREEN_LONG * SYGL_SCREEN_LONG];
static int rot = 3;
static int posts, post_fn, post_rect[4];
static int dsa_n = -1, dsa_rects[8];

int sy_fb_info(SyThread* t) { t->fb = fb; t->fbPitch = 240 * 4; return 0; }
int sy_display_rotation(SyThread* t) { (void)t; return rot; }
int sy_display_ctl(SyThread* t, int fn, void* a1)
	{
	(void)t;
	u32* r = (u32*)a1;
	posts++, post_fn = fn;
	post_rect[0] = r[0] & 0xffff, post_rect[1] = r[1] & 0xffff, post_rect[2] = r[0] >> 16, post_rect[3] = r[1] >> 16;
	return 0;
	}
int sy_dsa_region(SyThread* t, SySurface* s, int* rects, int max)
	{
	(void)t; (void)s; (void)max;
	if (dsa_n > 0) memcpy(rects, dsa_rects, sizeof(int) * 4 * dsa_n);
	return dsa_n;
	}
void sy_hw_sync(SySurface* s) { (void)s; }

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(void)
	{
	static u32 color[100 * 50];
	SyThread t; memset(&t, 0, sizeof t);
	SySurface s; memset(&s, 0, sizeof s);
	for (int i = 0; i < 100 * 50; i++) color[i] = 0xff00ff00u;
	s.color = color, s.w = 100, s.h = 50, s.wx = 10, s.wy = 20;
	sy_present(&t, &s);								/* no DSA: the whole window */
	CHECK(fb[20 * 240 + 10] == 0xff00ff00u && fb[69 * 240 + 109] == 0xff00ff00u);
	CHECK(posts == 1 && post_fn == 9);
	CHECK(post_rect[0] == 10 && post_rect[1] == 20 && post_rect[2] == 100 && post_rect[3] == 50);
	posts = 0; dsa_n = 2;							/* DSA: one post per visible rect */
	int r[8] = { 10, 20, 50, 10, 60, 40, 50, 30 };
	memcpy(dsa_rects, r, sizeof r);
	sy_present(&t, &s);
	CHECK(posts == 2);
	CHECK(post_rect[0] == 60 && post_rect[1] == 40 && post_rect[2] == 50 && post_rect[3] == 30);
	posts = 0; dsa_n = 1;							/* a rect outside the window: nothing written, nothing posted */
	int r2[4] = { 200, 300, 10, 10 };
	memcpy(dsa_rects, r2, sizeof r2);
	sy_present(&t, &s);
	CHECK(posts == 0);
	/* bug 2026-11-17 (E90 800x352): the blit clipped to a fixed 320x240 screen, so GL drew only that corner. At
	   rotation 0 (landscape) a full-width window is posted at the profile's full width */
	static u32 wide[SYGL_SCREEN_LONG * 4];
	SySurface w; memset(&w, 0, sizeof w);
	w.color = wide, w.w = SYGL_SCREEN_LONG, w.h = 4;
	rot = 0; t.fbPitch = SYGL_SCREEN_LONG * 4; posts = 0; dsa_n = -1;
	sy_present(&t, &w);
	CHECK(posts == 1 && post_rect[2] == SYGL_SCREEN_LONG);
	printf("present_test ok\n");
	return 0;
	}
