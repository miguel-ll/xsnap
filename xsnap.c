#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <png.h>
#include <X11/Xlib.h>
#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include "config.h"

Display* display;
Window   root;
Window   overlay;
XImage*  img;
Cursor   cursor;
Pixmap   pm;
GC 		 gc;
GC		 ogc;
XWindowAttributes hoverw;

char*    fpath;
uint8_t* buffer;
uint8_t* keymap;

int opaque_mode = 0;

int32_t exit_clean(char* err) {
	if(img) XDestroyImage(img);
	if(pm)  XFreePixmap(display, pm);
	if(gc)  XFreeGC(display, gc);
	if(ogc) XFreeGC(display, ogc);
	XUnmapWindow(display, overlay);
	XFreeCursor(display, cursor);

	if(fpath)  free(fpath);
	if(buffer) free(buffer);
	if(keymap) free(keymap);
	XCloseDisplay(display);

	if(err) {
		fprintf(stderr, "%s", err);
		return 1;
	}
	return 0;
}

int32_t create_filename(char** ts) {
	*ts = malloc(sizeof(char) * 40);
	if(!*ts)
		return exit_clean("Could not allocate filename\n");
	time_t t;
	time(&t);
	struct tm *info = localtime(&t);
	strftime(*ts, 40, "%Y-%m-%d-%H%M%S_xsnap.png", info);

	return 0;
}

int32_t create_png(uint8_t* buffer, uint32_t width, uint32_t height, char* ts) {
	printf("%s\n", ts);

	FILE *fp = fopen(ts, "wb");
	if(!fp)
		return exit_clean("Could not open png for writing\n");

	png_bytep row_pointers[height];
	for(uint32_t i = 0; i < height; i++) {
		row_pointers[i] = (buffer + i * width * 3);
	}

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if(!png)
		return exit_clean("Could not create png write struct\n");

	png_infop info = png_create_info_struct(png);
	if (!info)
		return exit_clean("Could not create png info struct\n");

	if(setjmp(png_jmpbuf(png)))
		return exit_clean("Could not set png jmpbuf\n");

	png_init_io(png, fp);

	png_set_IHDR(
		png, info, width, height, 8,
		PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT
	);
	png_write_info(png, info);

	png_write_image(png, row_pointers);
	png_write_end(png, info);

	fclose(fp);
	png_destroy_write_struct(&png, &info);

	return 0;
}

int main(int argc, char** argv) {
	if(argc > 1) {
		if(!strcmp(argv[1], "-o"))
			opaque_mode = 1;
		else if(!strcmp(argv[1], "-t"))
			opaque_mode = 0;
	}

	display = XOpenDisplay(NULL);
	if(!display)
		exit_clean("Failed to open X display\n");
	root = DefaultRootWindow(display);
	// query windows for hover selection. this may result in undefined behavior if a window is opened while the overlay is displayed.
	Window r_ret, p_ret;
	Window* wins;
	uint32_t nwins;
	uint32_t hovn;
	XQueryTree(display, root, &r_ret, &p_ret, &wins, &nwins);

	XWindowAttributes gwa;
	XGetWindowAttributes(display, root, &gwa);
	XVisualInfo vinfo;
	// if there are compositor issues (black screen or blur) we can
	// create a 24 bit image and paint a temporary screenshot onto it
	// to achieve the same effect as a fully transparent 32 bit image.
	if(opaque_mode)
		XMatchVisualInfo(display, DefaultScreen(display), 24, TrueColor, &vinfo);
	else
		XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &vinfo);

	XSetWindowAttributes attrs;
	attrs.override_redirect = 1;
	attrs.colormap = XCreateColormap(display, root, vinfo.visual, AllocNone);
	attrs.background_pixel = 0;
	attrs.border_pixel = 0;
	// this is somewhat scuffed but we need to send an exposure event otherwise the temp screenshot will sometimes be blank.
	if(opaque_mode) {
		XSelectInput(display, root, ExposureMask);
		XEvent evt;
		XSendEvent(display, root, 1, ExposureMask, &evt);
		img = XGetImage(display, root, 0, 0, gwa.width, gwa.height, AllPlanes, ZPixmap);
	}
// if compositor supports transparency we can create a transparent window overlay which is pretty efficient. Otherwise we have to take a temp screenshot and draw it to the overlay instead.
	overlay = XCreateWindow( display, root, 0, 0, gwa.width, gwa.height, 0, vinfo.depth, InputOutput, vinfo.visual, CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel, &attrs);
	if(opaque_mode) {
		ogc = XCreateGC(display, overlay, 0, 0);
		pm = XCreatePixmap(display, overlay, gwa.width, gwa.height, 24);
		XPutImage(display, overlay, ogc, img, 0, 0, 0, 0, gwa.width, gwa.height);
	}

	XMapWindow(display, overlay);

	XGCValues gcval;
	gcval.foreground = XWhitePixel(display, 0);
	gcval.function = GXxor;
	gcval.background = XBlackPixel(display, 0);
	gcval.plane_mask = gcval.background ^ gcval.foreground;
	gcval.subwindow_mode = IncludeInferiors;
	gc = XCreateGC(display, overlay, GCFunction | GCForeground | GCBackground | GCSubwindowMode, &gcval);

	cursor = XCreateFontCursor(display, CURSOR);
	XDefineCursor(display, overlay, cursor);

	uint32_t wx, wy;		// junk
	Window cw, rw;			// junk

	uint32_t startx, starty, endx, endy, temp;
	int grabbing = 0;
	uint32_t mousex, mousey, mask;
	uint32_t bsx, bsy, bex, bey;

	keymap = malloc(sizeof(uint8_t) * 32);
	for(;;) {
		// safe exit on keypress
		XQueryKeymap(display, keymap);
		KeyCode kc = XKeysymToKeycode(display, KQUIT);
		int pressed = !!(keymap[kc>>3] & (1<<(kc&7))); // esc key
		if(pressed) return exit_clean(NULL);

		XQueryPointer(display, root, &cw, &rw, &mousex, &mousey, &wx, &wy, &mask);

		XWindowAttributes a;
		for(uint32_t i=1; i<nwins; i++) {
			XGetWindowAttributes(display, wins[i], &a);
			if(mousex >= a.x && mousex <= a.x+a.width  &&
			   mousey >= a.y && mousey <= a.y+a.height &&
			   a.map_state == IsViewable) {
				hoverw = a;
				hovn   = i;
				XGetGeometry(display, wins[i], &cw, &bsx, &bsy, &bex, &bey, &wx, &wy);
			}
		}

		if(mask == 256) {			// left click
			if(!grabbing) {
				grabbing = 1;
				startx = mousex;
				starty = mousey;
			}
		} else if(mask == 1024) {	// right click
			if(!grabbing) {
				grabbing = 1;
				startx = mousex;
				starty = mousey;
			}
		}
		else if (grabbing) {
				grabbing = 0;
				endx = mousex;
				endy = mousey;
				break;
			}

		if(opaque_mode)
			XPutImage(display, pm, ogc, img, 0, 0, 0, 0, gwa.width, gwa.height);
		else
			XClearArea(display, overlay, 0, 0, gwa.width, gwa.height, 0);

		if(grabbing) {
			bsx = (mousex > startx) ? startx-1 : mousex-1;
			bsy = (mousey > starty) ? starty-1 : mousey-1;
			bex = (mousex > startx) ? mousex-startx+3 : startx-mousex+3;
			bey = (mousey > starty) ? mousey-starty+3 : starty-mousey+1;
		}
		if(opaque_mode) {
		    XDrawRectangle(display, pm, gc, bsx, bsy, bex, bey);
		    XClearArea(display, overlay, 0, 0, gwa.width, gwa.height, 0);
		    XSetWindowBackgroundPixmap(display, overlay, pm);
		} else {
			XDrawRectangle(display, overlay, gc, bsx, bsy, bex, bey);
		}

		XFlush(display);
		usleep(POLLRATE * 1000); // experiment as needed.
	}
	uint32_t width, height;
	if(startx == endx || starty == endy) {
		width  = hoverw.width;
		height = hoverw.height;

		img = XGetImage(display, wins[hovn], 0, 0, width, height, AllPlanes, ZPixmap);
		uint32_t rmask = img->red_mask;
		uint32_t gmask = img->green_mask;
		uint32_t bmask = img->blue_mask;

		buffer = malloc(sizeof(uint8_t) * width * height * 3);
		if(!buffer) {
			return exit_clean("Could not allocate image buffer\n");
		}
		uint32_t c = 0;
		for(uint32_t h = 0; h < height; h++) {
			for(uint32_t w = 0; w < width; w++) {
				uint32_t pix = XGetPixel(img, w, h);
				uint8_t r = (pix & rmask) >> 16;
				uint8_t g = (pix & gmask) >> 8;
				uint8_t b = pix & bmask;

				buffer[c++] = r;
				buffer[c++] = g;
				buffer[c++] = b;
			}
		}
	} else {
		// more corner flipping.
		if(startx > endx) {
			temp = startx;
			startx = endx;
			endx = temp;
		}
		if(starty > endy) {
			temp = starty;
			starty = endy;
			endy = temp;
		}

		width = endx+1-startx;
		height = endy-starty;

		img = XGetImage(display, root, 0, 0, gwa.width, gwa.height, AllPlanes, ZPixmap);
		uint32_t rmask = img->red_mask;
		uint32_t gmask = img->green_mask;
		uint32_t bmask = img->blue_mask;

		buffer = malloc(sizeof(uint8_t) * width * height * 3);
		if(!buffer) {
			return exit_clean("Could not allocate image buffer\n");
		}

		uint32_t c = 0;
		for(uint32_t h = starty+1; h < endy+1; h++) {
			for(uint32_t w = startx+1; w < endx+2; w++) {
				uint32_t pix = XGetPixel(img, w, h);
				uint8_t r = (pix & rmask) >> 16;
				uint8_t g = (pix & gmask) >> 8;
				uint8_t b = pix & bmask;

				buffer[c++] = r;
				buffer[c++] = g;
				buffer[c++] = b;
			}
		}
	}
	create_filename(&fpath);

	int32_t png = create_png(buffer, width, height, fpath);
	if(png != 0) return png;

	exit_clean(NULL);
	return 0;
}

