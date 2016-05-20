#ifndef __MB_VIDEO_H__
#define __MB_VIDEO_H__

struct mbv_window;

struct mbv_window*
mbv_window_new(
	char *title,
	int x,
	int y,
	int width,
	int height);

void
mbv_window_show(struct mbv_window *win);

void
mbv_window_hide(struct mbv_window *win);

void
mbv_window_destroy(struct mbv_window *win);

void
mbv_clear(void);

int
mbv_screen_width_get(void);

int
mbv_screen_height_get(void);

void
mbv_init(int argc, char **argv);

void
mbv_destroy();

#endif