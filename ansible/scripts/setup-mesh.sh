#!/bin/bash
# CoinbaseChain Mesh Network Setup Script
# Establishes full mesh connectivity between all nodes

set -e

# Configuration
SSH_KEY="${SSH_KEY:-~/.ssh/mykey}"
CONTAINER_NAME="${CONTAINER_NAME:-coinbasechain-regtest}"
P2P_PORT="${P2P_PORT:-29333}"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Node list with names
declare -A NODES=(
    ["ct20"]="178.18.251.16"
    ["ct21"]="185.225.233.49"
    ["ct22"]="207.244.248.15"
    ["ct23"]="194.140.197.98"
    ["ct24"]="173.212.251.205"
    ["ct25"]="144.126.138.46"
)

# Helper function to run remote commands
run_remote() {
    local node_ip=$1
    shift
    ssh -i "$SSH_KEY" -o ConnectTimeout=5 -o StrictHostKeyChecking=no root@"$node_ip" "$@" 2>/dev/null
}

# Add peer connection
add_peer() {
    local from_ip=$1
    local to_ip=$2
    local to_port=$3

    run_remote "$from_ip" "docker exec $CONTAINER_NAME coinbasechain-cli addnode ${to_ip}:${to_port} add" &>/dev/null
}

# Main setup
main() {
    echo "=========================================="
    echo " CoinbaseChain Mesh Network Setup"
    echo "=========================================="
    echo
    echo "Establishing connections between ${#NODES[@]} nodes..."
    echo

    local total_connections=0
    local expected_connections=$((${#NODES[@]} * (${#NODES[@]} - 1)))

    # Iterate through all nodes and connect each to all others
    for from_name in "${!NODES[@]}"; do
        from_ip="${NODES[$from_name]}"
        echo -e "${GREEN}Connecting $from_name ($from_ip) to other nodes...${NC}"

        for to_name in "${!NODES[@]}"; do
            if [[ "$from_name" != "$to_name" ]]; then
                to_ip="${NODES[$to_name]}"
                echo -n "  → $to_name ($to_ip): "

                if add_peer "$from_ip" "$to_ip" "$P2P_PORT"; then
                    echo -e "${GREEN}✓${NC}"
                    ((total_connections++))
                else
                    echo -e "${YELLOW}⚠ Failed${NC}"
                fi
            fi
        done
        echo
    done

    # Verify connections
    echo "Waiting for connections to establish..."
    sleep 5

    echo
    echo "Connection Summary:"
    echo "-------------------"
    for name in "${!NODES[@]}"; do
        ip="${NODES[$name]}"
        peer_count=$(run_remote "$ip" "docker exec $CONTAINER_NAME coinbasechain-cli getconnectioncount 2>&1" || echo "0")
        echo "  $name: $peer_count peers connected"
    done

    echo
    echo "=========================================="
    echo -e "${GREEN}✓ Mesh network setup complete!${NC}"
    echo "  Total connections initiated: $total_connections / $expected_connections"
    echo "=========================================="
}

# Check if running as root or with sudo
if [[ $EUID -ne 0 ]] && [[ ! -f "$SSH_KEY" ]]; then
   echo "This script requires SSH key access to remote nodes."
   echo "Please ensure SSH_KEY environment variable points to valid key."
   exit 1
fi

# Run main function
main "$@"