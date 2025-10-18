# Multi-stage Dockerfile for CoinbaseChain Node
# Build stage: Compile the project with all dependencies
# Runtime stage: Minimal image with only runtime requirements

# ============================================================================
# Build Stage
# ============================================================================
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libboost-system-dev \
    libssl-dev \
    pkg-config \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy project files
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY src/ ./src/
COPY test/ ./test/
COPY fuzz/ ./fuzz/
COPY tools/ ./tools/

# Configure CMake build
# Build in Release mode for optimized binaries
# Note: We still need test/ directory for CMake configuration
RUN cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=20 \
    -DENABLE_FUZZING=OFF \
    -DBUILD_TESTING=OFF

# Build the main node and CLI executables
# Use all available cores for faster compilation
RUN cmake --build build --target coinbasechain coinbasechain-cli -j$(nproc)

# ============================================================================
# Runtime Stage
# ============================================================================
FROM ubuntu:22.04

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libboost-system1.74.0 \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user for security
RUN useradd -m -u 1000 -s /bin/bash coinbasechain

# Copy built binaries from builder stage
COPY --from=builder /build/build/bin/coinbasechain /usr/local/bin/
COPY --from=builder /build/build/bin/coinbasechain-cli /usr/local/bin/

# Copy entrypoint script
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Set up data directory
RUN mkdir -p /home/coinbasechain/.coinbasechain && \
    chown -R coinbasechain:coinbasechain /home/coinbasechain

# Switch to non-root user
USER coinbasechain
WORKDIR /home/coinbasechain

# Expose P2P ports
EXPOSE 9590
EXPOSE 19333
EXPOSE 29333

# Create volume mount point for persistent data
VOLUME ["/home/coinbasechain/.coinbasechain"]

# Use custom entrypoint for flexible configuration
ENTRYPOINT ["docker-entrypoint.sh"]

# Default arguments can be overridden
CMD []
