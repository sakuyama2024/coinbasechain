# OSS-Fuzz Dockerfile for CoinbaseChain
FROM gcr.io/oss-fuzz-base/base-builder

# Install dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    libboost-system-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone the project
RUN git clone --depth 1 https://github.com/unicitynetwork/coinbasechain.git $SRC/coinbasechain

# Set working directory
WORKDIR $SRC/coinbasechain

# Copy build script
COPY build.sh $SRC/
