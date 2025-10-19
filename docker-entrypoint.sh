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
LOGLEVEL="${COINBASECHAIN_LOGLEVEL:-}"
DEBUG="${COINBASECHAIN_DEBUG:-}"

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
    PORT="${COINBASECHAIN_PORT:-19590}"
    ;;
  regtest)
    ARGS+=("--regtest")
    PORT="${COINBASECHAIN_PORT:-29590}"
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

# Log level configuration (trace, debug, info, warn, error, critical)
# Examples: COINBASECHAIN_LOGLEVEL=trace or COINBASECHAIN_LOGLEVEL=debug
if [ -n "$LOGLEVEL" ]; then
  ARGS+=("--loglevel=$LOGLEVEL")
fi

# Component-specific debug logging
# Examples: COINBASECHAIN_DEBUG=chain or COINBASECHAIN_DEBUG=chain,network
# Special: COINBASECHAIN_DEBUG=all enables TRACE for all components
if [ -n "$DEBUG" ]; then
  ARGS+=("--debug=$DEBUG")
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
if [ -n "$LOGLEVEL" ]; then
  echo "Log Level:  $LOGLEVEL"
fi
if [ -n "$DEBUG" ]; then
  echo "Debug:      $DEBUG"
fi
echo "========================================="
echo "Command: coinbasechain ${ARGS[*]}"
echo "========================================="

# Execute the node
exec coinbasechain "${ARGS[@]}"
