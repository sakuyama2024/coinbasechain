#include "application.hpp"
#include "chain/logging.hpp"
#include "version.hpp"
#include <cstring>
#include <iostream> // Keep for CLI output and early errors before logger initialized

void print_usage(const char *program_name) {
  std::cout
      << "Usage: " << program_name << " [options]\n"
      << "\n"
      << "Options:\n"
      << "  --datadir=<path>     Data directory (default: ~/.coinbasechain)\n"
      << "  --port=<port>        Listen port (default: 9590 mainnet, 19590 testnet, 29590 regtest)\n"
      << "  --listen             Enable inbound connections\n"
      << "  --nolisten           Disable inbound connections (default)\n"
      << "  --threads=<n>        Number of IO threads (default: 4)\n"
      << "  --par=<n>            Number of parallel RandomX verification "
         "threads (default: 0 = auto)\n"
      << "  --suspiciousreorgdepth=<n>  Max reorg depth before halt (default: "
         "100, 0 = unlimited)\n"
      << "  --regtest            Use regression test chain (easy mining)\n"
      << "  --testnet            Use test network\n"
      << "\n"
      << "Logging:\n"
      << "  --loglevel=<level>   Set global log level (trace,debug,info,warn,error,critical)\n"
      << "                       Default: info\n"
      << "  --debug=<component>  Enable trace logging for specific component(s)\n"
      << "                       Components: network, sync, chain, crypto, app, all\n"
      << "                       Can be comma-separated: --debug=network,sync\n"
      << "  --verbose            Equivalent to --loglevel=debug\n"
      << "\n"
      << "Other:\n"
      << "  --version            Show version information\n"
      << "  --help               Show this help message\n"
      << std::endl;
}

int main(int argc, char *argv[]) {
  try {
    // Parse command line arguments
    coinbasechain::app::AppConfig config;
    std::string log_level = "info";
    std::vector<std::string> debug_components;

    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      if (arg == "--help") {
        print_usage(argv[0]);
        return 0;
      } else if (arg == "--version") {
        std::cout << coinbasechain::GetFullVersionString() << std::endl;
        std::cout << coinbasechain::GetCopyrightString() << std::endl;
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
      } else if (arg.find("--loglevel=") == 0) {
        log_level = arg.substr(11);
      } else if (arg.find("--debug=") == 0) {
        // Parse comma-separated components: --debug=net,sync,chain
        std::string components = arg.substr(8);
        size_t pos = 0;
        while (pos < components.length()) {
          size_t comma = components.find(',', pos);
          if (comma == std::string::npos) {
            debug_components.push_back(components.substr(pos));
            break;
          }
          debug_components.push_back(components.substr(pos, comma - pos));
          pos = comma + 1;
        }
      } else {
        std::cerr << "Unknown option: " << arg << std::endl;
        print_usage(argv[0]);
        return 1;
      }
    }

    // Initialize logging system (enable file logging with debug.log)
    std::string log_file = (config.datadir / "debug.log").string();
    coinbasechain::util::LogManager::Initialize(log_level, true, log_file);

    // Apply component-specific debug levels
    for (const auto& component : debug_components) {
      if (component == "all") {
        coinbasechain::util::LogManager::SetLogLevel("trace");
      } else if (component == "net" || component == "network") {
        coinbasechain::util::LogManager::SetComponentLevel("network", "trace");
      } else {
        coinbasechain::util::LogManager::SetComponentLevel(component, "trace");
      }
    }

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
