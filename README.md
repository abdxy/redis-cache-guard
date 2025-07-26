# cache-guard

A Redis module for preventing **cache stampede** by providing simple, key-level cache management commands.

## Overview

**cache-guard** is a C-based Redis module that introduces two commands: `CACHE.SETSM` and `CACHE.GETSM`. These commands help manage cache entries and their expiration in a way that reduces the risk of cache stampede, ensuring that only one process regenerates a missing or expired cache entry at a time.

## Why cache-guard?

In high-traffic systems, a cache miss can trigger many clients to simultaneously rebuild the same cache entry, overloading backend resources. **cache-guard** addresses this by providing atomic cache get/set operations with expiration, so only one client sees a true miss and others wait for the cache to be rebuilt.

## Commands

- **`CACHE.SETSM <key> <value> <expire>`**  
  Sets the value for `<key>` with an expiration time (in milliseconds).  
  - Returns `OK` on success.
  - Example:  
    ```sh
    CACHE.SETSM my-key "some value" 60000
    ```

- **`CACHE.GETSM <key>`**  
  Gets the value for `<key>`.  
  - If the key exists and is not expired, returns the value.
  - If the key is missing or expired, deletes the key and returns null, signaling the caller to regenerate the cache.
  - Example:  
    ```sh
    CACHE.GETSM my-key
    ```

## How it works

- When a cache entry expires, the first client to call `CACHE.GETSM` will receive a null response and is responsible for regenerating the cache.
- Other clients will wait for the cache to be set again, preventing multiple simultaneous rebuilds.
- The `CACHE.SETSM` command sets the cache value and its expiration.

## Installation

1. Build the module:
   ```sh
   make
   ```
2. Load it into Redis:
   ```sh
   redis-server --loadmodule /path/to/cache-guard.so
   ```

## Example Workflow

1. On cache miss, call `CACHE.GETSM key`.
2. If null is returned, regenerate the value and set it with `CACHE.SETSM key value expire`.
3. If a value is returned, use it directly.

## License

MIT License

---

Contributions and feedback are