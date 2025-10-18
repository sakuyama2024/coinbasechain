#include "simulated_network.hpp"
#include "network_bridged_transport.hpp"
#include "util/time.hpp"
#include <algorithm>

namespace coinbasechain {
namespace test {

SimulatedNetwork::SimulatedNetwork(uint64_t seed)
    : rng_(seed),
      message_queue_([](const PendingMessage& a, const PendingMessage& b) {
          // Min-heap: smaller delivery_time has priority
          // If delivery times are equal, use sequence number to maintain FIFO order
          if (a.delivery_time_ms != b.delivery_time_ms) {
              return a.delivery_time_ms > b.delivery_time_ms;
          }
          return a.sequence_number > b.sequence_number;
      })
{
    // Initialize mock time to match simulated time (start at 1 second, not 0)
    // NOTE: SetMockTime(0) means "disable mocking", so we use 1 instead
    util::SetMockTime(1);
}

SimulatedNetwork::~SimulatedNetwork() {
    // Restore real time when simulation is destroyed
    // SetMockTime(0) disables time mocking
    util::SetMockTime(0);
}

void SimulatedNetwork::SetNetworkConditions(const NetworkConditions& conditions) {
    global_conditions_ = conditions;
}

void SimulatedNetwork::SetLinkConditions(int from_node, int to_node, const NetworkConditions& conditions) {
    link_conditions_[{from_node, to_node}] = conditions;
}

void SimulatedNetwork::SendMessage(int from_node, int to_node, const std::vector<uint8_t>& data) {
    printf("[DEBUG] SendMessage ENTRY: this=%p, from=%d, to=%d, size=%zu\n",
           (void*)this, from_node, to_node, data.size());

    stats_.total_messages_sent++;
    stats_.total_bytes_sent += data.size();
    stats_.messages_per_node[from_node]++;

    // Check network partition
    if (partition_.active && IsPartitioned(from_node, to_node)) {
        stats_.total_messages_dropped++;
        printf("[DEBUG] SendMessage: DROPPED by partition\n");
        return;  // Message blocked by partition
    }

    // Check packet loss
    if (ShouldDropMessage(from_node, to_node)) {
        stats_.total_messages_dropped++;
        printf("[DEBUG] SendMessage: DROPPED by packet loss\n");
        return;
    }

    // Calculate delivery time
    uint64_t delivery_time = CalculateDeliveryTime(from_node, to_node, data.size());

    // Enqueue message with sequence number for stable ordering
    PendingMessage msg;
    msg.from_node = from_node;
    msg.to_node = to_node;
    msg.data = data;
    msg.delivery_time_ms = delivery_time;
    msg.bytes = data.size();
    msg.sequence_number = message_sequence_++;  // Assign unique sequence for FIFO ordering

    printf("[DEBUG] SendMessage: from=%d, to=%d, delivery_time=%lu ms, seq=%lu, QUEUED\n",
           from_node, to_node, delivery_time, msg.sequence_number);

    message_queue_.push(std::move(msg));
}

void SimulatedNetwork::RegisterConnection(int from_node, int to_node) {
    active_connections_.insert({from_node, to_node});
    printf("[DEBUG] SimulatedNetwork::RegisterConnection: %d -> %d (total connections: %zu)\n",
           from_node, to_node, active_connections_.size());
}

void SimulatedNetwork::NotifyDisconnect(int from_node, int to_node) {
    printf("[DEBUG] SimulatedNetwork::NotifyDisconnect: node %d disconnecting from node %d\n",
           from_node, to_node);

    // Remove from active connections (both directions)
    active_connections_.erase({from_node, to_node});
    active_connections_.erase({to_node, from_node});

    // Purge all queued messages between these nodes
    // We need to rebuild the queue without the messages for this connection
    std::priority_queue<
        PendingMessage,
        std::vector<PendingMessage>,
        std::function<bool(const PendingMessage&, const PendingMessage&)>
    > new_queue([](const PendingMessage& a, const PendingMessage& b) {
        // Min-heap with sequence tiebreaker (same as constructor)
        if (a.delivery_time_ms != b.delivery_time_ms) {
            return a.delivery_time_ms > b.delivery_time_ms;
        }
        return a.sequence_number > b.sequence_number;
    });

    size_t purged_count = 0;
    while (!message_queue_.empty()) {
        const auto& msg = message_queue_.top();

        // Keep message if it's not between the disconnected nodes
        if (!((msg.from_node == from_node && msg.to_node == to_node) ||
              (msg.from_node == to_node && msg.to_node == from_node))) {
            new_queue.push(msg);
        } else {
            purged_count++;
            printf("[DEBUG] SimulatedNetwork::NotifyDisconnect: Purging queued message from %d to %d\n",
                   msg.from_node, msg.to_node);
        }

        message_queue_.pop();
    }

    message_queue_ = std::move(new_queue);
    printf("[DEBUG] SimulatedNetwork::NotifyDisconnect: Purged %zu queued messages\n", purged_count);

    // Find the transport for the target node and notify
    auto it = transports_.find(to_node);
    if (it != transports_.end() && it->second) {
        printf("[DEBUG] SimulatedNetwork::NotifyDisconnect: Found transport for node %d, calling handle_remote_disconnect\n", to_node);
        // Call handle_remote_disconnect on the remote transport
        it->second->handle_remote_disconnect(from_node);
    } else {
        printf("[DEBUG] SimulatedNetwork::NotifyDisconnect: No transport found for node %d\n", to_node);
    }
}

size_t SimulatedNetwork::ProcessMessages(uint64_t current_time_ms) {
    size_t delivered = 0;

    while (!message_queue_.empty() && message_queue_.top().delivery_time_ms <= current_time_ms) {
        // IMPORTANT: Copy the message BEFORE popping, because the callback may
        // trigger a disconnect which rebuilds the queue, invalidating references
        PendingMessage msg = message_queue_.top();
        message_queue_.pop();

        // Deliver message via node-specific callback (pass sender node_id)
        auto it = node_callbacks_.find(msg.to_node);
        if (it != node_callbacks_.end()) {
            it->second(msg.from_node, msg.data);
        }

        stats_.total_messages_delivered++;
        stats_.total_bytes_delivered += msg.bytes;
        delivered++;
    }

    return delivered;
}

size_t SimulatedNetwork::AdvanceTime(uint64_t new_time_ms) {
    if (new_time_ms < current_time_ms_) {
        return 0;  // Can't go backwards in time
    }

    current_time_ms_ = new_time_ms;

    // Synchronize util::GetTime() with simulated time
    // Convert milliseconds to seconds for the time mock
    // IMPORTANT: SetMockTime(0) means "disable mocking", so always use at least 1
    int64_t mock_time_seconds = std::max(int64_t(1), static_cast<int64_t>(current_time_ms_ / 1000));
    util::SetMockTime(mock_time_seconds);

    // Process messages and events in multiple rounds to handle message chains
    // (e.g., INV -> GETHEADERS -> HEADERS)
    // IMPORTANT: Keep looping as long as there are pending messages ready for delivery
    size_t total_delivered = 0;
    const int MAX_ROUNDS = 10;  // Prevent infinite loops

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        // Process any messages that are ready for delivery
        size_t delivered = ProcessMessages(current_time_ms_);
        total_delivered += delivered;

        // DEBUG: Log round activity
        if (delivered > 0) {
            printf("[DEBUG] AdvanceTime round %d: delivered %zu messages\n", round, delivered);
        }

        // Process io_context events on all nodes
        // This allows peers to react to received messages and send responses
        // IMPORTANT: This may queue new messages with delivery_time <= current_time_ms
        for (auto& [node_id, node] : nodes_) {
            if (node) {
                node->ProcessEvents();
            }
        }

        // Check if we should continue:
        // - If messages were delivered this round, continue (there may be responses)
        // - If queue is empty, we're done
        // - If next message is in the future, we're done
        bool has_ready_messages = !message_queue_.empty() &&
                                  message_queue_.top().delivery_time_ms <= current_time_ms_;

        if (delivered == 0 && !has_ready_messages) {
            break;  // No work done and no pending work
        }
    }

    return total_delivered;
}

void SimulatedNetwork::CreatePartition(const std::vector<int>& group_a, const std::vector<int>& group_b) {
    partition_.group_a = group_a;
    partition_.group_b = group_b;
    partition_.active = true;
}

void SimulatedNetwork::HealPartition() {
    printf("[DEBUG] SimulatedNetwork::HealPartition: partition healing, setting active=false\n");
    partition_.active = false;
    partition_.group_a.clear();
    partition_.group_b.clear();
    printf("[DEBUG] SimulatedNetwork::HealPartition: partition healed\n");
}

bool SimulatedNetwork::IsPartitioned(int node_a, int node_b) const {
    if (!partition_.active) {
        return false;
    }

    // Check if nodes are in different groups
    bool a_in_group_a = std::find(partition_.group_a.begin(), partition_.group_a.end(), node_a) != partition_.group_a.end();
    bool a_in_group_b = std::find(partition_.group_b.begin(), partition_.group_b.end(), node_a) != partition_.group_b.end();
    bool b_in_group_a = std::find(partition_.group_a.begin(), partition_.group_a.end(), node_b) != partition_.group_a.end();
    bool b_in_group_b = std::find(partition_.group_b.begin(), partition_.group_b.end(), node_b) != partition_.group_b.end();

    // Partitioned if one is in group_a and other is in group_b
    return (a_in_group_a && b_in_group_b) || (a_in_group_b && b_in_group_a);
}

void SimulatedNetwork::Reset() {
    current_time_ms_ = 0;
    while (!message_queue_.empty()) {
        message_queue_.pop();
    }
    link_conditions_.clear();
    partition_.active = false;
    partition_.group_a.clear();
    partition_.group_b.clear();
    stats_ = Stats{};
}

uint64_t SimulatedNetwork::CalculateDeliveryTime(int from_node, int to_node, size_t bytes) {
    const auto& conditions = GetLinkConditions(from_node, to_node);

    // Base latency (uniform random between min and max)
    std::uniform_int_distribution<uint64_t> latency_dist(
        conditions.latency_min.count(),
        conditions.latency_max.count()
    );
    uint64_t latency_ms = latency_dist(rng_);

    // Add jitter
    if (conditions.jitter_max.count() > 0) {
        std::uniform_int_distribution<uint64_t> jitter_dist(0, conditions.jitter_max.count());
        latency_ms += jitter_dist(rng_);
    }

    // Bandwidth delay (if limited)
    if (conditions.bandwidth_bytes_per_sec > 0) {
        uint64_t transmission_time_ms = (bytes * 1000) / conditions.bandwidth_bytes_per_sec;
        latency_ms += transmission_time_ms;
    }

    return current_time_ms_ + latency_ms;
}

bool SimulatedNetwork::ShouldDropMessage(int from_node, int to_node) {
    const auto& conditions = GetLinkConditions(from_node, to_node);

    if (conditions.packet_loss_rate <= 0.0) {
        return false;
    }

    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng_) < conditions.packet_loss_rate;
}

const SimulatedNetwork::NetworkConditions& SimulatedNetwork::GetLinkConditions(int from_node, int to_node) const {
    auto it = link_conditions_.find({from_node, to_node});
    if (it != link_conditions_.end()) {
        return it->second;
    }
    return global_conditions_;
}

} // namespace test
} // namespace coinbasechain
