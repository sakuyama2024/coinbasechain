# RPC Security Audit Report

**Date**: 2025-10-17
**Auditor**: Security Review
**Scope**: RPC subsystem (JSON-RPC server, client, command handling)
**Codebase**: CoinbaseChain RPC implementation

---

## Executive Summary

This security audit identified **10 critical and high-severity vulnerabilities** in the RPC implementation. The RPC system currently has **NO AUTHENTICATION**, **NO AUTHORIZATION**, **NO INPUT VALIDATION**, and **NO RATE LIMITING**. This makes it completely unsuitable for any production deployment or even local testing with untrusted users.

**Risk Level**: CRITICAL - Complete lack of security controls

**Immediate Action Required**:
1. **DO NOT DEPLOY** this RPC system to production
2. **DO NOT EXPOSE** the Unix socket to untrusted users
3. Implement authentication immediately
4. Add input validation and sanitization
5. Implement rate limiting and DoS protections

---

## Critical Vulnerabilities

### 1. NO AUTHENTICATION OR AUTHORIZATION
**Severity**: CRITICAL
**CWE**: CWE-306 (Missing Authentication for Critical Function)
**Location**: Entire RPC subsystem

#### Vulnerability Description

The RPC server has **absolutely no authentication mechanism**. Any process that can access the Unix domain socket can execute **any** RPC command, including:
- Stopping the node (`stop` command)
- Starting/stopping mining
- Adding/removing peer connections
- Setting mock time (manipulating consensus)
- Generating blocks (on regtest)
- Reading all blockchain data

#### Vulnerable Code

```cpp
void RPCServer::HandleClient(int client_fd)
{
    char buffer[4096];
    ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    // NO AUTHENTICATION CHECK
    // NO AUTHORIZATION CHECK
    // Directly processes any command from any client

    std::string response = ExecuteCommand(method, params);
    send(client_fd, response.c_str(), response.size(), 0);
}
```

#### Attack Scenarios

**Scenario 1: Local Privilege Escalation**
```bash
# Any user on the system can:
nc -U /tmp/coinbasechain.sock
{"method":"stop"}

# Node shuts down immediately
```

**Scenario 2: Malicious Mining Control**
```bash
# Attacker starts mining to exhaust CPU
echo '{"method":"startmining"}' | nc -U /tmp/coinbasechain.sock

# Or generates 1000 blocks to manipulate chain (regtest)
echo '{"method":"generate","params":["1000"]}' | nc -U /tmp/coinbasechain.sock
```

**Scenario 3: Network Manipulation**
```bash
# Disconnect all peers
for peer in $(getpeerinfo); do
    echo '{"method":"addnode","params":["'$peer'","remove"]}' | nc -U /tmp/coinbasechain.sock
done

# Add malicious peers
echo '{"method":"addnode","params":["attacker.com:8333","add"]}' | nc -U /tmp/coinbasechain.sock
```

#### Impact

- **Complete node compromise** by any local user
- **Denial of service** via `stop` command
- **Resource exhaustion** via mining commands
- **Network isolation** via peer manipulation
- **Consensus manipulation** via `setmocktime` and `generate`
- **Information disclosure** of all blockchain data

#### Recommended Fix

Implement multi-layer authentication:

```cpp
class RPCServer {
private:
    std::string auth_token_;  // Shared secret
    std::set<uid_t> allowed_uids_;  // Allowed user IDs

    bool AuthenticateClient(int client_fd) {
        // Layer 1: Unix credentials check
        struct ucred cred;
        socklen_t len = sizeof(cred);
        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
            if (allowed_uids_.find(cred.uid) == allowed_uids_.end()) {
                LOG_WARN("Rejected connection from unauthorized UID: {}", cred.uid);
                return false;
            }
        }

        // Layer 2: Token-based authentication
        char buffer[256];
        ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, MSG_PEEK);
        if (received <= 0) return false;

        buffer[received] = '\0';
        std::string request(buffer);

        // Extract auth token from request
        size_t auth_pos = request.find("\"auth\":\"");
        if (auth_pos == std::string::npos) {
            LOG_WARN("No auth token provided");
            return false;
        }

        auth_pos += 8;
        size_t auth_end = request.find("\"", auth_pos);
        if (auth_end == std::string::npos) return false;

        std::string provided_token = request.substr(auth_pos, auth_end - auth_pos);

        // Constant-time comparison
        if (!constant_time_compare(provided_token, auth_token_)) {
            LOG_WARN("Invalid auth token");
            std::this_thread::sleep_for(std::chrono::seconds(1));  // Rate limit failures
            return false;
        }

        return true;
    }

    void HandleClient(int client_fd) {
        if (!AuthenticateClient(client_fd)) {
            std::string error = "{\"error\":\"Authentication failed\"}\n";
            send(client_fd, error.c_str(), error.size(), 0);
            return;
        }

        // Continue with normal processing...
    }
};
```

**Additional Recommendations**:
1. Generate random auth token on startup, write to `~/.coinbasechain/rpc.cookie`
2. Restrict socket file permissions: `chmod 600 /path/to/socket`
3. Use `SO_PEERCRED` to validate Unix user ID
4. Implement command-level authorization (read-only vs admin commands)
5. Log all RPC attempts for audit trail

---

### 2. NO INPUT VALIDATION OR SANITIZATION
**Severity**: CRITICAL
**CWE**: CWE-20 (Improper Input Validation)
**Location**: Multiple handler functions

#### Vulnerability Description

RPC handlers directly use user input without validation, leading to multiple injection and crash vectors.

#### Vulnerable Code Examples

**Integer Parsing Without Bounds Checking**
```cpp
std::string RPCServer::HandleGetBlockHash(const std::vector<std::string>& params)
{
    if (params.empty()) {
        return "{\"error\":\"Missing height parameter\"}\n";
    }

    int height = std::stoi(params[0]);  // VULNERABLE: No range check, can throw
    auto* index = chainstate_manager_.GetBlockAtHeight(height);
    // ...
}
```

**Attack**: Send extremely large number:
```json
{"method":"getblockhash","params":["999999999999999999999"]}
```

Result: `std::stoi` throws `std::out_of_range`, crashes server thread

**Hash Parsing Without Validation**
```cpp
std::string RPCServer::HandleGetBlockHeader(const std::vector<std::string>& params)
{
    if (params.empty()) {
        return "{\"error\":\"Missing block hash parameter\"}\n";
    }

    uint256 hash;
    hash.SetHex(params[0]);  // VULNERABLE: No validation of hex format
    // ...
}
```

**Attack**: Send malformed hex:
```json
{"method":"getblockheader","params":["GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG"]}
```

Result: Possible undefined behavior in `SetHex`

**Port Parsing Without Validation**
```cpp
std::string RPCServer::HandleAddNode(const std::vector<std::string>& params)
{
    // ...
    std::string port_str = node_addr.substr(colon_pos + 1);
    uint16_t port = static_cast<uint16_t>(std::stoi(port_str));  // VULNERABLE
```

**Attack**: Send port > 65535:
```json
{"method":"addnode","params":["host:99999","add"]}
```

Result: Port wraps around due to cast, connects to wrong port

#### Impact

- **Denial of service** via crashes from uncaught exceptions
- **Undefined behavior** from malformed inputs
- **Port manipulation** from integer overflow
- **Resource exhaustion** from large allocations

#### Recommended Fix

```cpp
// Safe integer parsing with bounds
std::optional<int> SafeParseInt(const std::string& str, int min, int max) {
    try {
        size_t pos = 0;
        long value = std::stol(str, &pos);

        // Check entire string was consumed
        if (pos != str.size()) {
            return std::nullopt;
        }

        // Check bounds
        if (value < min || value > max) {
            return std::nullopt;
        }

        return static_cast<int>(value);
    } catch (...) {
        return std::nullopt;
    }
}

// Safe hex parsing
std::optional<uint256> SafeParseHash(const std::string& str) {
    // Check length
    if (str.size() != 64) {
        return std::nullopt;
    }

    // Check characters
    for (char c : str) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }

    uint256 hash;
    hash.SetHex(str);
    return hash;
}

// Updated handler
std::string RPCServer::HandleGetBlockHash(const std::vector<std::string>& params)
{
    if (params.empty()) {
        return "{\"error\":\"Missing height parameter\"}\n";
    }

    auto height_opt = SafeParseInt(params[0], 0, 10000000);
    if (!height_opt) {
        return "{\"error\":\"Invalid height (must be 0-10000000)\"}\n";
    }

    int height = *height_opt;
    auto* index = chainstate_manager_.GetBlockAtHeight(height);
    // ...
}
```

---

### 3. BUFFER OVERFLOW IN HandleClient
**Severity**: CRITICAL
**CWE**: CWE-120 (Buffer Copy without Checking Size of Input)
**Location**: `rpc_server.cpp:163-171`

#### Vulnerable Code

```cpp
void RPCServer::HandleClient(int client_fd)
{
    char buffer[4096];
    ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (received <= 0) {
        return;
    }

    buffer[received] = '\0';  // VULNERABLE: No bounds check on 'received'
    std::string request(buffer, received);
```

#### Vulnerability Description

If `recv()` returns 4096 bytes, then `buffer[received]` writes to `buffer[4096]`, which is out of bounds (array is 0-4095).

#### Attack

Send exactly 4096 bytes:
```bash
python3 -c "print('{'*4096)" | nc -U /tmp/coinbasechain.sock
```

Result: Stack buffer overflow, possible code execution

#### Impact

- **Stack corruption**
- **Possible arbitrary code execution**
- **Server crash**

#### Recommended Fix

```cpp
void RPCServer::HandleClient(int client_fd)
{
    char buffer[4096];
    ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (received <= 0) {
        return;
    }

    // Bounds check (received should be < sizeof(buffer))
    if (received >= sizeof(buffer)) {
        LOG_ERROR("RPC request too large: {} bytes", received);
        return;
    }

    buffer[received] = '\0';
    std::string request(buffer, received);
    // ...
}
```

Or better, use dynamic allocation:

```cpp
void RPCServer::HandleClient(int client_fd)
{
    std::vector<char> buffer(4096);
    ssize_t received = recv(client_fd, buffer.data(), buffer.size(), 0);

    if (received <= 0) {
        return;
    }

    std::string request(buffer.data(), received);
    // ...
}
```

---

### 4. JSON INJECTION VIA ERROR MESSAGES
**Severity**: HIGH
**CWE**: CWE-74 (Improper Neutralization of Special Elements in Output)
**Location**: `rpc_server.cpp:224-228`

#### Vulnerable Code

```cpp
std::string RPCServer::ExecuteCommand(const std::string& method,
                                     const std::vector<std::string>& params)
{
    // ...
    try {
        return it->second(params);
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "{\"error\":\"" << e.what() << "\"}\n";  // VULNERABLE
        return oss.str();
    }
}
```

#### Vulnerability Description

Exception messages are inserted directly into JSON responses without escaping. An attacker can craft inputs that trigger exceptions with malicious messages containing JSON metacharacters.

#### Attack Example

If an exception message contains: `Malformed input","admin":true,"error":"ignored`

Resulting JSON:
```json
{"error":"Malformed input","admin":true,"error":"ignored"}
```

This injects `"admin":true` into the response, potentially bypassing client-side checks.

#### Impact

- **JSON structure manipulation**
- **Client-side validation bypass**
- **Information leakage** through crafted error messages

#### Recommended Fix

```cpp
std::string EscapeJSONString(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

std::string RPCServer::ExecuteCommand(const std::string& method,
                                     const std::vector<std::string>& params)
{
    try {
        return it->second(params);
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "{\"error\":\"" << EscapeJSONString(e.what()) << "\"}\n";
        return oss.str();
    }
}
```

---

### 5. NO RATE LIMITING
**Severity**: HIGH
**CWE**: CWE-770 (Allocation of Resources Without Limits)
**Location**: `rpc_server.cpp:142-159`

#### Vulnerability Description

The RPC server accepts unlimited connections with no rate limiting, allowing trivial denial-of-service attacks.

#### Vulnerable Code

```cpp
void RPCServer::ServerThread()
{
    while (running_) {
        int client_fd = accept(server_fd_, ...);  // Unlimited accepts
        if (client_fd < 0) {
            continue;
        }

        HandleClient(client_fd);  // Synchronous, blocks server thread
        close(client_fd);
    }
}
```

#### Attack Scenarios

**Attack 1: Connection Spam**
```bash
# 10,000 rapid connections
for i in {1..10000}; do
    echo '{"method":"getinfo"}' | nc -U /tmp/coinbasechain.sock &
done
```

Result: Server thread blocked handling requests, new connections queued

**Attack 2: Slow Loris**
```python
import socket, time
socks = []
for i in range(1000):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect('/tmp/coinbasechain.sock')
    s.send(b'{')  # Send partial request
    socks.append(s)
    time.sleep(0.1)
# Keep sockets open indefinitely
time.sleep(999999)
```

Result: File descriptor exhaustion, no new connections possible

**Attack 3: Expensive Command Spam**
```bash
# Spam expensive generate command
while true; do
    echo '{"method":"generate","params":["1000"]}' | nc -U /tmp/coinbasechain.sock &
done
```

Result: CPU exhaustion from mining

#### Impact

- **Denial of service** via connection exhaustion
- **CPU exhaustion** via expensive commands
- **File descriptor exhaustion**
- **Memory exhaustion** from queued connections

#### Recommended Fix

```cpp
class RPCServer {
private:
    struct RateLimiter {
        std::map<uid_t, std::deque<std::chrono::steady_clock::time_point>> request_times;
        std::mutex mutex;

        bool AllowRequest(uid_t uid, size_t max_per_minute = 100) {
            std::lock_guard<std::mutex> lock(mutex);

            auto now = std::chrono::steady_clock::now();
            auto& times = request_times[uid];

            // Remove entries older than 1 minute
            auto cutoff = now - std::chrono::minutes(1);
            while (!times.empty() && times.front() < cutoff) {
                times.pop_front();
            }

            // Check limit
            if (times.size() >= max_per_minute) {
                return false;
            }

            times.push_back(now);
            return true;
        }
    };

    RateLimiter rate_limiter_;
    std::atomic<size_t> active_connections_{0};
    static constexpr size_t MAX_CONCURRENT_CONNECTIONS = 10;

    void ServerThread() {
        while (running_) {
            // Check connection limit
            if (active_connections_.load() >= MAX_CONCURRENT_CONNECTIONS) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            int client_fd = accept(server_fd_, ...);
            if (client_fd < 0) {
                continue;
            }

            // Get client credentials
            struct ucred cred;
            socklen_t len = sizeof(cred);
            if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
                if (!rate_limiter_.AllowRequest(cred.uid)) {
                    LOG_WARN("Rate limit exceeded for UID {}", cred.uid);
                    std::string error = "{\"error\":\"Rate limit exceeded\"}\n";
                    send(client_fd, error.c_str(), error.size(), 0);
                    close(client_fd);
                    continue;
                }
            }

            // Handle in separate thread (with connection limit)
            active_connections_++;
            std::thread([this, client_fd]() {
                HandleClient(client_fd);
                close(client_fd);
                active_connections_--;
            }).detach();
        }
    }
};
```

---

### 6. COMMAND INJECTION IN SETMOCKTIME
**Severity**: MEDIUM-HIGH
**CWE**: CWE-88 (Argument Injection)
**Location**: `rpc_server.cpp:761-787`

#### Vulnerable Code

```cpp
std::string RPCServer::HandleSetMockTime(const std::vector<std::string>& params)
{
    if (params.empty()) {
        return "{\"error\":\"Missing timestamp parameter\"}\n";
    }

    int64_t mock_time = std::stoll(params[0]);  // VULNERABLE: No bounds check
    util::SetMockTime(mock_time);
    // ...
}
```

#### Vulnerability Description

No validation on mock time value. Negative or extremely large values can cause:
- Integer overflow in time calculations
- Consensus failures
- Blockchain state corruption

#### Attack Example

```json
{"method":"setmocktime","params":["-9223372036854775808"]}
```

Setting time to `INT64_MIN` causes:
- Block timestamps appear in year 1677
- Difficulty calculations may overflow
- Median time calculation errors

#### Impact

- **Consensus manipulation**
- **Chain state corruption**
- **Integer overflow** in time-dependent logic
- **Block validation failures**

#### Recommended Fix

```cpp
std::string RPCServer::HandleSetMockTime(const std::vector<std::string>& params)
{
    if (params.empty()) {
        return "{\"error\":\"Missing timestamp parameter\"}\n";
    }

    int64_t mock_time;
    try {
        mock_time = std::stoll(params[0]);
    } catch (...) {
        return "{\"error\":\"Invalid timestamp format\"}\n";
    }

    // Validate reasonable range (year 1970 to 2106)
    if (mock_time < 0 || mock_time > 4294967295LL) {
        return "{\"error\":\"Timestamp out of range (must be 0-4294967295)\"}\n";
    }

    // Only allow on regtest/testnet
    if (params_.GetChainType() == chain::ChainType::MAIN) {
        return "{\"error\":\"setmocktime not allowed on mainnet\"}\n";
    }

    util::SetMockTime(mock_time);
    // ...
}
```

---

## High Severity Vulnerabilities

### 7. UNLIMITED GENERATE BLOCKS
**Severity**: HIGH
**CWE**: CWE-400 (Uncontrolled Resource Consumption)
**Location**: `rpc_server.cpp:673-747`

#### Vulnerable Code

```cpp
std::string RPCServer::HandleGenerate(const std::vector<std::string>& params)
{
    // ...
    int num_blocks = std::stoi(params[0]);
    if (num_blocks <= 0 || num_blocks > 1000) {
        return "{\"error\":\"Invalid number of blocks (must be 1-1000)\"}\n";
    }

    // Mine blocks one at a time
    for (int i = 0; i < num_blocks; i++) {
        miner_->Start(1);

        // Wait up to 60 seconds per block
        int wait_count = 0;
        while (current_height < expected_height && wait_count < 600) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }
        // ...
    }
}
```

#### Vulnerability Description

While there's a 1000 block limit, an attacker can call this repeatedly without rate limiting. Also, the function **blocks the RPC thread** for up to 60 seconds × 1000 blocks = **16.6 hours**.

#### Attack Scenario

```bash
# Call generate repeatedly
for i in {1..100}; do
    echo '{"method":"generate","params":["1000"]}' | nc -U /tmp/coinbasechain.sock &
done
```

Result:
- RPC server blocked for hours
- 100,000 blocks generated
- Massive CPU usage
- Chain state bloat

#### Impact

- **Long-term DoS** (blocks RPC for hours)
- **CPU exhaustion**
- **Blockchain bloat**
- **Consensus issues** from rapid block generation

#### Recommended Fix

```cpp
std::string RPCServer::HandleGenerate(const std::vector<std::string>& params)
{
    // Limit to 10 blocks per call for safety
    int num_blocks = std::stoi(params[0]);
    if (num_blocks <= 0 || num_blocks > 10) {
        return "{\"error\":\"Invalid number of blocks (must be 1-10)\"}\n";
    }

    // Only allow on regtest
    if (params_.GetChainType() != chain::ChainType::REGTEST) {
        return "{\"error\":\"generate only available on regtest\"}\n";
    }

    // Use async generation instead of blocking
    std::thread([this, num_blocks]() {
        for (int i = 0; i < num_blocks; i++) {
            // Generate block asynchronously
        }
    }).detach();

    return "{\"success\":true,\"message\":\"Block generation started\"}\n";
}
```

---

### 8. EXCEPTION LEAKS INTERNAL STATE
**Severity**: MEDIUM-HIGH
**CWE**: CWE-209 (Generation of Error Message Containing Sensitive Information)
**Location**: Multiple handlers

#### Vulnerable Code

```cpp
try {
    return it->second(params);
} catch (const std::exception& e) {
    std::ostringstream oss;
    oss << "{\"error\":\"" << e.what() << "\"}\n";  // Leaks internal details
    return oss.str();
}
```

#### Vulnerability Description

Exception messages may contain:
- File paths
- Memory addresses
- Internal implementation details
- Stack traces (if enabled)

#### Example Leak

```cpp
// If chainstate_manager throws:
throw std::runtime_error("Failed to load block index from /home/user/.coinbasechain/blocks/index");
```

Response:
```json
{"error":"Failed to load block index from /home/user/.coinbasechain/blocks/index"}
```

Leaks:
- Username
- Directory structure
- Internal paths

#### Impact

- **Information disclosure**
- **Path traversal** information
- **Fingerprinting** system configuration

#### Recommended Fix

```cpp
try {
    return it->second(params);
} catch (const std::exception& e) {
    // Log full error internally
    LOG_ERROR("RPC command '{}' failed: {}", method, e.what());

    // Return generic error to client
    return "{\"error\":\"Internal server error\"}\n";
}
```

---

### 9. RACE CONDITION IN STOP COMMAND
**Severity**: MEDIUM
**CWE**: CWE-362 (Concurrent Execution using Shared Resource with Improper Synchronization)
**Location**: `rpc_server.cpp:749-759`

#### Vulnerable Code

```cpp
std::string RPCServer::HandleStop(const std::vector<std::string>& params)
{
    LOG_INFO("Received stop command via RPC");

    // Trigger graceful shutdown via callback
    if (shutdown_callback_) {
        shutdown_callback_();  // Asynchronous shutdown starts
    }

    return "\"CoinbaseChain stopping\"\n";  // Returns immediately
}
```

#### Vulnerability Description

The shutdown is asynchronous, but the RPC server may try to handle more requests while shutting down, leading to:
- Use-after-free if components are destroyed
- Null pointer dereferences
- Inconsistent state

#### Attack Scenario

```bash
# Send stop + immediate follow-up
(echo '{"method":"stop"}'; echo '{"method":"getinfo"}') | nc -U /tmp/coinbasechain.sock
```

Result: Second command may execute during shutdown, accessing freed memory

#### Impact

- **Crashes** during shutdown
- **Use-after-free vulnerabilities**
- **Undefined behavior**

#### Recommended Fix

```cpp
std::string RPCServer::HandleStop(const std::vector<std::string>& params)
{
    LOG_INFO("Received stop command via RPC");

    // Set shutdown flag immediately
    shutting_down_.store(true, std::memory_order_release);

    // Trigger shutdown
    if (shutdown_callback_) {
        shutdown_callback_();
    }

    // Stop accepting new connections
    Stop();

    return "\"CoinbaseChain stopping\"\n";
}

void RPCServer::HandleClient(int client_fd) {
    // Check shutdown flag before processing
    if (shutting_down_.load(std::memory_order_acquire)) {
        std::string error = "{\"error\":\"Server shutting down\"}\n";
        send(client_fd, error.c_str(), error.size(), 0);
        return;
    }

    // Normal processing...
}
```

---

### 10. SIMPLISTIC JSON PARSING
**Severity**: MEDIUM
**CWE**: CWE-1176 (Inefficient CPU Computation)
**Location**: `rpc_server.cpp:173-205`

#### Vulnerable Code

```cpp
// Simple JSON parsing (method and params)
size_t method_pos = request.find("\"method\":\"");
if (method_pos != std::string::npos) {
    method_pos += 10;
    size_t method_end = request.find("\"", method_pos);
    // ...
}
```

#### Vulnerability Description

Hand-rolled JSON parsing with no validation:
- Doesn't handle nested objects
- No escape sequence handling
- Doesn't validate JSON structure
- Fragile to malformed input
- No handling of whitespace variations

#### Attack Examples

**Nested quotes break parsing**:
```json
{"method":"get\"info\""}
```

**Escaped quotes bypassed**:
```json
{"method":"\\\""}
```

**Malformed JSON accepted**:
```json
{method:"getinfo"}  // No quotes on key
```

#### Impact

- **Parser confusion** leading to wrong command execution
- **Potential bypasses** of command restrictions
- **Fragile** - breaks on valid JSON variations

#### Recommended Fix

Use a proper JSON library:

```cpp
#include <nlohmann/json.hpp>

void RPCServer::HandleClient(int client_fd)
{
    char buffer[4096];
    ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    // ...

    std::string request(buffer, received);

    try {
        // Use proper JSON parsing
        nlohmann::json j = nlohmann::json::parse(request);

        // Extract method
        std::string method = j.at("method").get<std::string>();

        // Extract params
        std::vector<std::string> params;
        if (j.contains("params") && j["params"].is_array()) {
            for (const auto& param : j["params"]) {
                params.push_back(param.get<std::string>());
            }
        }

        // Execute command
        std::string response = ExecuteCommand(method, params);
        send(client_fd, response.c_str(), response.size(), 0);

    } catch (const nlohmann::json::exception& e) {
        std::string error = "{\"error\":\"Invalid JSON\"}\n";
        send(client_fd, error.c_str(), error.size(), 0);
    }
}
```

---

## Medium Severity Issues

### 11. Unix Socket File Permission Issues
**Severity**: MEDIUM
**CWE**: CWE-732 (Incorrect Permission Assignment for Critical Resource)
**Location**: `rpc_server.cpp:82-103`

#### Issue

Socket file created with default permissions, potentially world-readable/writable depending on `umask`.

#### Recommended Fix

```cpp
bool RPCServer::Start()
{
    // Remove old socket
    unlink(socket_path_.c_str());

    // Set restrictive umask before creating socket
    mode_t old_umask = umask(0077);  // rw------- for socket file

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    // ... bind, listen ...

    // Restore umask
    umask(old_umask);

    // Explicitly set permissions (double-check)
    chmod(socket_path_.c_str(), 0600);  // Only owner can access

    // ...
}
```

---

### 12. No Timeout on recv()
**Severity**: MEDIUM
**CWE**: CWE-400 (Uncontrolled Resource Consumption)
**Location**: `rpc_server.cpp:164`

#### Issue

```cpp
ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
```

No timeout, client can hold connection open indefinitely.

#### Fix

```cpp
// Set receive timeout (e.g., 5 seconds)
struct timeval timeout;
timeout.tv_sec = 5;
timeout.tv_usec = 0;
setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
if (received < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
    LOG_WARN("RPC client timeout");
    return;
}
```

---

### 13. Information Disclosure in getpeerinfo
**Severity**: LOW-MEDIUM
**CWE**: CWE-200 (Exposure of Sensitive Information)
**Location**: `rpc_server.cpp:393-450`

#### Issue

`getpeerinfo` exposes detailed peer information including:
- IP addresses
- Connection times
- Misbehavior scores
- Internal peer IDs

This information could help attackers:
- Map network topology
- Identify which peers to attack
- Time eclipse attacks

#### Recommendation

Add privacy mode that masks sensitive fields:

```cpp
std::string HandleGetPeerInfo(const std::vector<std::string>& params) {
    bool verbose = false;
    if (!params.empty() && params[0] == "verbose") {
        verbose = true;
    }

    // In non-verbose mode, mask IP addresses
    if (!verbose) {
        oss << "    \"addr\": \"" << MaskIP(peer->address()) << ":" << peer->port() << "\",\n";
    } else {
        oss << "    \"addr\": \"" << peer->address() << ":" << peer->port() << "\",\n";
    }
}
```

---

## Security Best Practices Missing

### Authentication
- ❌ No authentication mechanism
- ❌ No authorization (all commands available to all clients)
- ❌ No audit logging of RPC access

### Input Validation
- ❌ No input sanitization
- ❌ No bounds checking on integers
- ❌ No hex/hash validation
- ❌ Fragile JSON parsing

### Rate Limiting
- ❌ No connection rate limiting
- ❌ No command rate limiting
- ❌ No per-user quotas

### Resource Protection
- ❌ No maximum request size
- ❌ No timeout on connections
- ❌ No concurrent connection limit
- ❌ Blocking operations (generate blocks)

### Error Handling
- ❌ Detailed errors leak internal state
- ❌ Exception messages not sanitized
- ❌ No generic error responses

### Network Security
- ❌ Socket permissions not set
- ❌ No IP-based restrictions (Unix socket only, but still)
- ❌ No TLS/encryption (not applicable for Unix sockets)

---

## Attack Surface Summary

| Vulnerability | Severity | Exploit Difficulty | Impact |
|---|---|---|---|
| No authentication | CRITICAL | Trivial | Complete compromise |
| No input validation | CRITICAL | Easy | Crashes, injection |
| Buffer overflow | CRITICAL | Medium | Code execution |
| No rate limiting | HIGH | Trivial | DoS |
| JSON injection | HIGH | Medium | Response manipulation |
| Unlimited generate | HIGH | Easy | Resource exhaustion |
| Exception leaks | MEDIUM-HIGH | Easy | Info disclosure |
| Command injection (setmocktime) | MEDIUM-HIGH | Easy | Consensus manipulation |
| Race in shutdown | MEDIUM | Medium | Crashes |
| Weak JSON parsing | MEDIUM | Medium | Parser confusion |

---

## Remediation Priority

### P0 - Critical (Block Deployment)
1. ✓ Implement authentication (cookie + Unix credentials)
2. ✓ Add input validation to all handlers
3. ✓ Fix buffer overflow in HandleClient
4. ✓ Set socket file permissions (chmod 600)

### P1 - High (Required for Production)
5. ✓ Implement rate limiting (per-UID)
6. ✓ Add concurrent connection limits
7. ✓ Fix JSON injection in error messages
8. ✓ Add bounds checks to all integer parsing
9. ✓ Implement proper JSON parsing (use library)

### P2 - Medium (Improve Security)
10. ✓ Add command-level authorization
11. ✓ Implement audit logging
12. ✓ Add timeouts to socket operations
13. ✓ Sanitize error messages
14. ✓ Fix race condition in stop command
15. ✓ Restrict setmocktime to regtest only

### P3 - Low (Hardening)
16. ✓ Add privacy mode to getpeerinfo
17. ✓ Implement command quotas
18. ✓ Add request size limits
19. ✓ Make generate async

---

## Secure RPC Configuration Guide

### Minimum Secure Configuration

```cpp
// 1. Authentication
std::string GenerateAuthToken() {
    // Generate 32-byte random token
    unsigned char token[32];
    RAND_bytes(token, 32);

    // Encode as hex
    std::string hex_token;
    for (int i = 0; i < 32; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", token[i]);
        hex_token += buf;
    }

    return hex_token;
}

// 2. Write to cookie file
void WriteRPCCookie(const std::string& datadir, const std::string& token) {
    std::string cookie_path = datadir + "/.cookie";
    std::ofstream file(cookie_path);
    if (file) {
        file << "__cookie__:" << token;
        file.close();
        chmod(cookie_path.c_str(), 0600);  // Owner read/write only
    }
}

// 3. Socket permissions
void SetSocketPermissions(const std::string& socket_path) {
    chmod(socket_path.c_str(), 0600);  // Only owner
}

// 4. Rate limiting
RateLimiter rate_limiter(100);  // 100 requests/minute per UID
```

### Production Checklist

- [ ] Authentication enabled (cookie file)
- [ ] Socket permissions set to 0600
- [ ] Rate limiting configured
- [ ] Input validation on all handlers
- [ ] Audit logging enabled
- [ ] Error messages sanitized
- [ ] Command authorization implemented
- [ ] Timeouts configured
- [ ] Connection limits set
- [ ] Run RPC server as non-root user

---

## Testing Recommendations

### Security Test Suite

```bash
# Test 1: Authentication bypass
echo '{"method":"stop"}' | nc -U /tmp/coinbasechain.sock
# Expected: Authentication error

# Test 2: Rate limiting
for i in {1..200}; do
    echo '{"method":"getinfo"}' | nc -U /tmp/coinbasechain.sock &
done
# Expected: Rate limit errors after 100 requests

# Test 3: Buffer overflow
python3 -c "print('{'*4096)" | nc -U /tmp/coinbasechain.sock
# Expected: Graceful error, no crash

# Test 4: Invalid JSON
echo 'not json' | nc -U /tmp/coinbasechain.sock
# Expected: JSON parse error

# Test 5: Integer overflow
echo '{"method":"getblockhash","params":["9999999999999999999"]}' | nc -U /tmp/coinbasechain.sock
# Expected: Out of range error

# Test 6: Malicious mock time
echo '{"method":"setmocktime","params":["-999999999"]}' | nc -U /tmp/coinbasechain.sock
# Expected: Rejected (mainnet) or range error
```

---

## Comparison with Bitcoin Core RPC

| Feature | CoinbaseChain | Bitcoin Core | Status |
|---------|---------------|--------------|--------|
| Authentication | ❌ None | ✅ Cookie file | MISSING |
| Authorization | ❌ None | ✅ Per-command | MISSING |
| Rate limiting | ❌ None | ✅ Yes | MISSING |
| Input validation | ❌ Minimal | ✅ Comprehensive | MISSING |
| Audit logging | ❌ No | ✅ Yes | MISSING |
| TLS support | N/A (Unix socket) | ✅ Optional | N/A |
| Timeout handling | ❌ No | ✅ Yes | MISSING |
| Error sanitization | ❌ No | ✅ Yes | MISSING |

---

## References

- [Bitcoin Core RPC Security](https://github.com/bitcoin/bitcoin/blob/master/doc/JSON-RPC-interface.md)
- [CWE-306: Missing Authentication](https://cwe.mitre.org/data/definitions/306.html)
- [OWASP API Security Top 10](https://owasp.org/www-project-api-security/)
- [Unix Socket Security Best Practices](https://man7.org/linux/man-pages/man7/unix.7.html)

---

## Conclusion

The RPC subsystem is currently **completely insecure** and must not be deployed in production or exposed to untrusted users. The lack of authentication alone makes it unsuitable for any multi-user environment.

**Estimated Remediation Effort**:
- Critical fixes (auth + validation): 5-7 days
- High priority fixes (rate limiting + JSON): 3-4 days
- Medium priority fixes: 3-5 days

**Total**: 11-16 days of focused security work

Until these issues are fixed, the RPC interface should only be used:
- In single-user environments
- With socket file permissions set to 0600
- On development/test systems only
- Never exposed to untrusted networks

---

**Audit Report Generated**: 2025-10-17
**Document Version**: 1.0
