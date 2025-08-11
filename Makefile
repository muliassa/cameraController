# Compiler and tools
NVCC = /usr/local/cuda/bin/nvcc
CXX = g++

# Check if NVCC exists
ifeq (,$(wildcard $(NVCC)))
    $(error NVCC not found at $(NVCC). Please check your CUDA installation)
endif

# Directories
BUILD_DIR = build
SDK_DIR = Video_Codec_SDK
CUDA_DIR = /usr/local/cuda

# Check if SDK directory exists
ifeq (,$(wildcard $(SDK_DIR)/Samples))
    $(error NVIDIA Video Codec SDK not found at $(SDK_DIR). Please extract the SDK)
endif

# Source files - add only the needed SDK implementation files
SOURCES = controller.cpp 

TARGET = $(BUILD_DIR)/cameraController

# Include directories
INCLUDES = -Iincludes \
			-IVideo_Codec_SDK/Samples \
           -IVideo_Codec_SDK/Samples/Utils \
           -IVideo_Codec_SDK/Samples/NvCodec \
           -IVideo_Codec_SDK/Interface \
           -I/usr/local/cuda/include 

# Library directories
LIB_DIRS = -LVideo_Codec_SDK/Lib/linux/stubs/x86_64 \
           -L/usr/local/cuda/lib64 \
           -L/lib/x86_64-linux-gnu

# Libraries - CUDA 12.x NPP libraries with proper order
# LIBS = -lcuda -lcudart -lnvcuvid -lnvidia-encode -lnppig -lnppc \
#        -lavformat -lavcodec -lavutil

# Libraries - CUDA 12.x NPP libraries with explicit FFmpeg 6.1.1 paths
LIBS = -lcuda -lcudart -lnvcuvid -lnvidia-encode -lnppig -lnppc -lssl -lcrypto \
       -l:libavformat.so.60.16.100 \
       -l:libavcodec.so.60.31.102 \
       -l:libavutil.so.58.29.100 \
       -l:libswscale.so.7.5.100 \
       -l:libavfilter.so.9.12.100
# Compiler flags
CXXFLAGS = -std=c++17 -O2 -Wall
NVCCFLAGS = -std=c++17 -O2 --compiler-options -Wall

# Default target
all: $(TARGET)

# Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Check dependencies
check-deps:
	@echo "Checking dependencies..."
	@pkg-config --exists libavformat libavcodec libavutil || (echo "Error: FFmpeg development libraries not found. Run 'make install-deps'" && exit 1)
	@echo "Dependencies OK"

# Main target
$(TARGET): $(SOURCES) | $(BUILD_DIR) check-deps
	@echo "Building with NVCC..."
	@echo "NVCC: $(NVCC)"
	@echo "Sources: $(SOURCES)"
	@echo "Includes: $(INCLUDES)"
	@echo "Libraries: $(LIBS)"
	$(NVCC) $(NVCCFLAGS) $(INCLUDES) $(LIB_DIRS) -o $@ $(SOURCES) $(LIBS)

# Alternative using g++ (if you prefer)
$(TARGET)_gcc: $(SOURCES) | $(BUILD_DIR) check-deps
	@echo "Building with GCC..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LIB_DIRS) -o $@ $(SOURCES) $(LIBS)

# Debug build
debug: NVCCFLAGS += -g -G -DDEBUG
debug: $(TARGET)

# Verbose build (shows all commands)
verbose: NVCCFLAGS += -v
verbose: $(TARGET)

# Clean target
clean:
	rm -rf $(BUILD_DIR)

# Install dependencies (Ubuntu/Debian)
install-deps:
	sudo apt-get update
	sudo apt-get install -y libavformat-dev libavcodec-dev libavutil-dev pkg-config

# Test compile (just check syntax)
test-compile: $(SOURCES)
	$(NVCC) $(NVCCFLAGS) $(INCLUDES) -fsyntax-only $<

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build the video transcoder"
	@echo "  debug        - Build with debug symbols"
	@echo "  verbose      - Build with verbose output"
	@echo "  test-compile - Test compilation (syntax check only)"
	@echo "  check-deps   - Check if dependencies are installed"
	@echo "  clean        - Remove build directory"
	@echo "  install-deps - Install FFmpeg development libraries"
	@echo "  help         - Show this help message"

# Phony targets
.PHONY: all clean install-deps help check-deps debug verbose test-compile
