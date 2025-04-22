#CXX = g++
CXX = clang++

BUILD_DIR = build
SRC_DIR = src
EXT_DIR = ext
RES_DIR = res
BIN_MAIN = $(BUILD_DIR)/sabrewing

IMGUI_DIR = $(EXT_DIR)/imgui
IMPLOT_DIR = $(EXT_DIR)/implot
FONTS_DIR = $(RES_DIR)/fonts

INCLUDES = -I$(FONTS_DIR) -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(IMPLOT_DIR)

SOURCES_IMGUI = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_demo.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
SOURCES_IMGUI += $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
SOURCES_IMPLOT = $(IMPLOT_DIR)/implot.cpp $(IMPLOT_DIR)/implot_demo.cpp $(IMPLOT_DIR)/implot_items.cpp
SOURCES_EXT = $(SOURCES_IMGUI) $(SOURCES_IMPLOT)
SOURCES = $(SRC_DIR)/gui.cpp

OBJS_EXT = ${patsubst %.cpp, build/%.o, ${SOURCES_EXT}}
OBJS = ${patsubst %.cpp, build/%.o, ${SOURCES}}

UNAME_S := $(shell uname -s)
LINUX_GL_LIBS = -lGL

CXXFLAGS  = -std=c++11 -g -O0
CXXFLAGS += -Wall -Wextra -Wformat -Wno-missing-field-initializers -Wno-missing-braces
CXXFLAGS += -pthread
CXXFLAGS += $(INCLUDES) `sdl2-config --cflags`
CFLAGS = $(CXXFLAGS)


##---------------------------------------------------------------------
## OPENGL ES
##---------------------------------------------------------------------

## This assumes a GL ES library available in the system, e.g. libGLESv2.so
CXXFLAGS += -DIMGUI_IMPL_OPENGL_ES2
# LINUX_GL_LIBS = -lGLESv2
## If you're on a Raspberry Pi and want to use the legacy drivers,
## use the following instead:
# LINUX_GL_LIBS = -L/opt/vc/lib -lbrcmGLESv2

LIBS += $(LINUX_GL_LIBS) `sdl2-config --libs`


##---------------------------------------------------------------------
## BUILD RULES
##---------------------------------------------------------------------

$(BUILD_DIR)/%.o : %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BIN_MAIN): $(OBJS) $(OBJS_EXT)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

all: $(BIN_MAIN)
	@echo Build complete

clean:
	rm -f $(BIN_MAIN) $(OBJS) $(OBJS_EXT)
