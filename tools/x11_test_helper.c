/*
 * x11_test_helper -- drive and inspect an X server for the X11 front end's
 * tests (tests/e2e/x11_e2e.py). A test tool, not part of the client.
 *
 * Input goes through XTest, so it reaches the client the way a user's does:
 * through the server, to whichever window has the pointer and the focus. XTest
 * is loaded at run time (libXtst.so.6), so building this needs only Xlib; a host
 * without the library exits 77, which the harness reports as SKIPPED.
 *
 *   x11_test_helper STEP...
 *     move X Y            pointer to (X, Y) on the root window
 *     click X Y           move, then press and release button 1
 *     key KEYSYM          press and release a key (KEYSYM as XStringToKeysym)
 *     chord MOD+...+KEYSYM  e.g. ctrl+a, ctrl+alt+Home: modifiers held around it
 *     type TEXT           one key per character of TEXT (ASCII letters/digits)
 *     sleep MS
 *     wait NAME SECONDS   until a top-level window titled NAME is mapped
 *     geom NAME           print "W H" of that window
 *     resize NAME W H     resize it (there is no window manager to do it)
 *     pixel NAME X Y      print the window's pixel at (X, Y) as RRGGBB
 *     snap NAME FILE      write the window's contents to FILE as a binary PPM
 *     clip-get            print the CLIPBOARD selection (UTF8_STRING), or
 *                         "<none>" when nobody owns it
 *     clip-set TEXT SECS  own CLIPBOARD with TEXT and serve it for SECS
 *                         seconds (blocks; run it in the background)
 *
 * Exit status: 0 done, 1 a step failed, 2 usage, 77 no XTest.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <dlfcn.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int (*FakeMotion)(Display*, int, int, int, unsigned long);
typedef int (*FakeButton)(Display*, unsigned int, Bool, unsigned long);
typedef int (*FakeKey)(Display*, unsigned int, Bool, unsigned long);

static FakeMotion fake_motion;
static FakeButton fake_button;
static FakeKey fake_key;

static int load_xtest(void)
{
	void* lib = dlopen("libXtst.so.6", RTLD_NOW);
	if (lib == NULL)
		lib = dlopen("libXtst.so", RTLD_NOW);
	if (lib == NULL)
		return 0;
	fake_motion = (FakeMotion)dlsym(lib, "XTestFakeMotionEvent");
	fake_button = (FakeButton)dlsym(lib, "XTestFakeButtonEvent");
	fake_key = (FakeKey)dlsym(lib, "XTestFakeKeyEvent");
	return fake_motion != NULL && fake_button != NULL && fake_key != NULL;
}

static void pause_ms(int ms)
{
	struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
}

/* The first top-level window whose title contains `name`. */
static Window find_window(Display* d, const char* name)
{
	Window root = DefaultRootWindow(d), parent, *children = NULL;
	unsigned int count = 0;
	Window found = None;
	if (!XQueryTree(d, root, &root, &parent, &children, &count))
		return None;
	for (unsigned int i = 0; i < count && found == None; i++) {
		char* title = NULL;
		XWindowAttributes attributes;
		if (XFetchName(d, children[i], &title) && title != NULL) {
			if (strstr(title, name) != NULL
				&& XGetWindowAttributes(d, children[i], &attributes)
				&& attributes.map_state == IsViewable)
				found = children[i];
			XFree(title);
		}
	}
	if (children != NULL)
		XFree(children);
	return found;
}

static void key_tap(Display* d, KeySym sym)
{
	const KeyCode code = XKeysymToKeycode(d, sym);
	fake_key(d, code, True, 0);
	XFlush(d);
	pause_ms(20);
	fake_key(d, code, False, 0);
	XFlush(d);
	pause_ms(20);
}

static int chord(Display* d, char* spec)
{
	KeySym held[4];
	int count = 0;
	char* save = NULL;
	char* part = strtok_r(spec, "+", &save);
	char* last = NULL;
	while (part != NULL) {
		char* next = strtok_r(NULL, "+", &save);
		if (next == NULL) {
			last = part;
			break;
		}
		if (count < 4) {
			if (!strcmp(part, "ctrl"))
				held[count++] = XK_Control_L;
			else if (!strcmp(part, "alt"))
				held[count++] = XK_Alt_L;
			else if (!strcmp(part, "shift"))
				held[count++] = XK_Shift_L;
			else
				return 0;
		}
		part = next;
	}
	if (last == NULL)
		return 0;
	const KeySym sym = XStringToKeysym(last);
	if (sym == NoSymbol)
		return 0;
	for (int i = 0; i < count; i++) {
		fake_key(d, XKeysymToKeycode(d, held[i]), True, 0);
		XFlush(d);
		pause_ms(20);
	}
	key_tap(d, sym);
	for (int i = count - 1; i >= 0; i--) {
		fake_key(d, XKeysymToKeycode(d, held[i]), False, 0);
		XFlush(d);
		pause_ms(20);
	}
	return 1;
}

static int clip_get(Display* d)
{
	const Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
	const Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
	const Atom property = XInternAtom(d, "X11_TEST_HELPER", False);
	if (XGetSelectionOwner(d, clipboard) == None) {
		printf("<none>\n");
		return 0;
	}
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
	XConvertSelection(d, clipboard, utf8, property, w, CurrentTime);
	XFlush(d);
	for (int waited = 0; waited < 3000; waited += 10) {
		XEvent event;
		while (XPending(d)) {
			XNextEvent(d, &event);
			if (event.type != SelectionNotify)
				continue;
			if (event.xselection.property == None) {
				printf("<refused>\n");
				return 1;
			}
			Atom type;
			int format;
			unsigned long n, after;
			unsigned char* data = NULL;
			XGetWindowProperty(d, w, property, 0, 1 << 20, True, AnyPropertyType,
				&type, &format, &n, &after, &data);
			printf("%.*s\n", (int)n, data != NULL ? (char*)data : "");
			if (data != NULL)
				XFree(data);
			return 0;
		}
		pause_ms(10);
	}
	printf("<timeout>\n");
	return 1;
}

static int clip_set(Display* d, const char* text, int seconds)
{
	const Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
	const Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
	const Atom targets = XInternAtom(d, "TARGETS", False);
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
	XSetSelectionOwner(d, clipboard, w, CurrentTime);
	if (XGetSelectionOwner(d, clipboard) != w)
		return 1;
	printf("owning\n");
	fflush(stdout);
	const time_t end = time(NULL) + seconds;
	while (time(NULL) < end) {
		XEvent event;
		if (!XPending(d)) {
			struct pollfd fd = {ConnectionNumber(d), POLLIN, 0};
			poll(&fd, 1, 100);
			continue;
		}
		XNextEvent(d, &event);
		if (event.type == SelectionClear)
			return 0;
		if (event.type != SelectionRequest)
			continue;
		XSelectionRequestEvent* r = &event.xselectionrequest;
		XEvent reply;
		memset(&reply, 0, sizeof reply);
		reply.xselection.type = SelectionNotify;
		reply.xselection.requestor = r->requestor;
		reply.xselection.selection = r->selection;
		reply.xselection.target = r->target;
		reply.xselection.time = r->time;
		reply.xselection.property = None;
		if (r->target == targets) {
			Atom list[2] = {targets, utf8};
			XChangeProperty(d, r->requestor, r->property, XA_ATOM, 32,
				PropModeReplace, (unsigned char*)list, 2);
			reply.xselection.property = r->property;
		} else if (r->target == utf8 || r->target == XA_STRING) {
			XChangeProperty(d, r->requestor, r->property, r->target, 8,
				PropModeReplace, (const unsigned char*)text, (int)strlen(text));
			reply.xselection.property = r->property;
		}
		XSendEvent(d, r->requestor, False, 0, &reply);
		XFlush(d);
	}
	return 0;
}

int main(int argc, char** argv)
{
	Display* d = XOpenDisplay(NULL);
	if (d == NULL) {
		fprintf(stderr, "x11_test_helper: no display\n");
		return 1;
	}
	int need_xtest = 0;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "move") || !strcmp(argv[i], "click")
			|| !strcmp(argv[i], "key") || !strcmp(argv[i], "chord")
			|| !strcmp(argv[i], "type"))
			need_xtest = 1;
	if (need_xtest && !load_xtest()) {
		fprintf(stderr, "x11_test_helper: XTest (libXtst.so.6) not available\n");
		return 77;
	}
	for (int i = 1; i < argc; i++) {
		const char* step = argv[i];
		const int left = argc - i - 1;
		if (!strcmp(step, "move") && left >= 2) {
			fake_motion(d, -1, atoi(argv[i + 1]), atoi(argv[i + 2]), 0);
			i += 2;
		} else if (!strcmp(step, "click") && left >= 2) {
			fake_motion(d, -1, atoi(argv[i + 1]), atoi(argv[i + 2]), 0);
			XFlush(d);
			pause_ms(120);
			fake_button(d, 1, True, 0);
			XFlush(d);
			pause_ms(60);
			fake_button(d, 1, False, 0);
			i += 2;
		} else if (!strcmp(step, "key") && left >= 1) {
			const KeySym sym = XStringToKeysym(argv[i + 1]);
			if (sym == NoSymbol)
				return 2;
			key_tap(d, sym);
			i += 1;
		} else if (!strcmp(step, "chord") && left >= 1) {
			if (!chord(d, argv[i + 1]))
				return 2;
			i += 1;
		} else if (!strcmp(step, "type") && left >= 1) {
			for (const char* c = argv[i + 1]; *c != '\0'; c++) {
				char name[2] = {*c, '\0'};
				const KeySym sym = XStringToKeysym(name);
				if (sym == NoSymbol)
					return 2;
				key_tap(d, sym);
			}
			i += 1;
		} else if (!strcmp(step, "sleep") && left >= 1) {
			XFlush(d);
			pause_ms(atoi(argv[i + 1]));
			i += 1;
		} else if (!strcmp(step, "wait") && left >= 2) {
			const int deadline = atoi(argv[i + 2]) * 1000;
			int waited = 0;
			while (find_window(d, argv[i + 1]) == None && waited < deadline) {
				pause_ms(50);
				waited += 50;
			}
			if (find_window(d, argv[i + 1]) == None) {
				fprintf(stderr, "x11_test_helper: no window \"%s\"\n", argv[i + 1]);
				return 1;
			}
			i += 2;
		} else if (!strcmp(step, "geom") && left >= 1) {
			const Window w = find_window(d, argv[i + 1]);
			XWindowAttributes a;
			if (w == None || !XGetWindowAttributes(d, w, &a))
				return 1;
			printf("%d %d\n", a.width, a.height);
			i += 1;
		} else if (!strcmp(step, "resize") && left >= 3) {
			const Window w = find_window(d, argv[i + 1]);
			if (w == None)
				return 1;
			XResizeWindow(d, w, (unsigned)atoi(argv[i + 2]), (unsigned)atoi(argv[i + 3]));
			XFlush(d);
			i += 3;
		} else if (!strcmp(step, "pixel") && left >= 3) {
			const Window w = find_window(d, argv[i + 1]);
			if (w == None)
				return 1;
			XImage* image = XGetImage(d, w, atoi(argv[i + 2]), atoi(argv[i + 3]),
				1, 1, AllPlanes, ZPixmap);
			if (image == NULL)
				return 1;
			const unsigned long p = XGetPixel(image, 0, 0);
			printf("%02lx%02lx%02lx\n", (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
			XDestroyImage(image);
			i += 3;
		} else if (!strcmp(step, "snap") && left >= 2) {
			const Window w = find_window(d, argv[i + 1]);
			XWindowAttributes a;
			if (w == None || !XGetWindowAttributes(d, w, &a))
				return 1;
			XImage* image = XGetImage(d, w, 0, 0, (unsigned)a.width,
				(unsigned)a.height, AllPlanes, ZPixmap);
			FILE* out = fopen(argv[i + 2], "wb");
			if (image == NULL || out == NULL)
				return 1;
			fprintf(out, "P6\n%d %d\n255\n", a.width, a.height);
			for (int y = 0; y < a.height; y++)
				for (int x = 0; x < a.width; x++) {
					const unsigned long p = XGetPixel(image, x, y);
					const unsigned char rgb[3] = {(unsigned char)(p >> 16),
						(unsigned char)(p >> 8), (unsigned char)p};
					fwrite(rgb, 1, 3, out);
				}
			fclose(out);
			XDestroyImage(image);
			i += 2;
		} else if (!strcmp(step, "clip-get")) {
			if (clip_get(d) != 0)
				return 1;
		} else if (!strcmp(step, "clip-set") && left >= 2) {
			if (clip_set(d, argv[i + 1], atoi(argv[i + 2])) != 0)
				return 1;
			i += 2;
		} else {
			fprintf(stderr, "x11_test_helper: bad step \"%s\"\n", step);
			return 2;
		}
		XFlush(d);
	}
	XSync(d, False);
	XCloseDisplay(d);
	return 0;
}
