# CoinbaseChain Ansible Deployment

Ansible automation for deploying CoinbaseChain Docker containers across multiple servers for regtest network testing.

## Overview

This Ansible setup deploys a 6-node CoinbaseChain regtest network across dedicated servers:

- **ct20** (178.18.251.16) - Node 0
- **ct21** (185.225.233.49) - Node 1
- **ct22** (207.244.248.15) - Node 2
- **ct23** (194.140.197.98) - Node 3
- **ct24** (173.212.251.205) - Node 4
- **ct25** (144.126.138.46) - Node 5

## Prerequisites

### Local Machine

1. **Install Ansible**:
   ```bash
   # macOS
   brew install ansible

   # Ubuntu/Debian
   sudo apt install ansible
   ```

2. **Install Ansible Docker module**:
   ```bash
   ansible-galaxy collection install community.docker
   ```

3. **SSH Access**:
   - Ensure you have SSH access to all servers
   - SSH keys should be configured (password-less authentication recommended)
   ```bash
   # Test SSH access
   ssh root@178.18.251.16
   ssh root@185.225.233.49
   # ... etc
   ```

### Remote Servers

The playbook will automatically install:
- Docker CE
- Docker CLI
- containerd.io

## Quick Start

### 1. Deploy the Network

```bash
cd ansible

# Deploy to all nodes (installs Docker, builds image, starts containers)
ansible-playbook -i inventory.yml deploy-coinbasechain.yml

# Deploy only (skip Docker installation if already installed)
ansible-playbook -i inventory.yml deploy-coinbasechain.yml --skip-tags install

# Deploy and connect peers
ansible-playbook -i inventory.yml deploy-coinbasechain.yml --tags all
```

### 2. Check Network Status

```bash
# Check sync status across all nodes
ansible-playbook -i inventory.yml check-sync.yml

# Quick connectivity test
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker exec coinbasechain-regtest coinbasechain-cli -regtest getconnectioncount"
```

### 3. Mine Blocks

```bash
# Mine 10 blocks on all nodes
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=10"

# Mine on specific node only
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=5" --limit ct20

# Mine different amounts on each node
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=1"
```

### 4. Verify Sync

After mining, check that all nodes sync to the same height:

```bash
ansible-playbook -i inventory.yml check-sync.yml --tags summary
```

Expected output:
```
========================================
NETWORK SYNC SUMMARY
========================================
Total Nodes: 6
Unique Heights: [101]
Nodes In Sync: YES
========================================
```

## Playbook Reference

### deploy-coinbasechain.yml

Main deployment playbook with the following stages:

1. **Install Docker** - Installs Docker CE on all nodes
2. **Build Image** - Transfers project files and builds Docker image
3. **Configure** - Creates data directories and stops old containers
4. **Deploy** - Starts containers with proper configuration
5. **Connect** - Establishes P2P connections between all nodes

**Tags**:
- `install` - Install Docker only
- `build` - Build Docker image only
- `config` - Configuration tasks only
- `deploy` - Deploy containers only
- `connect` - Connect peers only
- `info` - Display information only

**Examples**:
```bash
# Full deployment
ansible-playbook -i inventory.yml deploy-coinbasechain.yml

# Rebuild image and redeploy
ansible-playbook -i inventory.yml deploy-coinbasechain.yml --tags build,deploy

# Just reconnect peers
ansible-playbook -i inventory.yml deploy-coinbasechain.yml --tags connect
```

### mine-blocks.yml

Mines blocks on nodes for testing sync.

**Variables**:
- `blocks` - Number of blocks to mine (default: 1)

**Examples**:
```bash
# Mine 1 block on all nodes
ansible-playbook -i inventory.yml mine-blocks.yml

# Mine 100 blocks on all nodes
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=100"

# Mine only on ct20 and ct21
ansible-playbook -i inventory.yml mine-blocks.yml --limit ct20,ct21
```

### check-sync.yml

Checks blockchain sync status across all nodes.

**Output includes**:
- Block height for each node
- Peer connection count
- Container status
- Network-wide sync summary

**Examples**:
```bash
# Full sync check with summary
ansible-playbook -i inventory.yml check-sync.yml

# Quick info only
ansible-playbook -i inventory.yml check-sync.yml --tags info
```

### cleanup.yml

Stops containers, removes images, and optionally deletes data.

**Variables**:
- `remove_data` - Delete blockchain data (default: false)

**Examples**:
```bash
# Stop containers and remove images
ansible-playbook -i inventory.yml cleanup.yml

# Complete cleanup including blockchain data
ansible-playbook -i inventory.yml cleanup.yml -e "remove_data=true"

# Remove images only
ansible-playbook -i inventory.yml cleanup.yml --tags image
```

## Common Operations

### View Logs

```bash
# View logs from all nodes
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker logs coinbasechain-regtest --tail 50"

# View logs from specific node
ansible ct20 -i inventory.yml -m shell -a "docker logs coinbasechain-regtest --tail 100"

# Follow logs on specific node (via SSH)
ssh root@178.18.251.16
docker logs -f coinbasechain-regtest
```

### Execute CLI Commands

```bash
# Get blockchain info from all nodes
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker exec coinbasechain-regtest coinbasechain-cli -regtest getblockchaininfo"

# Get peer info
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker exec coinbasechain-regtest coinbasechain-cli -regtest getpeerinfo"

# Get network info
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker exec coinbasechain-regtest coinbasechain-cli -regtest getnetworkinfo"
```

### Restart Containers

```bash
# Restart all containers
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker restart coinbasechain-regtest"

# Restart specific node
ansible ct20 -i inventory.yml -m shell -a "docker restart coinbasechain-regtest"
```

### Update and Redeploy

```bash
# After code changes, rebuild and redeploy
ansible-playbook -i inventory.yml deploy-coinbasechain.yml --tags build,deploy,connect
```

## Testing Scenarios

### Scenario 1: Basic P2P Sync Test

```bash
# 1. Deploy network
ansible-playbook -i inventory.yml deploy-coinbasechain.yml

# 2. Mine blocks on ct20 only
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=10" --limit ct20

# 3. Wait for propagation
sleep 10

# 4. Check all nodes have same height
ansible-playbook -i inventory.yml check-sync.yml --tags summary
```

### Scenario 2: Multi-Node Mining Test

```bash
# 1. Deploy network
ansible-playbook -i inventory.yml deploy-coinbasechain.yml

# 2. Mine on different nodes
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=5" --limit ct20
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=3" --limit ct21
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=7" --limit ct22

# 3. Check sync (should converge to longest chain)
ansible-playbook -i inventory.yml check-sync.yml
```

### Scenario 3: Network Partition Test

```bash
# 1. Deploy network
ansible-playbook -i inventory.yml deploy-coinbasechain.yml

# 2. Partition network (stop 3 nodes)
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker stop coinbasechain-regtest" --limit ct23,ct24,ct25

# 3. Mine on one partition
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=10" --limit ct20

# 4. Mine on other partition (stopped nodes)
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker start coinbasechain-regtest" --limit ct23,ct24,ct25
ansible-playbook -i inventory.yml mine-blocks.yml -e "blocks=15" --limit ct23

# 5. Reconnect and observe reorg
ansible-playbook -i inventory.yml deploy-coinbasechain.yml --tags connect
sleep 20
ansible-playbook -i inventory.yml check-sync.yml
```

## Troubleshooting

### Connection Refused

If nodes can't connect to each other:

```bash
# Check firewall rules
ansible coinbasechain_nodes -i inventory.yml -m shell -a "ufw status"

# Open P2P port
ansible coinbasechain_nodes -i inventory.yml -m shell -a "ufw allow 29333/tcp"

# Check container port bindings
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker port coinbasechain-regtest"
```

### Container Not Starting

```bash
# Check container logs
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker logs coinbasechain-regtest --tail 100"

# Check Docker status
ansible coinbasechain_nodes -i inventory.yml -m shell -a "systemctl status docker"

# Restart Docker daemon
ansible coinbasechain_nodes -i inventory.yml -m shell -a "systemctl restart docker"
```

### Build Failures

```bash
# Check disk space
ansible coinbasechain_nodes -i inventory.yml -m shell -a "df -h"

# Clean up old images
ansible-playbook -i inventory.yml cleanup.yml --tags prune

# Rebuild from scratch
ansible-playbook -i inventory.yml cleanup.yml
ansible-playbook -i inventory.yml deploy-coinbasechain.yml
```

### Sync Issues

```bash
# Check peer connections
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker exec coinbasechain-regtest coinbasechain-cli -regtest getpeerinfo | grep addr"

# Manually reconnect peers
ansible-playbook -i inventory.yml deploy-coinbasechain.yml --tags connect

# Check for errors in logs
ansible coinbasechain_nodes -i inventory.yml -m shell -a "docker logs coinbasechain-regtest 2>&1 | grep -i error"
```

## File Structure

```
ansible/
├── ansible.cfg                 # Ansible configuration
├── inventory.yml               # Server inventory
├── deploy-coinbasechain.yml    # Main deployment playbook
├── mine-blocks.yml             # Mining playbook
├── check-sync.yml              # Sync status playbook
├── cleanup.yml                 # Cleanup playbook
└── README.md                   # This file
```

## Architecture

Each node runs:
- **Docker container** with CoinbaseChain
- **coinbasechain daemon** listening on port 29333 (P2P)
- **RPC interface** on port 29334
- **Persistent volume** at `/var/lib/coinbasechain`

Network topology:
- **Full mesh** - Each node connects to all other nodes
- **Regtest mode** - Low difficulty for fast testing
- Inbound P2P is enabled by default (set `COINBASECHAIN_LISTEN=0` to disable)

## Security Notes

1. **Root access**: Playbooks assume root SSH access
2. **Firewall**: Ensure ports 29333 (P2P) and 29334 (RPC) are accessible
3. **Data persistence**: Blockchain data stored in `/var/lib/coinbasechain`
4. **Container user**: Containers run as non-root user `coinbasechain` (UID 1000)

## Performance Tuning

### Performance tips

- Reduce log verbosity (omit --verbose unless debugging)
- Limit number of peers via firewall or container resource limits if needed

### Increase Connection Limits

For larger networks:

```bash
# Edit deploy-coinbasechain.yml
# Change COINBASECHAIN_MAXCONNECTIONS: "10" to higher value
```

## Support

For issues or questions:
- Check logs: `docker logs coinbasechain-regtest`
- Review network status: `ansible-playbook -i inventory.yml check-sync.yml`
- See main project documentation: `../DOCKER.md`
