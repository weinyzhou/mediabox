#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <directfb.h>
#include <directfb_windows.h>
#include <cairo/cairo.h>

#include "debug.h"
#include "video.h"

/* #define DEBUG_MEMORY */
#define DEFAULT_OPACITY     (MBV_DEFAULT_OPACITY)


/* window object structure */
struct mbv_dfb_window
{
	struct mbv_dfb_window *parent;
	IDirectFBWindow *dfb_window;
	IDirectFBSurface *surface;
	IDirectFBSurface *content;
	DFBRectangle rect;
	cairo_t *cairo_context;
	int visible;
	uint8_t opacity;
};


static DFBRectangle* rects[10];
static uint8_t *screen_mask = NULL;


IDirectFB *dfb = NULL; /* global so input-directfb.c can see it */
static IDirectFBDisplayLayer *layer = NULL;
static int screen_width = 0;
static int screen_height = 0;
static int is_fbdev = 0;


static struct mbv_dfb_window *root_window = NULL;
static int root_window_flipper_exit = 0;
static pthread_t root_window_flipper;
static pthread_mutex_t root_window_lock = PTHREAD_MUTEX_INITIALIZER;



#define DFBCHECK(x...)                                         \
{                                                            \
	DFBResult err = x;                                         \
         \
	if (err != DFB_OK)                                         \
	{                                                        \
		fprintf( stderr, "%s <%d>:\n\t", __FILE__, __LINE__ ); \
		DirectFBErrorFatal( #x, err );                         \
	}                                                        \
}

#define DFBCOLOR(color) \
	(color >> 24) & 0xFF, \
	(color >> 16) & 0xFF, \
	(color >>  8) & 0xFF, \
	(color      ) & 0xFF


static char *
mbv_dfb_pixfmt_tostring(DFBSurfacePixelFormat fmt)
{
	static char pix_fmt[256];
	switch (fmt) {
	case DSPF_RGB32:  return "RBG32";
	case DSPF_RGB24:  return "RGB24";
	case DSPF_RGB16:  return "RGB16";
	case DSPF_ARGB:   return "ARGB";
	case DSPF_RGB332: return "RGB332";
	case DSPF_YUY2:   return "YUY2";
	case DSPF_UYVY:   return "UYVY";
	case DSPF_YV12:   return "YV12";
	default:
		sprintf(pix_fmt, "PIXFMT: OTHER: %i", fmt);
		return pix_fmt;
	}
}


/**
 * mbv_dfb_isfbdev() -- Returns one if we're running on a real framebuffer.
 */
int
mbv_dfb_isfbdev(void)
{
	return is_fbdev;
}


/**
 * mbv_dfb_getscreensize() -- Gets the screen width and height
 */
void
mbv_dfb_getscreensize(int *w, int *h)
{
	*w = screen_width;
	*h = screen_height;
}


/**
 * mbv_dfb_getscreenmask() -- Gets a pointer to the screen mask.
 * Please see the description of mbv_dfb_regeneratemask() for more
 * details
 */
void *
mbv_dfb_getscreenmask(void *buf)
{
	return (void *) screen_mask;
}


/**
 * mbv_dfb_regenerate_mask() -- Generates a mask of all visible windows.
 * Since rendering video to a DirectFB window is very inneficient the
 * media player object will render the video directly to the framebuffer
 * whenever possible. This mask contains areas that are being used by other
 * windows so that the player doesn't draw over them. TODO: When supported
 * this should be done in hardware (layers).
 */
static void
mbv_dfb_regeneratemask(void)
{
	int i, x, y;

	memset(screen_mask, 0, screen_width * screen_height);

	for (i = 0; i < 10; i++) {
		if (rects[i] != NULL) {
			for (y = rects[i]->y; y < (rects[i]->y + rects[i]->h); y++) {
				for (x = rects[i]->x; x < (rects[i]->x + rects[i]->w); x++) {
					*(screen_mask + (y * screen_width) + x) = 1;
				}
			}
		}
	}
}


static void
mbv_dfb_addwindowmask(struct mbv_dfb_window *window)
{
	int i;

	if (window == root_window) {
		return;
	}

	for (i = 0; i < 10; i++) {
		if (rects[i] == NULL) {
			rects[i] = &window->rect;
			break;
		}
	}
	mbv_dfb_regeneratemask();
}


static void
mbv_dfb_removewindowmask(struct mbv_dfb_window *window)
{
	int i;

	if (window == root_window) {
		return;
	}

	for (i = 0; i < 10; i++) {
		if (rects[i] == &window->rect) {
			rects[i] = NULL;
			break;
		}
	}

	mbv_dfb_regeneratemask();
}


int
mbv_dfb_window_blit_buffer(
	struct mbv_dfb_window *window,
	void *buf, int width, int height, int x, int y)
{
#if 1	
	DFBSurfaceDescription dsc;
	static IDirectFBSurface *surface = NULL;

	assert(window != NULL);
	assert(window->content != NULL);

	dsc.width = width;
	dsc.height = height;
	dsc.flags = DSDESC_HEIGHT | DSDESC_WIDTH | DSDESC_PREALLOCATED | DSDESC_PIXELFORMAT;
	dsc.caps = DSCAPS_NONE;
	dsc.pixelformat = DSPF_RGB32;
	dsc.preallocated[0].data = buf;
	dsc.preallocated[0].pitch = width * 4;
	dsc.preallocated[1].data = NULL;
	dsc.preallocated[1].pitch = 0;

	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &surface));
	DFBCHECK(surface->SetBlittingFlags(surface, DSBLIT_NOFX));
	DFBCHECK(window->content->Blit(window->content, surface, NULL, x, y));
	surface->Release(surface);
#else
	void *dst;
	int pitch;
	DFBCHECK(window->surface->Lock(window->surface, DSLF_READ | DSLF_WRITE, &dst, &pitch));
	memcpy(dst, buf, height * pitch);
	DFBCHECK(window->surface->Unlock(window->surface));;
	//DFBCHECK(window->surface->Flip(window->surface, NULL, DSFLIP_NONE));
#endif

	return 0;
}


/**
 * mbv_dfb_autofliproot() -- Tries to flip the root window at 30 Hz.
 * When we're rendering video to the root window DirectFB may not be able
 * to keep up with this rate, so by running a separate thread for flipping
 * the video stays synchronized but some frames get dropped. Hhopefully this
 * will only be the case when running inside an X server. When running on the
 * framebuffer the video player should draw directly to the framebuffer).
 */
static void *
mbv_dfb_autofliproot(void *arg)
{
	MB_DEBUG_SET_THREAD_NAME("autoflip_root");

	(void) arg;

	while (!root_window_flipper_exit) {
		pthread_mutex_lock(&root_window_lock);
		DFBCHECK(root_window->surface->Flip(root_window->surface, NULL, DSFLIP_NONE));
		pthread_mutex_unlock(&root_window_lock);
		usleep(33333);
	}
	return NULL;
}


#if 0
void
mbv_dfb_window_resize(struct mbv_dfb_window* window, int w, int h)
{
	window->rect.w = w;
	window->rect.h = h;

	mbv_dfb_regeneratemask();

	if (parent == NULL) {

		assert(window->dfb_window != NULL);

		DFBCHECK(window->dfb_window->ResizeSurface(window->dfb_window, w, h));
	}
}
#endif


/**
 * mbv_dfb_window_new() -- Creates a new window
 */
struct mbv_dfb_window*
mbv_dfb_window_new(
	char *title,
	int posx,
	int posy,
	int width,
	int height)
{
	struct mbv_dfb_window *win;
	DFBWindowDescription window_desc = {
		.flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT |
			 DWDESC_CAPS | DWDESC_SURFACE_CAPS,
		.caps = /*DWCAPS_ALPHACHANNEL | |*/ /*DWCAPS_DOUBLEBUFFER | */DWCAPS_NODECORATION,
		.surface_caps = DSCAPS_NONE |  DSCAPS_FLIPPING,
		.posx = posx,
		.posy = posy,
		.width = width,
		.height = height
	};

	assert(title == NULL);

	/* if this is the root window set as primary */
	if (root_window == NULL) {
		window_desc.surface_caps |= DSCAPS_PRIMARY;
	}

	/* first allocate the window structure */
	win = malloc(sizeof(struct mbv_dfb_window));
	if (win == NULL) {
		fprintf(stderr, "mbv_dfb_window_new() failed -- out of memory\n");
		return NULL;
	}


	/* initialize window structure */
	win->parent = NULL;
	win->visible = 0;
	win->rect.w = width;
	win->rect.h = height;
	win->rect.x = posx;
	win->rect.y = posy;
	win->opacity = (uint8_t) ((0xFF * DEFAULT_OPACITY) / 100);
	win->cairo_context = NULL;

	if (0 && root_window == NULL) {
		DFBCHECK(layer->GetWindow(layer, 1, &win->dfb_window));
	} else {
		DFBCHECK(layer->CreateWindow(layer, &window_desc, &win->dfb_window));
	}

	/* set opacity to 100% */
	if (root_window == NULL) {
		DFBCHECK(win->dfb_window->SetOpacity(win->dfb_window, 0xff));
	} else {
		DFBCHECK(win->dfb_window->SetOpacity(win->dfb_window, win->opacity));
	}

	/* get the window surface */
	DFBCHECK(win->dfb_window->GetSurface(win->dfb_window, &win->surface));

	/* set basic drawing flags */
	DFBCHECK(win->surface->SetBlittingFlags(win->surface, DSBLIT_NOFX));

	DFBCHECK(win->surface->GetSubSurface(win->surface, NULL, &win->content));

	return win;
}


/**
 * mbv_dfb_window_cairo_begin() -- Gets a cairo context for drawing
 * to the window
 */
cairo_t *
mbv_dfb_window_cairo_begin(struct mbv_dfb_window *window)
{
	cairo_surface_t *surface;
	int pitch;
	void *buf;

	assert(window != NULL);
	assert(window->cairo_context == NULL);

	DFBCHECK(window->content->Lock(window->content, DSLF_READ | DSLF_WRITE, &buf, &pitch));

	surface = cairo_image_surface_create_for_data(buf,
		CAIRO_FORMAT_ARGB32, window->rect.w, window->rect.h,
		cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, window->rect.w));
	if (surface == NULL) {
		DFBCHECK(window->content->Unlock(window->content));
		return NULL;
	}

	window->cairo_context = cairo_create(surface);
	cairo_surface_destroy(surface);
	if (window->cairo_context == NULL) {
		DFBCHECK(window->content->Unlock(window->content));
	}
		
	return window->cairo_context;
}


/**
 * mbv_dfb_window_cairo_end() -- Ends a cairo drawing session and
 * unlocks the surface
 */
void
mbv_dfb_window_cairo_end(struct mbv_dfb_window *window)
{
	assert(window != NULL);
	assert(window->cairo_context != NULL);

	cairo_destroy(window->cairo_context);
	window->cairo_context = NULL;

	DFBCHECK(window->content->Unlock(window->content));
}


/**
 * mbv_dfb_window_getchildwindow() -- Creates a new child window
 */
struct mbv_dfb_window*
mbv_dfb_window_getchildwindow(struct mbv_dfb_window *window,
	int x, int y, int width, int height)
{
	struct mbv_dfb_window *inst;

	assert(window != NULL);
	assert(width != -1);
	assert(height != -1);

	/* allocate memory for window object */
	inst = malloc(sizeof(struct mbv_dfb_window));
	if (inst == NULL) {
		fprintf(stderr, "mbv: Out of memory\n");
		return NULL;
	}

	/* initialize new window object */
	inst->parent = window;
	inst->dfb_window = window->dfb_window;
	inst->visible = 0;
	inst->cairo_context = NULL;
	inst->rect.w = width;
	inst->rect.h = height;
	inst->rect.x = x;
	inst->rect.y = y;

	/* create the sub-window surface */
	DFBRectangle rect = { x, y, width, height };
	DFBCHECK(window->content->GetSubSurface(window->content, &rect, &inst->surface));
	DFBCHECK(inst->surface->GetSubSurface(inst->surface, NULL, &inst->content));

	return inst;
}


void
mbv_dfb_window_update(struct mbv_dfb_window *window)
{
	DFBCHECK(window->surface->Flip(window->surface, NULL, DSFLIP_BLIT));
}


void
mbv_dfb_window_show(struct mbv_dfb_window *window)
{
	int visible_changed = 0;

	assert(window != NULL);

	if (window != root_window) {
		if (!window->visible) {
			window->visible = 1;
			DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, window->opacity));
			visible_changed = 1;
		}
	}

	if (visible_changed) {
		mbv_dfb_addwindowmask(window);
	}

	mbv_dfb_window_update(window);
}


void
mbv_dfb_window_hide(struct mbv_dfb_window *window)
{
	int visible_changed = 0;
	if (window->visible) {
		DFBCHECK(window->dfb_window->SetOpacity(window->dfb_window, 0x00));
		window->visible = 0;
		visible_changed = 1;
	}
	if (visible_changed) {
		mbv_dfb_removewindowmask(window);
	}

	mbv_dfb_window_update(window);

}


/**
 * mbv_dfb_window_destroy() -- Destroy a window
 */
void
mbv_dfb_window_destroy(struct mbv_dfb_window *window)
{
	assert(window != NULL);

	/* hide the window first */
	mbv_dfb_window_hide(window);

	/* release window surfaces */
	window->content->Release(window->content);
	window->surface->Release(window->surface);

	/* if this is not a subwindow then destroy the directfb
	 * window object as well */
	if (window->parent == NULL) {
		window->dfb_window->Release(window->dfb_window);
	}

	/* free window object */
	free(window);
}


/**
 * enum_display_layers() -- Calledback by dfb to enumerate layers.
 */
static DFBEnumerationResult
enum_display_layers(DFBDisplayLayerID id, DFBDisplayLayerDescription desc, void *data)
{
	fprintf(stderr, "mbv: Found display layer %i\n", id);
	return DFENUM_OK;
}


/**
 * Gets a pointer to the root window
 */
struct mbv_dfb_window*
mbv_dfb_getrootwindow(void)
{
	return root_window;
}


static DFBEnumerationResult
mbv_dfb_video_mode_callback(int width, int height, int bpp, void *arg)
{
	(void) arg;
	DEBUG_VPRINT("video-dfb", "Video mode detected %ix%ix%i", width, height, bpp);
	return DFENUM_OK;
}


/**
 * mbv_init() -- Initialize video device
 */
struct mbv_dfb_window *
mbv_dfb_init(int argc, char **argv)
{
	int i;
	DFBCHECK(DirectFBInit(&argc, &argv));
	DFBCHECK(DirectFBCreate(&dfb));
	DFBCHECK(dfb->SetCooperativeLevel(dfb, DFSCL_NORMAL));
	DFBCHECK(dfb->EnumVideoModes(dfb, mbv_dfb_video_mode_callback, NULL));

	/* IDirectFBScreen does not return the correct size on SDL */
	DFBSurfaceDescription dsc;
	dsc.flags = DSDESC_CAPS;
	dsc.caps  = DSCAPS_PRIMARY;
	IDirectFBSurface *primary;
	DFBCHECK(dfb->CreateSurface(dfb, &dsc, &primary));
	DFBCHECK(primary->GetSize(primary, &screen_width, &screen_height));
	primary->Release(primary);

	/* enumerate display layers */
	DFBCHECK(dfb->EnumDisplayLayers(dfb, enum_display_layers, NULL));

	/* get primary layer */
	DFBCHECK(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	DFBCHECK(layer->SetBackgroundColor(layer, 0x00, 0x00, 0x00, 0xff));
	DFBCHECK(layer->EnableCursor(layer, 0));
	DFBCHECK(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	
	/* create root window */
	root_window = mbv_dfb_window_new(
		NULL, 0, 0, screen_width, screen_height);
	if (root_window == NULL) {
		fprintf(stderr, "Could not create root window\n");
		abort();
	}

	/* print the pixel format of the root window */
	DFBSurfacePixelFormat pix_fmt;
	DFBCHECK(root_window->content->GetPixelFormat(root_window->content, &pix_fmt));
	DEBUG_VPRINT("video-dfb", "Root window pixel format: %s", mbv_dfb_pixfmt_tostring(pix_fmt));

	/* for now one byte per pixel */
	screen_mask = malloc(screen_width * screen_height);
	if (screen_mask == NULL) {
		fprintf(stderr, "mbv: malloc() failed\n");
		abort();
	}

	/* regenerate the screen mask */
	mbv_dfb_regeneratemask();
	for (i = 0; i < 10; i++) {
		rects[i] = NULL;
	}

	/* try to detect if we're running inside an X server */
	/* TODO: Use a more reliable way */
	is_fbdev = (getenv("DISPLAY") == NULL);
	if (!is_fbdev) {
		if (pthread_create(&root_window_flipper, NULL, mbv_dfb_autofliproot, NULL) != 0) {
			fprintf(stderr, "mbv: Could not create autoflip thread\n");
			abort();
		}
	}
	return root_window;
}


/**
 * mbv_dfb_destroy() -- Destroy the directfb video driver
 */
void
mbv_dfb_destroy()
{
	if (!is_fbdev) {
		root_window_flipper_exit = 1;
		pthread_join(root_window_flipper, NULL);
	}

	mbv_dfb_window_destroy(root_window);
	layer->Release(layer);
	dfb->Release(dfb);
	free(screen_mask);
}

