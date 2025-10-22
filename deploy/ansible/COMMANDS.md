# CoinbaseChain Docker Commands Reference

Quick reference for managing deployed CoinbaseChain nodes.

## Server Access

```bash
# SSH into servers (using mykey)
ssh -i ~/.ssh/mykey root@178.18.251.16  # ct20
ssh -i ~/.ssh/mykey root@185.225.233.49 # ct21
ssh -i ~/.ssh/mykey root@207.244.248.15 # ct22
ssh -i ~/.ssh/mykey root@194.140.197.98 # ct23
ssh -i ~/.ssh/mykey root@173.212.251.205 # ct24
ssh -i ~/.ssh/mykey root@144.126.138.46 # ct25
```

## Container Management

### Check Container Status

```bash
# List running containers
docker ps

# List all containers (including stopped)
docker ps -a

# Filter for coinbasechain containers
docker ps -a | grep coinbasechain

# Get detailed container info
docker inspect coinbasechain-regtest
```

### Container Lifecycle

```bash
# Start container
docker start coinbasechain-regtest

# Stop container
docker stop coinbasechain-regtest

# Restart container
docker restart coinbasechain-regtest

# Remove container
docker rm coinbasechain-regtest

# Remove container forcefully
docker rm -f coinbasechain-regtest
```

### View Logs

```bash
# View all logs
docker logs coinbasechain-regtest

# Follow logs (live tail)
docker logs -f coinbasechain-regtest

# Last 50 lines
docker logs coinbasechain-regtest --tail 50

# Last 100 lines with timestamps
docker logs coinbasechain-regtest --tail 100 --timestamps

# View debug.log inside container
docker exec coinbasechain-regtest tail -f /home/coinbasechain/.coinbasechain/debug.log

# Last 100 lines of debug.log
docker exec coinbasechain-regtest tail -100 /home/coinbasechain/.coinbasechain/debug.log

# Search logs for specific text
docker logs coinbasechain-regtest 2>&1 | grep -i "error"
docker exec coinbasechain-regtest grep -i "VERSION" /home/coinbasechain/.coinbasechain/debug.log
```

## CLI Commands

### IMPORTANT: Correct CLI Syntax

The CLI **does NOT support `-regtest` flag**. It only accepts:
- `--datadir=<path>` - specify data directory
- `--help` - show help

The CLI connects to the daemon via Unix socket at `<datadir>/node.sock`.

### Basic Node Information

```bash
# Get general node info
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getinfo

# Get blockchain info
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getblockchaininfo

# Get block count
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getblockcount

# Get best block hash
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getbestblockhash

# Get difficulty
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getdifficulty
```

### Block Operations

```bash
# Get block hash at specific height
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getblockhash <height>

# Example: Get block hash at height 1
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getblockhash 1

# Get block header by hash
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getblockheader <hash>
```

### Mining

```bash
# Mine 1 block
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain generate 1

# Mine 10 blocks
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain generate 10

# Get mining info
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getmininginfo

# Get network hashrate
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getnetworkhashps
```

### Network & Peers

```bash
# Get peer information
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getpeerinfo

# Add peer connection
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode <ip>:<port> add

# Example: Connect to ct20 from another node
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode 178.18.251.16:29333 add

# Remove peer connection
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode <ip>:<port> remove

# Get network info (if implemented)
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getnetworkinfo
```

### Node Control

```bash
# Stop the node
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain stop

# Get help
docker exec coinbasechain-regtest coinbasechain-cli --help
```

## Ansible Remote Execution

### Single Command Execution

```bash
# Execute command on one server
ansible ct20 -i inventory.yml -m shell -a "docker ps"

# Execute on all servers
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker ps -a | grep coinbasechain"

# Get info from all nodes
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getinfo"

# Check peer count on all nodes
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getpeerinfo | grep addr"
```

### Deployment Commands

```bash
# Deploy to specific server
ansible-playbook -i inventory.yml deploy-simple.yml --limit ct20

# Deploy to multiple servers
ansible-playbook -i inventory.yml deploy-simple.yml --limit ct20,ct21,ct22

# Deploy to all servers
ansible-playbook -i inventory.yml deploy-simple.yml

# Deploy only (skip build)
ansible-playbook -i inventory.yml deploy-simple.yml --tags deploy

# Build only (skip deploy)
ansible-playbook -i inventory.yml deploy-simple.yml --tags build

# Build and deploy
ansible-playbook -i inventory.yml deploy-simple.yml --tags build,deploy
```

## Image Management

```bash
# List images
docker images

# Remove image
docker rmi coinbasechain:latest

# Remove image forcefully
docker rmi -f coinbasechain:latest

# Build image manually
docker build -t coinbasechain:latest /path/to/coinbasechain-docker

# Prune unused images
docker image prune

# Prune all unused Docker resources
docker system prune -a
```

## Data Directory Management

```bash
# View data directory contents
docker exec coinbasechain-regtest ls -la /home/coinbasechain/.coinbasechain

# Check data directory size
docker exec coinbasechain-regtest du -sh /home/coinbasechain/.coinbasechain

# View specific files
docker exec coinbasechain-regtest cat /home/coinbasechain/.coinbasechain/peers.json
docker exec coinbasechain-regtest cat /home/coinbasechain/.coinbasechain/banlist.json

# Check permissions
docker exec coinbasechain-regtest ls -la /home/coinbasechain/.coinbasechain/debug.log
```

## Debugging

### Check Container Health

```bash
# View container stats
docker stats coinbasechain-regtest

# Check container resource usage
docker exec coinbasechain-regtest ps aux

# Check running processes
docker top coinbasechain-regtest

# Inspect container configuration
docker inspect coinbasechain-regtest | grep -A10 "Env"
docker inspect coinbasechain-regtest | grep -A10 "Mounts"
```

### Network Debugging

```bash
# Check if port is listening
docker exec coinbasechain-regtest netstat -tlnp | grep 29333

# Test connectivity from container
docker exec coinbasechain-regtest ping -c 3 178.18.251.16

# Check firewall rules (on host)
ufw status

# Check if container can reach peer
docker exec coinbasechain-regtest nc -zv 178.18.251.16 29333
```

### Log Analysis

```bash
# Search for errors
docker logs coinbasechain-regtest 2>&1 | grep -i error

# Search for peer connections
docker exec coinbasechain-regtest grep -i "peer" /home/coinbasechain/.coinbasechain/debug.log

# Search for VERSION/VERACK messages
docker exec coinbasechain-regtest grep -E "VERSION|VERACK" /home/coinbasechain/.coinbasechain/debug.log

# Count peer connections over time
docker exec coinbasechain-regtest grep "Added peer" /home/coinbasechain/.coinbasechain/debug.log | wc -l

# Find handshake issues
docker exec coinbasechain-regtest grep -i "timeout\|disconnect" /home/coinbasechain/.coinbasechain/debug.log
```

## Multi-Node Operations

### Connect Nodes to Each Other

```bash
# From ct21, connect to ct20
ssh -i ~/.ssh/mykey root@185.225.233.49 "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode 178.18.251.16:29333 add"

# From ct22, connect to ct20 and ct21
ssh -i ~/.ssh/mykey root@207.244.248.15 "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode 178.18.251.16:29333 add"
ssh -i ~/.ssh/mykey root@207.244.248.15 "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode 185.225.233.49:29333 add"
```

### Check Sync Status Across Nodes

```bash
# Get block height from all nodes
for ip in 178.18.251.16 185.225.233.49 207.244.248.15 194.140.197.98 173.212.251.205 144.126.138.46; do
  echo "=== $ip ==="
  ssh -i ~/.ssh/mykey root@$ip "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getinfo | grep blocks"
done

# Check peer count on all nodes
for ip in 178.18.251.16 185.225.233.49 207.244.248.15; do
  echo "=== $ip ==="
  ssh -i ~/.ssh/mykey root@$ip "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getpeerinfo | grep -c addr || echo 0"
done
```

### Mine on Different Nodes

```bash
# Mine 5 blocks on ct20
ssh -i ~/.ssh/mykey root@178.18.251.16 "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain generate 5"

# Mine 3 blocks on ct21
ssh -i ~/.ssh/mykey root@185.225.233.49 "docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain generate 3"
```

## Common Workflows

### Complete Node Restart

```bash
# Stop container
docker stop coinbasechain-regtest

# Remove container
docker rm coinbasechain-regtest

# Start fresh container
docker run -d \
  --name coinbasechain-regtest \
  --restart unless-stopped \
  -p 29333:29333 \
  -p 29334:29334 \
  -v /var/lib/coinbasechain:/home/coinbasechain/.coinbasechain \
  -e COINBASECHAIN_NETWORK=regtest \
  -e COINBASECHAIN_LISTEN=1 \
  -e COINBASECHAIN_THREADS=2 \
  -e COINBASECHAIN_MAXCONNECTIONS=10 \
  coinbasechain:latest
```

### Reset Node Data

```bash
# Stop container
docker stop coinbasechain-regtest

# Remove container
docker rm coinbasechain-regtest

# Clear blockchain data (CAUTION!)
rm -rf /var/lib/coinbasechain/*

# Fix permissions
chown -R 1000:1000 /var/lib/coinbasechain

# Restart container
docker run -d --name coinbasechain-regtest ... (same as above)
```

### Update Code and Redeploy

```bash
# From local machine (in ansible directory)
ansible-playbook -i inventory.yml deploy-simple.yml --limit ct20 --tags build,deploy

# Or manually on server
ssh -i ~/.ssh/mykey root@178.18.251.16
docker stop coinbasechain-regtest
docker rm coinbasechain-regtest
# ... copy new code ...
docker build -t coinbasechain:latest /tmp/coinbasechain-build
docker run -d ... (start container)
```

## Troubleshooting

### Container Won't Start

```bash
# Check logs for errors
docker logs coinbasechain-regtest 2>&1 | tail -50

# Check if permissions are correct
ls -la /var/lib/coinbasechain

# Should be owned by UID 1000
chown -R 1000:1000 /var/lib/coinbasechain

# Check if ports are already in use
netstat -tlnp | grep 29333
```

### No Peer Connections

```bash
# Check if node is listening
docker exec coinbasechain-regtest netstat -tlnp | grep 29333

# Check firewall
ufw status
ufw allow 29333/tcp

# Check peer addresses
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getpeerinfo

# Check debug log for connection attempts
docker exec coinbasechain-regtest grep -i "connect\|peer" /home/coinbasechain/.coinbasechain/debug.log | tail -20
```

### Handshake Timeout

```bash
# Check for VERSION/VERACK in logs
docker exec coinbasechain-regtest grep -E "VERSION|VERACK|timeout" /home/coinbasechain/.coinbasechain/debug.log

# Check network connectivity
docker exec coinbasechain-regtest ping -c 3 <peer_ip>

# Check if firewall is blocking
telnet <peer_ip> 29333
```

### Blocks Not Syncing

```bash
# Check peer info
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getpeerinfo

# Check if peer is at higher height
# On peer node:
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getinfo

# Check for sync errors in logs
docker exec coinbasechain-regtest grep -i "sync\|header\|block" /home/coinbasechain/.coinbasechain/debug.log | tail -30
```

## Quick Copy-Paste Commands

### Full Node Status Check

```bash
echo "=== Container Status ===" && \
docker ps -a | grep coinbasechain && \
echo "=== Node Info ===" && \
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getinfo && \
echo "=== Peer Info ===" && \
docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain getpeerinfo && \
echo "=== Recent Logs ===" && \
docker logs coinbasechain-regtest --tail 20
```

### Connect to All Other Nodes (run on each node)

```bash
# On ct20 (connect to ct21-ct25)
for ip in 185.225.233.49 207.244.248.15 194.140.197.98 173.212.251.205 144.126.138.46; do
  docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode $ip:29333 add
done

# On ct21 (connect to ct20,ct22-ct25)
for ip in 178.18.251.16 207.244.248.15 194.140.197.98 173.212.251.205 144.126.138.46; do
  docker exec coinbasechain-regtest coinbasechain-cli --datadir=/home/coinbasechain/.coinbasechain addnode $ip:29333 add
done
```

## Environment Variables

The container accepts these environment variables:

```bash
COINBASECHAIN_NETWORK     # Network type: mainnet, testnet, regtest (default: mainnet)
COINBASECHAIN_THREADS     # Number of IO threads (default: 4)
COINBASECHAIN_PORT        # P2P port (default: network-specific)
COINBASECHAIN_LISTEN      # Enable listening: 0 or 1 (default: 1)
COINBASECHAIN_SERVER      # Not used (RPC always enabled)
COINBASECHAIN_VERBOSE     # Verbose logging: 0 or 1 (default: 0)
COINBASECHAIN_MAXCONNECTIONS  # Max peer connections (default: 10)
```

## Notes

- **Container name**: `coinbasechain-regtest` (for regtest network)
- **Data directory (host)**: `/var/lib/coinbasechain`
- **Data directory (container)**: `/home/coinbasechain/.coinbasechain`
- **Container user**: `coinbasechain` (UID 1000)
- **P2P ports**: 9590 (mainnet), 19333 (testnet), 29333 (regtest)
- **RPC socket**: `<datadir>/node.sock` (Unix socket, not TCP)
- **Debug log**: `<datadir>/debug.log`
