#!/usr/bin/env python3
"""Test runner for functional tests."""

import sys
import os
import subprocess
from pathlib import Path


def run_test(test_script):
    """Run a single test script."""
    print(f"\n{'=' * 60}")
    print(f"Running: {test_script.name}")
    print('=' * 60)

    result = subprocess.run(
        [sys.executable, str(test_script)],
        cwd=test_script.parent.parent.parent
    )

    return result.returncode == 0


def main():
    """Run all functional tests."""
    test_dir = Path(__file__).parent

    # Files to exclude (setup scripts, debug scripts, and infrastructure)
    exclude_files = {
        "test_runner.py",
        "generate_test_chain.py",    # Setup script, not a test
        "generate_test_chains.py",   # Setup script, not a test
        "regenerate_test_chains.py", # Setup script, not a test
        "debug_sync_issue.py",       # Debug script, not a test
    }

    # Find all test scripts (only feature_* and test_* files)
    test_scripts = []
    for file in sorted(test_dir.glob("*.py")):
        if file.name not in exclude_files and not file.name.startswith("_"):
            # Only include files that start with "feature_" or "test_"
            if file.name.startswith("feature_") or file.name.startswith("test_"):
                test_scripts.append(file)

    if not test_scripts:
        print("No test scripts found!")
        return 1

    print(f"Found {len(test_scripts)} test(s)")

    # Run all tests
    results = {}
    for test_script in test_scripts:
        success = run_test(test_script)
        results[test_script.name] = success

    # Print summary
    print(f"\n{'=' * 60}")
    print("Test Summary")
    print('=' * 60)

    passed = sum(1 for success in results.values() if success)
    failed = len(results) - passed

    for test_name, success in results.items():
        status = "✓ PASSED" if success else "✗ FAILED"
        print(f"{status}: {test_name}")

    print(f"\n{passed} passed, {failed} failed out of {len(results)} tests")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
