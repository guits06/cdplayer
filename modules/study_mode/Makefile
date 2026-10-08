CXX ?= g++
CXXFLAGS ?= -O3 -fPIC -Wall -Wextra -std=c++17
INCLUDES = `sdl2-config --cflags` -I.
LIBS = `sdl2-config --libs` -lSDL2_ttf

TARGET = panel_plugin.so
SRC = panel_plugin.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -shared $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
