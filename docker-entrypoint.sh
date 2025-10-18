#!/bin/bash
set -e

# CoinbaseChain Docker Entrypoint Script
# Provides flexible configuration and initialization

# Data directory
DATADIR="${DATADIR:-/home/coinbasechain/.coinbasechain}"

# Default settings (can be overridden via environment variables)
THREADS="${COINBASECHAIN_THREADS:-4}"
PORT="${COINBASECHAIN_PORT:-9590}"
LISTEN="${COINBASECHAIN_LISTEN:-1}"
SERVER="${COINBASECHAIN_SERVER:-0}"
VERBOSE="${COINBASECHAIN_VERBOSE:-0}"
NETWORK="${COINBASECHAIN_NETWORK:-mainnet}"

# Ensure data directory exists
mkdir -p "$DATADIR"

# Build command-line arguments
ARGS=()
ARGS+=("--datadir=$DATADIR")
ARGS+=("--threads=$THREADS")

# Network selection
case "$NETWORK" in
  testnet)
    ARGS+=("--testnet")
    PORT="${COINBASECHAIN_PORT:-19333}"
    ;;
  regtest)
    ARGS+=("--regtest")
    PORT="${COINBASECHAIN_PORT:-29333}"
    ;;
  mainnet|*)
    # Default is mainnet, no flag needed
    PORT="${COINBASECHAIN_PORT:-9590}"
    ;;
esac

# Port configuration
if [ -n "$PORT" ]; then
  ARGS+=("--port=$PORT")
fi

# Listen configuration
if [ "$LISTEN" = "1" ]; then
  ARGS+=("--listen")
else
  ARGS+=("--nolisten")
fi

# Note: RPC server is always enabled, no --server flag needed

# Verbose logging
if [ "$VERBOSE" = "1" ]; then
  ARGS+=("--verbose")
fi

# Add any additional arguments passed to the container
ARGS+=("$@")

# Display configuration
echo "========================================="
echo "CoinbaseChain Node Starting"
echo "========================================="
echo "Network:    $NETWORK"
echo "Data Dir:   $DATADIR"
echo "Port:       $PORT"
echo "Threads:    $THREADS"
echo "Listen:     $LISTEN"
echo "Server:     $SERVER"
echo "Verbose:    $VERBOSE"
echo "========================================="
echo "Command: coinbasechain ${ARGS[*]}"
echo "========================================="

# Execute the node
exec coinbasechain "${ARGS[@]}"
