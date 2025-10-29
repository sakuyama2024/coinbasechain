// Copyright (c) 2024 Coinbase Chain
// Test helper for ChainstateManager with PoW bypass

#ifndef COINBASECHAIN_TEST_CHAINSTATE_MANAGER_HPP
#define COINBASECHAIN_TEST_CHAINSTATE_MANAGER_HPP

#include "chain/chainstate_manager.hpp"
#include "chain/randomx_pow.hpp"

namespace coinbasechain {
namespace test {

/**
 * TestChainstateManager - Test version that bypasses PoW validation
 *
 * This allows unit tests to run without expensive RandomX mining.
 * Inherits from ChainstateManager and overrides CheckProofOfWork
 * to always return true.
 *
 * Usage:
 *   TestChainstateManager chainstate(*params);
 *   chainstate.Initialize(params->GenesisBlock());
 *   // Now headers can be accepted without valid PoW
 */
class TestChainstateManager : public validation::ChainstateManager {
public:
    /**
     * Constructor - same as ChainstateManager
     */
    explicit TestChainstateManager(const chain::ChainParams& params)
        : ChainstateManager(params)
        , bypass_pow_validation_(true)
        , bypass_contextual_validation_(true)
    {
    }

    /**
     * Enable or disable PoW validation bypass
     *
     * When bypass_pow_validation is true (default), CheckProofOfWork always returns true.
     * When false, it calls the real ChainstateManager::CheckProofOfWork.
     *
     * This allows misbehavior tests to detect invalid PoW while keeping most tests fast.
     */
    void SetBypassPOWValidation(bool bypass) {
        printf("[TestChainstateManager %p] SetBypassPOWValidation(%s) - was %d, now %d\n",
               this, bypass ? "true" : "false", bypass_pow_validation_, bypass);
        bypass_pow_validation_ = bypass;
    }

    /**
     * Enable or disable contextual validation bypass (difficulty/timestamp)
     * Default: true (bypass). Set to false to exercise contextual checks in tests.
     */
    void SetBypassContextualValidation(bool bypass) {
        printf("[TestChainstateManager %p] SetBypassContextualValidation(%s) - was %d, now %d\n",
               this, bypass ? "true" : "false", bypass_contextual_validation_, bypass);
        bypass_contextual_validation_ = bypass;
    }

protected:
    /**
     * Override CheckProofOfWork to conditionally bypass validation
     *
     * When bypass_pow_validation_ is true (default), returns true without checking.
     * When false, calls real ChainstateManager::CheckProofOfWork for actual validation.
     * This is ONLY safe for unit tests where we control all inputs.
     */
    bool CheckProofOfWork(const CBlockHeader& header,
                         crypto::POWVerifyMode mode) const override
    {
        if (bypass_pow_validation_) {
            // Bypass PoW validation for tests
            printf("[TestChainstateManager %p] CheckProofOfWork: BYPASSED (bypass_pow_validation_=%d)\n", this, bypass_pow_validation_);
            return true;
        }
        // Use real PoW validation (for misbehavior tests)
        printf("[TestChainstateManager %p] CheckProofOfWork: CALLING REAL VALIDATION (bypass_pow_validation_=%d)\n", this, bypass_pow_validation_);
        bool result = ChainstateManager::CheckProofOfWork(header, mode);
        printf("[TestChainstateManager %p] CheckProofOfWork: result=%s\n", this, result ? "true" : "false");
        return result;
    }

private:
    bool bypass_pow_validation_;
    bool bypass_contextual_validation_;

    /**
     * Override CheckBlockHeaderWrapper to conditionally bypass validation
     *
     * When bypass_pow_validation_ is true (default), returns true without checking.
     * When false, calls real ChainstateManager::CheckBlockHeaderWrapper for actual validation.
     * This is ONLY safe for unit tests where we control all inputs.
     */
    bool CheckBlockHeaderWrapper(const CBlockHeader& header,
                                 validation::ValidationState& state) const override
    {
        if (bypass_pow_validation_) {
            // Bypass all header validation for tests
            return true;
        }
        // Use real header validation (for misbehavior tests)
        return ChainstateManager::CheckBlockHeaderWrapper(header, state);
    }

    /**
     * Override ContextualCheckBlockHeaderWrapper to optionally bypass contextual validation
     */
    bool ContextualCheckBlockHeaderWrapper(const CBlockHeader& header,
                                           const chain::CBlockIndex* pindexPrev,
                                           int64_t adjusted_time,
                                           validation::ValidationState& state) const override
    {
        if (bypass_contextual_validation_) {
            // Bypass contextual validation for unit tests
            // This allows tests to create arbitrary header chains without
            // worrying about difficulty adjustments or timestamp constraints
            return true;
        }
        return ChainstateManager::ContextualCheckBlockHeaderWrapper(header, pindexPrev, adjusted_time, state);
    }
};

} // namespace test
} // namespace coinbasechain

#endif // COINBASECHAIN_TEST_CHAINSTATE_MANAGER_HPP
