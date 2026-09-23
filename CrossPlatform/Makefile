CXX ?= g++
PKG_CONFIG ?= pkg-config

PACKAGES := libpng freetype2 harfbuzz
CPPFLAGS := -Iinclude $(shell $(PKG_CONFIG) --cflags $(PACKAGES) 2>/dev/null)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
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
HAS_OPENSSL := $(shell $(PKG_CONFIG) --exists openssl && echo 1)
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

.PHONY: all clean test interactive

all: $(BUILD)/haiku-remote \
	$(if $(HAS_SDL2),$(BUILD)/haiku-remote-gui) \
	$(if $(HAS_X11),$(BUILD)/haiku-remote-x11)

test: $(BUILD)/haiku-remote-tests $(BUILD)/haiku-remote-render-tests
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
