#include "app/application.hpp"
#include "util/logging.hpp"
#include <cstring>
#include <iostream> // Keep for CLI output and early errors before logger initialized

void print_usage(const char *program_name) {
  std::cout
      << "Usage: " << program_name << " [options]\n"
      << "\n"
      << "Options:\n"
      << "  --datadir=<path>     Data directory (default: ~/.coinbasechain)\n"
      << "  --port=<port>        Listen port (default: 8333)\n"
      << "  --listen             Enable inbound connections\n"
      << "  --nolisten           Disable inbound connections (default)\n"
      << "  --threads=<n>        Number of IO threads (default: 4)\n"
      << "  --par=<n>            Number of parallel RandomX verification "
         "threads (default: 0 = auto)\n"
      << "  --suspiciousreorgdepth=<n>  Max reorg depth before halt (default: "
         "100, 0 = unlimited)\n"
      << "  --regtest            Use regression test chain (easy mining)\n"
      << "  --testnet            Use test network\n"
      << "  --verbose            Verbose logging\n"
      << "  --help               Show this help message\n"
      << std::endl;
}

int main(int argc, char *argv[]) {
  try {
    // Parse command line arguments
    coinbasechain::app::AppConfig config;
    std::string log_level = "info";

    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      if (arg == "--help") {
        print_usage(argv[0]);
        return 0;
      } else if (arg.find("--datadir=") == 0) {
        config.datadir = arg.substr(10);
      } else if (arg.find("--port=") == 0) {
        config.network_config.listen_port = std::stoi(arg.substr(7));
      } else if (arg == "--listen") {
        config.network_config.listen_enabled = true;
      } else if (arg == "--nolisten") {
        config.network_config.listen_enabled = false;
      } else if (arg.find("--threads=") == 0) {
        config.network_config.io_threads = std::stoi(arg.substr(10));
      } else if (arg.find("--suspiciousreorgdepth=") == 0) {
        config.suspicious_reorg_depth = std::stoi(arg.substr(23));
      } else if (arg == "--regtest") {
        config.chain_type = coinbasechain::chain::ChainType::REGTEST;
        config.network_config.network_magic =
            coinbasechain::protocol::magic::REGTEST;
        config.network_config.listen_port =
            coinbasechain::protocol::ports::REGTEST;
      } else if (arg == "--testnet") {
        config.chain_type = coinbasechain::chain::ChainType::TESTNET;
        config.network_config.network_magic =
            coinbasechain::protocol::magic::TESTNET;
        config.network_config.listen_port =
            coinbasechain::protocol::ports::TESTNET;
      } else if (arg == "--verbose") {
        config.verbose = true;
        log_level = "debug";
      } else {
        std::cerr << "Unknown option: " << arg << std::endl;
        print_usage(argv[0]);
        return 1;
      }
    }

    // Initialize logging system (enable file logging with debug.log)
    std::string log_file = (config.datadir / "debug.log").string();
    coinbasechain::util::LogManager::Initialize(log_level, true, log_file);

    // Create and initialize application
    coinbasechain::app::Application app(config);

    if (!app.initialize()) {
      LOG_ERROR("Failed to initialize application");
      return 1;
    }

    if (!app.start()) {
      LOG_ERROR("Failed to start application");
      return 1;
    }

    // Run until shutdown requested
    app.wait_for_shutdown();

    // Shutdown logging
    coinbasechain::util::LogManager::Shutdown();

    return 0;

  } catch (const std::exception &e) {
    // Use std::cerr here because logger may not be safe during exception
    // handling
    std::cerr << "Fatal exception: " << e.what() << std::endl;
    coinbasechain::util::LogManager::Shutdown();
    return 1;
  }
}
