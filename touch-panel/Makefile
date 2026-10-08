CXX ?= g++
CXXFLAGS ?= -O3 -Wall -Wextra -std=c++17
INCLUDES = `sdl2-config --cflags`
LIBS = `sdl2-config --libs` -lSDL2_ttf -lSDL2_image -lcurl -lpthread -ldl

TARGET = cdpanel
SRC = cdpanel.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
