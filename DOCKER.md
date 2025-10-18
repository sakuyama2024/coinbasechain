# CoinbaseChain Docker Guide

This guide explains how to build and run CoinbaseChain using Docker.

## Table of Contents

- [Quick Start](#quick-start)
- [Building Images](#building-images)
- [Running the Node](#running-the-node)
- [Using Docker Compose](#using-docker-compose)
- [Configuration](#configuration)
- [CLI Usage](#cli-usage)
- [Running Tests](#running-tests)
- [Data Persistence](#data-persistence)
- [Networking](#networking)
- [Advanced Usage](#advanced-usage)

## Quick Start

The fastest way to get started with CoinbaseChain using Docker:

```bash
# Build and start the node
docker-compose up -d

# Check the logs
docker-compose logs -f node

# Use the CLI to interact with the node
docker-compose exec node coinbasechain-cli getinfo

# Stop the node
docker-compose down
```

## Building Images

### Main Node Image

Build the production-ready node image:

```bash
docker build -t coinbasechain:latest .
```

This creates a multi-stage build with:
- **Build stage**: Compiles the project with all dependencies
- **Runtime stage**: Minimal image (~200MB) with only runtime requirements

### Test Image

Build the test suite image:

```bash
docker build -f Dockerfile.test -t coinbasechain:test .
```

## Running the Node

### Using Docker Run

**Mainnet node with default settings:**

```bash
docker run -d \
  --name coinbasechain-node \
  -p 9590:9590 \
  -v coinbasechain-data:/home/coinbasechain/.coinbasechain \
  coinbasechain:latest
```

**Testnet node:**

```bash
docker run -d \
  --name coinbasechain-testnet \
  -p 19333:19333 \
  -v coinbasechain-testnet-data:/home/coinbasechain/.coinbasechain \
  -e COINBASECHAIN_NETWORK=testnet \
  coinbasechain:latest
```

**Regtest node (for local development):**

```bash
docker run -d \
  --name coinbasechain-regtest \
  -v coinbasechain-regtest-data:/home/coinbasechain/.coinbasechain \
  -e COINBASECHAIN_NETWORK=regtest \
  -e COINBASECHAIN_LISTEN=0 \
  coinbasechain:latest
```

### Viewing Logs

```bash
# Follow logs
docker logs -f coinbasechain-node

# View last 100 lines
docker logs --tail 100 coinbasechain-node
```

## Using Docker Compose

Docker Compose provides the easiest way to manage CoinbaseChain containers.

### Available Services

| Service | Description | Profile |
|---------|-------------|---------|
| `node` | Mainnet node | default |
| `testnet` | Testnet node | `testnet` |
| `regtest` | Regression test network | `regtest` |
| `tests` | Test suite | `test` |

### Common Commands

**Start mainnet node:**

```bash
docker-compose up -d
```

**Start testnet node:**

```bash
docker-compose --profile testnet up -d testnet
```

**Start regtest node:**

```bash
docker-compose --profile regtest up -d regtest
```

**Run tests:**

```bash
docker-compose --profile test run --rm tests
```

**View logs:**

```bash
docker-compose logs -f node
```

**Stop all services:**

```bash
docker-compose down
```

**Remove volumes (WARNING: deletes blockchain data):**

```bash
docker-compose down -v
```

## Configuration

### Environment Variables

Configure the node using environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `COINBASECHAIN_NETWORK` | `mainnet` | Network type: `mainnet`, `testnet`, or `regtest` |
| `COINBASECHAIN_THREADS` | `4` | Number of I/O threads |
| `COINBASECHAIN_PORT` | `9590` (mainnet), `19333` (testnet), `29333` (regtest) | P2P listening port |
| `COINBASECHAIN_LISTEN` | `1` | Enable incoming connections (1=yes, 0=no) |
| `COINBASECHAIN_VERBOSE` | `0` | Enable verbose logging (1=yes, 0=no) |

**Example using environment variables:**

```bash
docker run -d \
  --name coinbasechain-node \
  -p 9590:9590 \
  -e COINBASECHAIN_THREADS=8 \
  -e COINBASECHAIN_VERBOSE=1 \
  -v coinbasechain-data:/home/coinbasechain/.coinbasechain \
  coinbasechain:latest
```

**Example in docker-compose.yml:**

```yaml
services:
  node:
    image: coinbasechain:latest
    environment:
      - COINBASECHAIN_THREADS=8
      - COINBASECHAIN_VERBOSE=1
    volumes:
      - coinbasechain-data:/home/coinbasechain/.coinbasechain
```

### Command-Line Arguments

You can also pass arguments directly to the node:

```bash
docker run -d \
  --name coinbasechain-node \
  -p 9590:9590 \
  -v coinbasechain-data:/home/coinbasechain/.coinbasechain \
  coinbasechain:latest \
  --verbose --threads=8
```

## CLI Usage

### Accessing the CLI

**From within the container:**

```bash
docker-compose exec node coinbasechain-cli getinfo
```

**Or with docker run:**

```bash
docker exec coinbasechain-node coinbasechain-cli getinfo
```

### Available RPC Commands

| Command | Description |
|---------|-------------|
| `getinfo` | Get general node info |
| `getblockchaininfo` | Get blockchain state |
| `getblockcount` | Get current block height |
| `getblockhash <height>` | Get block hash at height |
| `getblockheader <hash>` | Get block header by hash |
| `getbestblockhash` | Get tip block hash |
| `getdifficulty` | Get PoW difficulty |
| `getmininginfo` | Get mining information |
| `getpeerinfo` | Get connected peer info |
| `stop` | Stop the node |

### Examples

```bash
# Get blockchain info
docker-compose exec node coinbasechain-cli getblockchaininfo

# Get peer info
docker-compose exec node coinbasechain-cli getpeerinfo

# Get block count
docker-compose exec node coinbasechain-cli getblockcount

# Stop the node gracefully
docker-compose exec node coinbasechain-cli stop
```

## Running Tests

### Full Test Suite

```bash
docker-compose --profile test run --rm tests
```

### Run Specific Tests

```bash
# Run specific test suite
docker-compose --profile test run --rm tests "block header"

# Run with verbose output
docker-compose --profile test run --rm tests -v

# Run tests with specific tags
docker-compose --profile test run --rm tests "[security]"
```

### Build and Test in One Command

```bash
docker build -f Dockerfile.test -t coinbasechain:test . && \
docker run --rm coinbasechain:test
```

## Data Persistence

### Volume Locations

CoinbaseChain stores data in `/home/coinbasechain/.coinbasechain`:

- `debug.log` - Application logs
- `node.sock` - Unix socket for RPC
- `block_index.json` - Persisted block metadata
- `addr_manager.json` - Persisted peer addresses

### Backup Data

**Export volume to tar:**

```bash
docker run --rm \
  -v coinbasechain-data:/data \
  -v $(pwd):/backup \
  ubuntu tar czf /backup/coinbasechain-backup.tar.gz -C /data .
```

**Restore from tar:**

```bash
docker run --rm \
  -v coinbasechain-data:/data \
  -v $(pwd):/backup \
  ubuntu tar xzf /backup/coinbasechain-backup.tar.gz -C /data
```

### Inspect Volume

```bash
docker run --rm \
  -v coinbasechain-data:/data \
  ubuntu ls -la /data
```

## Networking

### Port Mappings

| Port | Protocol | Purpose |
|------|----------|---------|
| 9590 | TCP | Mainnet P2P |
| 19333 | TCP | Testnet P2P |
| 29333 | TCP | Regtest P2P |

### Host Network Mode

For better P2P performance, you can use host networking:

```bash
docker run -d \
  --name coinbasechain-node \
  --network host \
  -v coinbasechain-data:/home/coinbasechain/.coinbasechain \
  coinbasechain:latest
```

**Note:** Host networking is only available on Linux.

### Custom Network

Create a custom bridge network for multiple nodes:

```bash
# Create network
docker network create coinbasechain-net

# Run multiple nodes
docker run -d \
  --name node1 \
  --network coinbasechain-net \
  -p 9590:9590 \
  coinbasechain:latest

docker run -d \
  --name node2 \
  --network coinbasechain-net \
  -p 9591:9590 \
  coinbasechain:latest
```

## Advanced Usage

### Resource Limits

**Limit CPU and memory:**

```bash
docker run -d \
  --name coinbasechain-node \
  --cpus="4.0" \
  --memory="4g" \
  -p 9590:9590 \
  -v coinbasechain-data:/home/coinbasechain/.coinbasechain \
  coinbasechain:latest
```

**In docker-compose.yml:**

```yaml
services:
  node:
    image: coinbasechain:latest
    deploy:
      resources:
        limits:
          cpus: '4.0'
          memory: 4G
        reservations:
          cpus: '2.0'
          memory: 2G
```

### Health Checks

Health checks are configured automatically in docker-compose. To check health status:

```bash
docker-compose ps
```

Or with docker run:

```bash
docker run -d \
  --name coinbasechain-node \
  --health-cmd="coinbasechain-cli getinfo" \
  --health-interval=30s \
  --health-timeout=10s \
  --health-retries=3 \
  --health-start-period=60s \
  -p 9590:9590 \
  coinbasechain:latest
```

### Development Setup

**Mount source code for development:**

```bash
docker run -it --rm \
  -v $(pwd):/workspace \
  -w /workspace \
  ubuntu:22.04 \
  bash
```

### Building with Different Compilers

**Build with Clang:**

```bash
docker build \
  --build-arg CMAKE_CXX_COMPILER=clang++ \
  -t coinbasechain:clang \
  .
```

### Multi-Platform Builds

Build for multiple architectures:

```bash
docker buildx build \
  --platform linux/amd64,linux/arm64 \
  -t coinbasechain:latest \
  --push \
  .
```

## Troubleshooting

### Container Won't Start

1. Check logs: `docker-compose logs node`
2. Verify ports are available: `netstat -tuln | grep 9590`
3. Check disk space: `docker system df`

### Performance Issues

1. Increase allocated resources (CPU/memory)
2. Use host networking for better P2P performance
3. Adjust thread count: `-e COINBASECHAIN_THREADS=8`

### Data Corruption

1. Stop the container: `docker-compose down`
2. Remove the volume: `docker volume rm coinbasechain-mainnet-data`
3. Start fresh: `docker-compose up -d`

### Build Failures

1. Clear Docker cache: `docker builder prune -a`
2. Rebuild without cache: `docker-compose build --no-cache`
3. Check available disk space: `docker system df`

## Security Considerations

1. **Non-root user**: Containers run as non-root user `coinbasechain` (UID 1000)
2. **Read-only filesystem**: Consider using `--read-only` flag for runtime
3. **Network isolation**: Use custom networks to isolate containers
4. **Volume permissions**: Ensure proper permissions on mounted volumes
5. **Resource limits**: Always set CPU and memory limits in production
6. **Regular updates**: Rebuild images regularly to get security updates

## Next Steps

- Read the main [README.md](README.md) for project overview
- Review [README_SECURITY.md](README_SECURITY.md) for security details
- Check [CI_CD_STRATEGY.md](CI_CD_STRATEGY.md) for CI/CD information
- Explore [CODEBASE_STRUCTURE.md](CODEBASE_STRUCTURE.md) for architecture details

## Support

For issues or questions:
- Open an issue on GitHub
- Check existing documentation in the repository
- Review Docker logs for diagnostic information
