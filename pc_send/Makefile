CXX ?= g++
CXXFLAGS ?= -O3 -Wall -Wextra -std=c++17 -pthread
LIBS = -lpulse -lpulse-simple

TARGET = rpi_streamer_linux

all: $(TARGET)

$(TARGET): rpi_streamer_linux.cpp
	$(CXX) $(CXXFLAGS) rpi_streamer_linux.cpp -o $(TARGET) $(LIBS)

# Cross-compile for Windows using mingw-w64 if installed
windows: rpi_streamer_windows.cpp
	x86_64-w64-mingw32-g++ -O3 -std=c++17 rpi_streamer_windows.cpp -o rpi_streamer_windows.exe -lws2_32 -lole32 -lksuser

clean:
	rm -f $(TARGET) rpi_streamer_windows.exe

.PHONY: all windows clean
