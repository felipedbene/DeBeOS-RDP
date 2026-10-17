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
 *     clip-get-file TARGET FILE
 *                         request CLIPBOARD as TARGET (UTF8_STRING, STRING),
 *                         following INCR, into FILE; prints
 *                         "got N bytes type T incr K max M"
 *     clip-abort TARGET N the same, but exit after N INCR chunks: a requestor
 *                         that vanishes mid-transfer
 *     clip-serve-file FILE SECS DIE
 *                         own CLIPBOARD with FILE's bytes as UTF8_STRING for
 *                         SECS seconds, through INCR (64 KiB chunks) when they
 *                         exceed one request; DIE > 0: exit after DIE chunks,
 *                         an owner that vanishes mid-transfer (blocks); prints
 *                         "lost ownership" when another client takes it
 *     clip-query TARGET   print the owner's answer to TARGETS or TIMESTAMP as
 *                         "TYPE/FORMAT: values..."
 *     wm SECS             be a minimal EWMH window manager for SECS seconds
 *                         (or until SIGTERM): maps, configures, and honours
 *                         _NET_WM_STATE_FULLSCREEN; logs each request (blocks)
 *
 * Exit status: 0 done, 1 a step failed, 2 usage, 77 no XTest.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
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

/* --- large selections and ICCCM INCR (ICCCM 2.7.2) ------------------------
 *
 * Written from the ICCCM, independently of the client's own implementation in
 * src/x11_main.cpp, so a transfer that works end to end is evidence that both
 * sides follow the convention and not merely each other.
 */

/* Bytes of a file, malloc'd; NULL when it cannot be read. */
static unsigned char* read_file(const char* path, size_t* size)
{
	FILE* f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	size_t capacity = 1 << 16, length = 0;
	unsigned char* data = malloc(capacity);
	size_t n;
	while (data != NULL && (n = fread(data + length, 1, capacity - length, f)) > 0) {
		length += n;
		if (length == capacity) {
			capacity *= 2;
			data = realloc(data, capacity);
		}
	}
	fclose(f);
	*size = length;
	return data;
}

/* Wait (up to `ms`) for an event on `w` matching `type`, and for PropertyNotify
 * also `atom` and `state`. Other events are dropped. */
static int wait_event(Display* d, Window w, int type, Atom atom, int state, int ms,
	XEvent* out)
{
	for (int waited = 0; waited <= ms; waited += 5) {
		while (XPending(d)) {
			XNextEvent(d, out);
			if (out->type != type)
				continue;
			if (type == PropertyNotify
				&& (out->xproperty.window != w || out->xproperty.atom != atom
					|| out->xproperty.state != state))
				continue;
			if (type == SelectionNotify && out->xselection.requestor != w)
				continue;
			return 1;
		}
		pause_ms(5);
	}
	return 0;
}

/* Request CLIPBOARD as `target_name` and write what arrives to `path` (or
 * print it, as text, when path is NULL). Follows INCR. With abort_after > 0,
 * exit the process -- its window with it -- after that many INCR chunks: a
 * requestor that disappears mid-transfer. Prints one line:
 *   "got N bytes type T incr K max M"  (K chunks of at most M bytes; K 0 when
 *   it came in one piece)
 */
static int clip_receive(Display* d, const char* target_name, const char* path,
	int abort_after)
{
	const Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
	const Atom target = XInternAtom(d, target_name, False);
	const Atom incr = XInternAtom(d, "INCR", False);
	const Atom property = XInternAtom(d, "X11_TEST_HELPER", False);
	if (XGetSelectionOwner(d, clipboard) == None) {
		printf("<none>\n");
		return 0;
	}
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
	XSelectInput(d, w, PropertyChangeMask);
	XConvertSelection(d, clipboard, target, property, w, CurrentTime);
	XFlush(d);
	XEvent event;
	if (!wait_event(d, w, SelectionNotify, None, 0, 3000, &event)) {
		printf("<timeout>\n");
		return 1;
	}
	if (event.xselection.property == None) {
		printf("<refused>\n");
		return 1;
	}
	size_t capacity = 1 << 16, length = 0;
	unsigned char* all = malloc(capacity);
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char* data = NULL;
	XGetWindowProperty(d, w, property, 0, 0x7fffffff / 4, False, AnyPropertyType,
		&type, &format, &n, &after, &data);
	Atom final_type = type;
	int chunks = 0;
	unsigned long largest = 0;
	if (type == incr) {
		/* Deleting the INCR property asks for the first chunk. */
		if (data != NULL)
			XFree(data);
		XDeleteProperty(d, w, property);
		XFlush(d);
		for (;;) {
			if (!wait_event(d, w, PropertyNotify, property, PropertyNewValue, 5000,
					&event)) {
				printf("<incr timeout after %d chunks>\n", chunks);
				return 1;
			}
			data = NULL;
			/* The chunk we abort after is read without deleting it: the
			 * deletion goes out below, together with the window's end. */
			const Bool last = abort_after > 0 && chunks + 1 >= abort_after;
			XGetWindowProperty(d, w, property, 0, 0x7fffffff / 4, !last,
				AnyPropertyType, &type, &format, &n, &after, &data);
			XFlush(d);
			if (n == 0) {
				if (data != NULL)
					XFree(data);
				break;
			}
			final_type = type;
			while (length + n > capacity) {
				capacity *= 2;
				all = realloc(all, capacity);
			}
			memcpy(all + length, data, n);
			length += n;
			XFree(data);
			chunks++;
			if (n > largest)
				largest = n;
			if (abort_after > 0 && chunks >= abort_after) {
				/* Deleting the chunk asks the owner for the next; destroying
				 * the window in the same request batch means the server has
				 * destroyed it before the owner can write that next chunk,
				 * so the owner's write always meets a window that is gone
				 * (BadWindow), not just sometimes. */
				XDeleteProperty(d, w, property);
				XDestroyWindow(d, w);
				XSync(d, False);
				printf("aborted after %d chunks\n", chunks);
				fflush(stdout);
				_exit(0);
			}
		}
	} else {
		while (length + n > capacity) {
			capacity *= 2;
			all = realloc(all, capacity);
		}
		if (data != NULL) {
			memcpy(all + length, data, n);
			length += n;
			XFree(data);
		}
		XDeleteProperty(d, w, property);
	}
	char* type_name = final_type != None ? XGetAtomName(d, final_type) : NULL;
	if (path != NULL) {
		FILE* f = fopen(path, "wb");
		if (f == NULL || fwrite(all, 1, length, f) != length)
			return 1;
		fclose(f);
		printf("got %zu bytes type %s incr %d max %lu\n", length,
			type_name != NULL ? type_name : "None", chunks, largest);
	} else {
		printf("%.*s\n", (int)length, (char*)all);
	}
	if (type_name != NULL)
		XFree(type_name);
	free(all);
	return 0;
}

/* Print what the owner answers to TARGETS (atom names, space separated) or to
 * TIMESTAMP (the INTEGER). */
static int clip_query(Display* d, const char* what)
{
	const Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
	const Atom target = XInternAtom(d, what, False);
	const Atom property = XInternAtom(d, "X11_TEST_HELPER", False);
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
	XConvertSelection(d, clipboard, target, property, w, CurrentTime);
	XFlush(d);
	XEvent event;
	if (!wait_event(d, w, SelectionNotify, None, 0, 3000, &event)
		|| event.xselection.property == None) {
		printf("<refused>\n");
		return 1;
	}
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char* data = NULL;
	XGetWindowProperty(d, w, property, 0, 1024, True, AnyPropertyType, &type, &format,
		&n, &after, &data);
	char* type_name = XGetAtomName(d, type);
	printf("%s/%d:", type_name, format);
	XFree(type_name);
	for (unsigned long i = 0; i < n && format == 32; i++) {
		const unsigned long value = ((unsigned long*)data)[i];
		if (type == XA_ATOM) {
			char* name = XGetAtomName(d, (Atom)value);
			printf(" %s", name);
			XFree(name);
		} else {
			printf(" %lu", value);
		}
	}
	printf("\n");
	if (data != NULL)
		XFree(data);
	return 0;
}

/* Own CLIPBOARD with the bytes of `path` as UTF8_STRING (and TARGETS) for
 * `seconds`, serving a request larger than one X request through INCR in 64
 * KiB chunks. With die_after > 0, exit -- taking the window with it -- right
 * after sending that many chunks: an owner that disappears mid-transfer.
 * Prints "owning" once it owns, then "served N bytes in K chunks" per INCR
 * transfer it completes. */
static int clip_serve_file(Display* d, const char* path, int seconds, int die_after)
{
	size_t size = 0;
	unsigned char* bytes = read_file(path, &size);
	if (bytes == NULL)
		return 1;
	const Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
	const Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
	const Atom targets = XInternAtom(d, "TARGETS", False);
	const Atom incr = XInternAtom(d, "INCR", False);
	const size_t limit = (size_t)XMaxRequestSize(d) * 4 - 100;
	const size_t chunk = 65536;
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
	XSetSelectionOwner(d, clipboard, w, CurrentTime);
	if (XGetSelectionOwner(d, clipboard) != w)
		return 1;
	printf("owning\n");
	fflush(stdout);
	/* The one INCR transfer under way, if any. */
	Window to = None;
	Atom to_property = None;
	size_t sent = 0;
	int chunks = 0;
	const time_t end = time(NULL) + seconds;
	while (time(NULL) < end) {
		XEvent event;
		if (!XPending(d)) {
			struct pollfd fd = {ConnectionNumber(d), POLLIN, 0};
			poll(&fd, 1, 100);
			continue;
		}
		XNextEvent(d, &event);
		if (event.type == SelectionClear) {
			printf("lost ownership\n");
			fflush(stdout);
			return 0;
		}
		if (event.type == PropertyNotify && event.xproperty.window == to
			&& event.xproperty.atom == to_property
			&& event.xproperty.state == PropertyDelete) {
			const size_t n = size - sent < chunk ? size - sent : chunk;
			XChangeProperty(d, to, to_property, utf8, 8, PropModeReplace,
				bytes + sent, (int)n);
			XFlush(d);
			sent += n;
			if (n == 0) {
				printf("served %zu bytes in %d chunks\n", size, chunks);
				fflush(stdout);
				XSelectInput(d, to, NoEventMask);
				to = None;
				continue;
			}
			chunks++;
			if (die_after > 0 && chunks >= die_after) {
				XSync(d, False);
				printf("dying after %d chunks\n", chunks);
				fflush(stdout);
				_exit(0);
			}
			continue;
		}
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
		} else if (r->target == utf8 && size <= limit) {
			XChangeProperty(d, r->requestor, r->property, utf8, 8,
				PropModeReplace, bytes, (int)size);
			reply.xselection.property = r->property;
		} else if (r->target == utf8 && to == None) {
			XSelectInput(d, r->requestor, PropertyChangeMask);
			const long lower_bound = (long)size;
			XChangeProperty(d, r->requestor, r->property, incr, 32,
				PropModeReplace, (const unsigned char*)&lower_bound, 1);
			to = r->requestor;
			to_property = r->property;
			sent = 0;
			chunks = 0;
			reply.xselection.property = r->property;
		}
		XSendEvent(d, r->requestor, False, 0, &reply);
		XFlush(d);
	}
	return 0;
}

/* --- a minimal EWMH window manager ------------------------------------------
 *
 * Just enough of one for the full-screen test: it maps and configures what is
 * asked, advertises _NET_WM_STATE_FULLSCREEN, and honours _NET_WM_STATE client
 * messages for it (EWMH 1.5, "_NET_WM_STATE") by covering the screen and
 * recording the state, restoring the old geometry on the way out. Each request
 * is logged as "fullscreen action=A source=S -> on|off". Runs for `seconds` or
 * until SIGTERM, then removes its root properties.
 */
static volatile sig_atomic_t wm_stop;
static void on_wm_signal(int sig)
{
	(void)sig;
	wm_stop = 1;
}

static int wm_failed;
static int on_wm_error(Display* d, XErrorEvent* e)
{
	(void)d;
	if (e->error_code == BadAccess)
		wm_failed = 1;
	return 0;
}

static int has_fullscreen(Display* d, Window w, Atom state, Atom fullscreen)
{
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char* data = NULL;
	int found = 0;
	if (XGetWindowProperty(d, w, state, 0, 64, False, XA_ATOM, &type, &format, &n,
			&after, &data) == Success && data != NULL) {
		for (unsigned long i = 0; i < n; i++)
			if (((unsigned long*)data)[i] == fullscreen)
				found = 1;
		XFree(data);
	}
	return found;
}

static int run_wm(Display* d, int seconds)
{
	const Window root = DefaultRootWindow(d);
	const Atom check = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False);
	const Atom supported = XInternAtom(d, "_NET_SUPPORTED", False);
	const Atom state = XInternAtom(d, "_NET_WM_STATE", False);
	const Atom fullscreen = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
	XSetErrorHandler(on_wm_error);
	XSelectInput(d, root, SubstructureRedirectMask | SubstructureNotifyMask);
	XSync(d, False);
	if (wm_failed) {
		fprintf(stderr, "x11_test_helper: another window manager is running\n");
		return 1;
	}
	signal(SIGTERM, on_wm_signal);
	const Window self = XCreateSimpleWindow(d, root, -10, -10, 1, 1, 0, 0, 0);
	XChangeProperty(d, root, check, XA_WINDOW, 32, PropModeReplace,
		(const unsigned char*)&self, 1);
	XChangeProperty(d, self, check, XA_WINDOW, 32, PropModeReplace,
		(const unsigned char*)&self, 1);
	const Atom list[2] = {state, fullscreen};
	XChangeProperty(d, root, supported, XA_ATOM, 32, PropModeReplace,
		(const unsigned char*)list, 2);
	XSync(d, False);
	printf("wm running\n");
	fflush(stdout);
	struct {
		Window w;
		int x, y, width, height;
	} saved[16];
	int saved_count = 0;
	const time_t end = time(NULL) + seconds;
	while (!wm_stop && time(NULL) < end) {
		if (!XPending(d)) {
			struct pollfd fd = {ConnectionNumber(d), POLLIN, 0};
			poll(&fd, 1, 100);
			continue;
		}
		XEvent e;
		XNextEvent(d, &e);
		if (e.type == MapRequest) {
			XMapWindow(d, e.xmaprequest.window);
		} else if (e.type == ConfigureRequest) {
			XWindowChanges changes;
			changes.x = e.xconfigurerequest.x;
			changes.y = e.xconfigurerequest.y;
			changes.width = e.xconfigurerequest.width;
			changes.height = e.xconfigurerequest.height;
			changes.border_width = e.xconfigurerequest.border_width;
			changes.sibling = e.xconfigurerequest.above;
			changes.stack_mode = e.xconfigurerequest.detail;
			XConfigureWindow(d, e.xconfigurerequest.window,
				(unsigned)e.xconfigurerequest.value_mask, &changes);
		} else if (e.type == ClientMessage && e.xclient.message_type == state
			&& ((Atom)e.xclient.data.l[1] == fullscreen
				|| (Atom)e.xclient.data.l[2] == fullscreen)) {
			const Window w = e.xclient.window;
			const long action = e.xclient.data.l[0];
			const int now = has_fullscreen(d, w, state, fullscreen);
			const int want = action == 1 ? 1 : action == 0 ? 0 : !now;
			printf("fullscreen action=%ld source=%ld -> %s\n", action,
				e.xclient.data.l[3], want ? "on" : "off");
			fflush(stdout);
			if (want && !now && saved_count < 16) {
				XWindowAttributes a;
				XGetWindowAttributes(d, w, &a);
				saved[saved_count].w = w;
				saved[saved_count].x = a.x;
				saved[saved_count].y = a.y;
				saved[saved_count].width = a.width;
				saved[saved_count].height = a.height;
				saved_count++;
				XMoveResizeWindow(d, w, 0, 0,
					(unsigned)DisplayWidth(d, DefaultScreen(d)),
					(unsigned)DisplayHeight(d, DefaultScreen(d)));
				XChangeProperty(d, w, state, XA_ATOM, 32, PropModeReplace,
					(const unsigned char*)&fullscreen, 1);
			} else if (!want && now) {
				for (int i = saved_count - 1; i >= 0; i--) {
					if (saved[i].w != w)
						continue;
					XMoveResizeWindow(d, w, saved[i].x, saved[i].y,
						(unsigned)saved[i].width, (unsigned)saved[i].height);
					saved[i] = saved[--saved_count];
					break;
				}
				XChangeProperty(d, w, state, XA_ATOM, 32, PropModeReplace, NULL, 0);
			}
		}
		XFlush(d);
	}
	XDeleteProperty(d, root, check);
	XDeleteProperty(d, root, supported);
	XSync(d, False);
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
		} else if (!strcmp(step, "clip-get-file") && left >= 2) {
			if (clip_receive(d, argv[i + 1], argv[i + 2], 0) != 0)
				return 1;
			i += 2;
		} else if (!strcmp(step, "clip-abort") && left >= 2) {
			if (clip_receive(d, argv[i + 1], "/dev/null", atoi(argv[i + 2])) != 0)
				return 1;
			i += 2;
		} else if (!strcmp(step, "clip-serve-file") && left >= 3) {
			if (clip_serve_file(d, argv[i + 1], atoi(argv[i + 2]), atoi(argv[i + 3])) != 0)
				return 1;
			i += 3;
		} else if (!strcmp(step, "clip-query") && left >= 1) {
			if (clip_query(d, argv[i + 1]) != 0)
				return 1;
			i += 1;
		} else if (!strcmp(step, "wm") && left >= 1) {
			if (run_wm(d, atoi(argv[i + 1])) != 0)
				return 1;
			i += 1;
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
