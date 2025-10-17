// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_UTIL_MACROS_HPP
#define COINBASECHAIN_UTIL_MACROS_HPP

#define PASTE(x, y) x ## y
#define PASTE2(x, y) PASTE(x, y)

/**
 * Converts the parameter X to a string after macro replacement on X has been performed.
 * Don't merge these into one macro!
 */
#define STRINGIZE(X) DO_STRINGIZE(X)
#define DO_STRINGIZE(X) #X

/**
 * Generates a unique variable name with the given prefix
 * Used in LOCK macro to create unique variable names for lock guards
 */
#define UNIQUE_NAME(prefix) PASTE2(prefix, __LINE__)

#endif // COINBASECHAIN_UTIL_MACROS_HPP
