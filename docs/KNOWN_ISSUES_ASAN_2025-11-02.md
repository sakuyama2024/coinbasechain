# Crash reports and fixes (ASan/UBSan) — 2025-11-02

This note documents two reproducible crashes found under AddressSanitizer/UBSan while running the lightweight debug functional test.

Environment
- Build: AddressSanitizer + UBSan (CMake `-DSANITIZE=address`)
- Command:
  - Build: `cmake -DSANITIZE=address .. && make -j$(nproc)`
  - Run: `COINBASE_TEST_LOG_LEVEL=trace COINBASE_TEST_DEBUG=network,chain CBC_KEEP_TEST_DIR=1 CBC_TEST_INHERIT_STDIO=1 COINBASE_BIN_DIR="$(pwd)/bin" python3 ../test/functional/debug_sequential_mining.py`
- Test: `test/functional/debug_sequential_mining.py` (sequential mining; minimal memory pressure)

---

1) RealTransport send queue use-after-empty (deque pop_front on null)
- Symptom: SIGSEGV inside libc++ `std::__1::deque< std::vector<uint8_t> >::pop_front()` during async write completion.
- Representative stack:
  - `std::__1::deque<...>::pop_front()` (deque:2262)
  - `boost::asio::detail::write_op<...>::operator()`
  - `boost::asio::detail::reactive_socket_send_op<...>::do_complete(...)`
  - `boost::asio::io_context::run()`
  - `coinbasechain::network::RealTransport::run()`
- Localization:
  - `src/network/real_transport.cpp` — `RealTransportConnection::do_write()` completion path.
- Hypothesis:
  - Race between clearing the send queue and the write completion handler; or the buffer is not kept alive until the async operation completes (lifetime ends when popped early).
  - With a single IO thread we still need to guarantee buffer lifetime and queue integrity across callbacks.
- Fix plan:
  - Ensure ownership/lifetime of the buffer persists until the async send completes:
    - Keep the buffer in the queue until completion, or
    - Store it in a member `std::shared_ptr<std::vector<uint8_t>>` referenced by the completion handler.
  - Serialize all send operations on the io_context thread (post/dispatch) and guard the queue with a mutex or strand-like discipline.
  - In the completion handler, check the queue is non-empty before `pop_front()` and that the front matches the in-flight buffer.
  - Add ASan regression test path (run `debug_sequential_mining.py`) after fix.

---

2) Shutdown crash: BanMan logging after logger shutdown (null logger)
- Symptom: SIGSEGV in `spdlog::logger::trace(...)` from `BanMan::SaveInternal()` during destruction path; UBSan: member call on null pointer of type `spdlog::logger*`.
- Representative stack:
  - `spdlog::logger::trace(...)` (logger.h:146)
  - `coinbasechain::network::BanMan::SaveInternal()` (src/network/banman.cpp:174)
  - `coinbasechain::network::BanMan::Save()` / `~BanMan()`
  - `coinbasechain::network::NetworkManager::~NetworkManager()`
  - `coinbasechain::app::Application::~Application()`
- Hypothesis:
  - Logging subsystem (spdlog) is unavailable or default logger reset during/after component teardown; destructor logging calls into a null logger.
  - Receipt of SIGTERM (test harness) can interleave shutdown such that BanMan destructor runs after logging shutdown.
- Fix plan:
  - Do not log in destructors (Save/SaveInternal). Move all persistence to explicit save points during orderly shutdown.
  - In `Application::shutdown()`, explicitly call `ban_man().Save(...)` before tearing down components; ensure logger remains active until after persistence.
  - Add guard in BanMan: obtain logger via `util::LogManager::GetLogger("network")` and check for null before logging; or wrap logging via a helper that no-ops if logger is not available.
  - Verify with ASan rerun that the null-logger path no longer segfaults.

---

Non-bug observations
- Under ASan on macOS, multiple node processes can hit malloc warnings (nano zone) due to VM pressure; the debug test limits concurrency to mitigate this and still reproduces the above issues.

---

Action items checklist
- [ ] RealTransport: fix send queue buffer lifetime; add checks and serialize queue ops
- [ ] RealTransport: add unit/integration test to send bursts and ensure stability
- [ ] BanMan: remove logging from destructor; guard all logs or centralize persistence earlier
- [ ] Application: ensure banlist is saved prior to logger shutdown; maintain teardown order
- [ ] Re-run ASan `debug_sequential_mining.py` and `feature_chaos_convergence.py` to confirm

Appendix: Repro command
```bash
# From build-asan/
COINBASE_TEST_LOG_LEVEL=trace \
COINBASE_TEST_DEBUG=network,chain \
CBC_KEEP_TEST_DIR=1 CBC_TEST_INHERIT_STDIO=1 \
COINBASE_BIN_DIR="$(pwd)/bin" \
python3 ../test/functional/debug_sequential_mining.py
```
