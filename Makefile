# Compiler and tools
CXX = g++

# Directories
BUILD_DIR = build

# Source files - add only the needed SDK implementation files
SOURCES = controller.cpp 

TARGET = $(BUILD_DIR)/cameraController

# Include directories
INCLUDES = -Iincludes 

# Library directories
LIB_DIRS = 

# Libraries - CUDA 12.x NPP libraries with proper order
# LIBS = -lcuda -lcudart -lnvcuvid -lnvidia-encode -lnppig -lnppc \
#        -lavformat -lavcodec -lavutil

# Libraries - CUDA 12.x NPP libraries with explicit FFmpeg 6.1.1 paths
LIBS = 

# Compiler flags
CXXFLAGS = -std=c++17 -O2 -Wall

# Default target
all: $(TARGET)

# Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Alternative using g++ (if you prefer)
$(TARGET)_gcc: $(SOURCES) | $(BUILD_DIR) check-deps
	@echo "Building with GCC..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LIB_DIRS) -o $@ $(SOURCES) $(LIBS)

# Debug build
debug: $(TARGET)

# Verbose build (shows all commands)
verbose: $(TARGET)

# Clean target
clean:
	rm -rf $(BUILD_DIR)

# Phony targets
.PHONY: all clean install-deps help check-deps debug verbose test-compile
