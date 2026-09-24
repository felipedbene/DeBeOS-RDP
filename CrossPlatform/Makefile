CXX ?= g++
PKG_CONFIG ?= pkg-config

PACKAGES := libpng freetype2 harfbuzz
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
ifeq ($(HAS_OPENSSL),1)
CPPFLAGS += -DHAIKU_REMOTE_HAVE_WSS $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)
endif

BUILD := build
CORE_SOURCES := \
	src/protocol.cpp \
	src/input_encoder.cpp \
	src/surface.cpp \
	src/text_engine.cpp \
	src/session.cpp \
	src/tcp_socket.cpp \
	src/transport.cpp \
	src/reconnect.cpp \
	src/png_writer.cpp
ifeq ($(HAS_OPENSSL),1)
CORE_SOURCES += src/websocket.cpp
endif
CORE_OBJECTS := $(CORE_SOURCES:src/%.cpp=$(BUILD)/%.o)

ifeq ($(OS),Windows_NT)
LDLIBS += -lws2_32
endif

.PHONY: all clean test interactive syntax-check frontend-report

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
	@echo '=== CrossPlatform frontends ===' >&2
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
test: all $(BUILD)/haiku-remote-tests $(BUILD)/haiku-remote-render-tests
	$(BUILD)/haiku-remote-tests
	$(BUILD)/haiku-remote-render-tests

$(BUILD)/haiku-remote: $(CORE_OBJECTS) $(BUILD)/main.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-tests: $(CORE_OBJECTS) $(BUILD)/tests.o
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/haiku-remote-render-tests: $(CORE_OBJECTS) $(BUILD)/render_tests.o
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

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/tests.o: tests/tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/render_tests.o: tests/render_tests.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
