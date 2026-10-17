# `CXX ?= g++` alone cannot win against make's *built-in* CXX: ?= only assigns
# when the variable is undefined, and a built-in default counts as defined. On
# most hosts the built-in is `g++` and the difference is invisible, but native
# make on Haiku arm64 has a built-in `CXX = aarch64-unknown-haiku-g++` -- a
# cross-compiler name that does not exist there -- so plain `make` died with
# Error 127. Override only a value whose origin is "default", so a CXX from the
# environment or the command line still wins.
ifeq ($(origin CXX),default)
CXX := g++
endif
CXX ?= g++
PKG_CONFIG ?= pkg-config

PACKAGES := libpng freetype2
CPPFLAGS := -Iinclude $(shell $(PKG_CONFIG) --cflags $(PACKAGES) 2>/dev/null)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
# Disable floating-point contraction (fused multiply-add). The bilinear filter
# weights in src/surface.cpp are computed in floating point and then quantised;
# a compiler that contracts `a*b + c` into a single fma -- which clang does by
# default on arm64 where the hardware has it -- carries an extra bit of
# precision through the multiply and can shift a quantised weight by one. That
# breaks the render tests' bit-exact checks (the pixel-aligned tiled draw must
# match the same draw with the filter bit set, and the golden weight vectors),
# which held on x86 only because no fma was emitted there. Turning contraction
# off makes surface.cpp reproduce app_server's exact integer arithmetic on
# every target rather than only under the test, so it applies to the whole
# build, not just the test objects (they link the same surface.o).
CXXFLAGS += -ffp-contract=off
# managed_transport.cpp drains each child's stderr on a std::thread. (Haiku's
# libroot has the threads built in and its gcc has no -pthread.)
ifneq ($(shell uname -s),Haiku)
CXXFLAGS += -pthread
endif
# Regenerate object files when a header changes. Without this, editing a type in
# include/haiku_remote/ relinked stale objects that disagreed about a struct's
# layout, and the symptom was wrong pixels in an apparently clean build.
DEPFLAGS := -MMD -MP
LDLIBS := $(shell $(PKG_CONFIG) --libs $(PACKAGES) 2>/dev/null)
X11_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 2>/dev/null)
X11_LIBS := $(shell $(PKG_CONFIG) --libs x11 2>/dev/null)
HAS_X11 := $(shell $(PKG_CONFIG) --exists x11 && echo 1)
SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL2_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
HAS_SDL2 := $(shell $(PKG_CONFIG) --exists sdl2 && echo 1)
# Even without the full SDL2 dev package (which pkg-config reports and which is
# needed to *link* haiku-remote-gui), the SDL2 *headers* may still be on the
# include path. If they are, we can at least -fsyntax-only src/sdl_main.cpp so a
# typo there is caught on a link-incapable host, instead of shipping a
# translation unit that nothing ever compiles (issue #26). Probe by actually
# parsing SDL.h with whatever cflags pkg-config could give us.
#
# -include, not a preprocessor directive in a here-string: a literal '#' inside
# $(shell ...) is a comment to GNU make 3.81, which is what Apple ships as
# /usr/bin/make. 3.81 truncated this line at the '#' and died with
# "unterminated call to function `shell': missing `)'" -- so the whole Makefile
# was unparseable on stock macOS while building fine against make 4.x on Linux.
# Escaping as \# would also work; having no '#' at all cannot be got wrong.
HAS_SDL2_HEADERS := $(shell echo | $(CXX) $(SDL2_CFLAGS) -include SDL.h -fsyntax-only -xc++ - >/dev/null 2>&1 && echo 1)
HAS_OPENSSL := $(shell $(PKG_CONFIG) --exists openssl && echo 1)
# zstd stream compression (RP_CAP_COMPRESS_ZSTD) is in every build: a browser
# frame (a raw full-view bitmap) crosses the wire 40-310x smaller with it
# (DeBeOS-RDP #23). The decoder is the system libzstd when pkg-config finds one,
# else the vendored single-file decoder in third_party/zstd (zstd 1.5.7, see its
# README). HAIKU_REMOTE_ZSTD=system|vendored forces a source; =none builds the
# no-decoder client, which never offers the bit -- kept only so the tests of that
# refusal path still have a build to run in.
HAIKU_REMOTE_ZSTD ?= auto
HAS_SYSTEM_ZSTD := $(shell $(PKG_CONFIG) --exists libzstd && echo 1)
ifeq ($(HAIKU_REMOTE_ZSTD),auto)
ZSTD_SOURCE := $(if $(HAS_SYSTEM_ZSTD),system,vendored)
else
ZSTD_SOURCE := $(HAIKU_REMOTE_ZSTD)
endif
ifeq ($(filter $(ZSTD_SOURCE),system vendored none),)
$(error HAIKU_REMOTE_ZSTD must be auto, system, vendored or none, not '$(HAIKU_REMOTE_ZSTD)')
endif
ifeq ($(ZSTD_SOURCE)$(HAS_SYSTEM_ZSTD),system)
$(error HAIKU_REMOTE_ZSTD=system, but pkg-config finds no libzstd)
endif

# Loud, single-line skip notices. A silently-skipped target reads exactly like a
# passing one, so the absence of a frontend must be impossible to miss (#26).
GUI_SKIP_MSG := haiku-remote-gui: SKIPPED (SDL2 development files not found; install SDL2 to build the SDL frontend)
X11_SKIP_MSG := haiku-remote-x11: SKIPPED (X11 development files not found; install X11 to build the X11 frontend)

# "built N of 3 frontends" bookkeeping for the end-of-build summary.
FRONTENDS_BUILT := $(words haiku-remote $(if $(HAS_SDL2),gui) $(if $(HAS_X11),x11))
FRONTENDS_SKIPPED :=
ifneq ($(HAS_SDL2),1)
FRONTENDS_SKIPPED += haiku-remote-gui
endif
ifneq ($(HAS_X11),1)
FRONTENDS_SKIPPED += haiku-remote-x11
endif
ifeq ($(ZSTD_SOURCE),system)
# The system library also has the encoder, which the tests use to produce
# streams the way the server does; the vendored decoder does not.
CPPFLAGS += -DHAIKU_REMOTE_HAVE_ZSTD -DHAIKU_REMOTE_HAVE_ZSTD_ENCODER \
	$(shell $(PKG_CONFIG) --cflags libzstd 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs libzstd 2>/dev/null)
endif
ifeq ($(ZSTD_SOURCE),vendored)
CPPFLAGS += -DHAIKU_REMOTE_HAVE_ZSTD -Ithird_party/zstd
endif
ifeq ($(ZSTD_SOURCE),none)
# Marks the one build allowed to lack the decoder, so a test can tell a
# deliberate no-decoder build from one that lost zstd by accident.
CPPFLAGS += -DHAIKU_REMOTE_ZSTD_NONE
endif
ifeq ($(HAS_OPENSSL),1)
CPPFLAGS += -DHAIKU_REMOTE_HAVE_WSS $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)
endif

BUILD := build
CORE_SOURCES := \
	src/protocol.cpp \
	src/wire_decoder.cpp \
	src/input_encoder.cpp \
	src/surface.cpp \
	src/text_engine.cpp \
	src/session.cpp \
	src/tcp_socket.cpp \
	src/transport.cpp \
	src/known_brokers.cpp \
	src/launch_arguments.cpp \
	src/reconnect.cpp \
	src/png_writer.cpp \
	src/json.cpp \
	src/connection_profile.cpp \
	src/profile_store.cpp \
	src/profile_library.cpp \
	src/profile_launch.cpp \
	src/managed_transport.cpp \
	src/font_fetch.cpp \
	src/library_screen.cpp \
	src/connect_flow.cpp \
	src/connect_screen.cpp \
	src/desktop_shell.cpp
ifeq ($(HAS_OPENSSL),1)
CORE_SOURCES += src/websocket.cpp src/broker_handshake.cpp src/quic.cpp
endif
CORE_OBJECTS := $(CORE_SOURCES:src/%.cpp=$(BUILD)/%.o)
ifeq ($(ZSTD_SOURCE),vendored)
CORE_OBJECTS += $(BUILD)/zstddeclib.o
endif

ifeq ($(OS),Windows_NT)
LDLIBS += -lws2_32
endif

# Haiku keeps the sockets API (getaddrinfo/socket/connect/...) in libnetwork,
# not libc, so the final link needs it explicitly (see issue #45).
ifeq ($(shell uname -s),Haiku)
LDLIBS += -lnetwork
endif

.PHONY: gui-check all clean test e2e interactive syntax-check frontend-report

all: $(BUILD)/haiku-remote \
	$(if $(HAS_SDL2),$(BUILD)/haiku-remote-gui) \
	$(if $(HAS_X11),$(BUILD)/haiku-remote-x11) \
	frontend-report

# Report which frontends were built and which were skipped, so the log carries
# "built N of 3 frontends (... SKIPPED ...)" rather than silence. Runs after the
# binaries because make completes all prerequisites before a rule's recipe. The
# skip path for haiku-remote-gui falls through to syntax-check, which either
# parses src/sdl_main.cpp (headers present) or says it cannot (headers absent) —
# never a fabricated pass.
frontend-report: $(if $(HAS_SDL2),,syntax-check)
	@echo '=== client frontends ===' >&2
	@echo 'haiku-remote:     built (headless / PNG capture)' >&2
ifeq ($(HAS_SDL2),1)
	@echo 'haiku-remote-gui: built (SDL2 frontend)' >&2
else
	@echo '$(GUI_SKIP_MSG)' >&2
endif
ifeq ($(HAS_X11),1)
	@echo 'haiku-remote-x11: built (X11 frontend)' >&2
else
	@echo '$(X11_SKIP_MSG)' >&2
endif
	@echo 'zstd stream compression: $(if $(filter none,$(ZSTD_SOURCE)),SKIPPED (HAIKU_REMOTE_ZSTD=none; the plain stream is used),built with the $(ZSTD_SOURCE) decoder (RP_CAP_COMPRESS_ZSTD offered))' >&2
	@echo 'built $(FRONTENDS_BUILT) of 3 frontends$(if $(FRONTENDS_SKIPPED), (SKIPPED:$(FRONTENDS_SKIPPED)))' >&2

# Syntax-only check of the SDL frontend for hosts that cannot link it. When the
# SDL2 headers are present this actually invokes the compiler on sdl_main.cpp so
# a typo (e.g. a bad connect-failure edit) fails the build; when they are absent
# it says so plainly — strictly better than a silent skip (#26).
syntax-check:
ifeq ($(HAS_SDL2_HEADERS),1)
	@echo 'haiku-remote-gui: syntax-checking src/sdl_main.cpp (SDL2 headers present, link libs $(if $(HAS_SDL2),present,absent))...' >&2
	$(CXX) $(CPPFLAGS) $(SDL2_CFLAGS) $(CXXFLAGS) -fsyntax-only src/sdl_main.cpp
	@echo 'haiku-remote-gui: syntax-check PASSED' >&2
else
	@echo 'haiku-remote-gui: SKIPPED syntax-check (SDL2 headers absent, cannot even syntax-check src/sdl_main.cpp)' >&2
endif

# `test` also depends on `all`, not just the test binaries. A fix verified here
# is usually re-measured next with ./build/haiku-remote (and the frontends), so
# those binaries must not be stale -- otherwise green tests plus a measurement of
# the old code reads as "the fix didn't help" or, worse, as a false pass (#14).
# The cost is one relink of anything out of date per test run; `all` also keeps
# the loud conditional-frontend report (#26) intact.
test: all $(BUILD)/haiku-remote-tests $(BUILD)/haiku-remote-render-tests \
		$(BUILD)/haiku-remote-profile-tests $(BUILD)/haiku-remote-library-tests \
		$(BUILD)/haiku-remote-tunnel-tests $(BUILD)/haiku-remote-connect-tests \
		$(BUILD)/haiku-remote-xplatform-tests $(BUILD)/haiku-remote-trust-tests \
		$(BUILD)/haiku-remote-shell-tests $(BUILD)/haiku-remote-font-tests \
		$(BUILD)/haiku-remote-font-fetch-tests
	$(BUILD)/haiku-remote-tests
	$(BUILD)/haiku-remote-render-tests
	$(BUILD)/haiku-remote-font-tests
	$(BUILD)/haiku-remote-font-fetch-tests
	$(BUILD)/haiku-remote-profile-tests
	$(BUILD)/haiku-remote-library-tests
	$(BUILD)/haiku-remote-tunnel-tests
	$(BUILD)/haiku-remote-connect-tests
	$(BUILD)/haiku-remote-xplatform-tests
	$(BUILD)/haiku-remote-trust-tests
	$(BUILD)/haiku-remote-shell-tests
ifeq ($(HAS_SDL2),1)
	@$(MAKE) --no-print-directory gui-check
endif
	$(MAKE) --no-print-directory e2e

# The SDL library window, headless: caret, selection, clipboard, minimum size and
# editor scrolling, checked as exact pixels read back from the renderer. Exit 77
# from the script means this SDL has no offscreen driver (reported as SKIP).
gui-check: $(BUILD)/haiku-remote-gui
	@if command -v python3 >/dev/null 2>&1; then \
		python3 tools/library_gui_check.py $(BUILD)/haiku-remote-gui; \
		status=$$?; [ $$status -eq 0 ] || [ $$status -eq 77 ]; \
	else \
		echo 'SKIP: library GUI check needs python3'; \
	fi

# Issue #1 end to end: a saved profile against tools/rp_mock_server.py through
# progress, handshake, desktop and disconnect (and back to the library when the
# GUI is built), with tests/e2e/fake-ssh standing in for ssh. It needs python3
# and a POSIX exec of the #! fake ssh; without them it says SKIPPED, never a
# silent pass (#26).
#
# The QUIC route needs tools/quic_bridge_mock.cpp, a stand-in for the broker's
# QUIC front door built on OpenSSL >= 3.5's QUIC server API -- the same version
# test that compiles quic:// into the client (quic.hpp), so the bridge is built
# exactly when the client has QUIC, and --quic-bridge is how the harness learns
# that (never from the client's own output). Without it the harness runs the
# non-QUIC expectations and says the QUIC routes were not built.
PYTHON3 ?= python3
HAS_PYTHON3 := $(shell $(PYTHON3) -c 'print(1)' 2>/dev/null)
HAS_QUIC_BRIDGE := $(shell $(PKG_CONFIG) --atleast-version=3.5 openssl && echo 1)
e2e: $(BUILD)/haiku-remote $(if $(HAS_SDL2),$(BUILD)/haiku-remote-gui) \
		$(if $(HAS_QUIC_BRIDGE),$(BUILD)/quic_bridge_mock)
ifeq ($(OS)$(HAS_PYTHON3),1)
	$(PYTHON3) tests/e2e/profile_e2e.py --client $(BUILD)/haiku-remote \
		$(if $(HAS_SDL2),--gui $(BUILD)/haiku-remote-gui) \
		$(if $(HAS_QUIC_BRIDGE),--quic-bridge $(BUILD)/quic_bridge_mock)
else
	@echo 'haiku-remote-e2e: SKIPPED (needs python3 on a POSIX host)' >&2
endif

# The e2e harness's QUIC bridge (a test tool, not part of the client).
$(BUILD)/quic_bridge_mock: tools/quic_bridge_mock.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null) $< \
		$(shell $(PKG_CONFIG) --libs openssl 2>/dev/null) -pthread -o $@

$(BUILD)/haiku-remote: $(CORE_OBJECTS) $(BUILD)/main.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-tests: $(CORE_OBJECTS) $(BUILD)/tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-render-tests: $(CORE_OBJECTS) $(BUILD)/render_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-font-tests: $(CORE_OBJECTS) $(BUILD)/font_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-font-fetch-tests: $(CORE_OBJECTS) $(BUILD)/font_fetch_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-profile-tests: $(CORE_OBJECTS) $(BUILD)/profile_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-library-tests: $(CORE_OBJECTS) $(BUILD)/library_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-tunnel-tests: $(CORE_OBJECTS) $(BUILD)/tunnel_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-connect-tests: $(CORE_OBJECTS) $(BUILD)/connect_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-xplatform-tests: $(CORE_OBJECTS) $(BUILD)/xplatform_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

# The integration half runs a TLS server on a thread.
$(BUILD)/haiku-remote-trust-tests: $(CORE_OBJECTS) $(BUILD)/trust_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -pthread -o $@

$(BUILD)/haiku-remote-shell-tests: $(CORE_OBJECTS) $(BUILD)/shell_tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

interactive:
ifeq ($(HAS_SDL2),1)
	$(MAKE) $(BUILD)/haiku-remote-gui
else
ifeq ($(HAS_X11),1)
	$(MAKE) $(BUILD)/haiku-remote-x11
else
	@echo "SDL2 and X11 development files are not installed" >&2
	@exit 2
endif
endif

$(BUILD)/haiku-remote-gui: $(CORE_OBJECTS) $(BUILD)/sdl_main.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) $(SDL2_LIBS) -o $@

$(BUILD)/sdl_main.o: src/sdl_main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(SDL2_CFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/haiku-remote-x11: $(CORE_OBJECTS) $(BUILD)/x11_main.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) $(X11_LIBS) -o $@

$(BUILD)/x11_main.o: src/x11_main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(X11_CFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Third-party C, compiled as its upstream ships it: not held to this project's
# warning flags, and never edited (third_party/zstd/README.md).
$(BUILD)/zstddeclib.o: third_party/zstd/zstddeclib.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -c $< -o $@

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/tests.o: tests/tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/render_tests.o: tests/render_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/font_tests.o: tests/font_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/font_fetch_tests.o: tests/font_fetch_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/profile_tests.o: tests/profile_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/library_tests.o: tests/library_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/tunnel_tests.o: tests/tunnel_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/connect_tests.o: tests/connect_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/xplatform_tests.o: tests/xplatform_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/shell_tests.o: tests/shell_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/trust_tests.o: tests/trust_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread $(DEPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
