# Redis Cache Guard Module

A Redis module that implements intelligent cache management with graceful degradation to prevent cache stampedes and improve application performance.

## Overview

Cache Guard is a Redis module that provides smart caching with a "grace period" mechanism. When cached data is near expiration, it allows serving stale data to subsequent clients while only one client regenerates the cache, preventing the thundering herd problem.

## Key Features

- **Grace Period Management**: Serve stale data during cache regeneration
- **Stampede Prevention**: Only one client regenerates expired cache at a time
- **Automatic Lock Management**: Built-in regeneration locks with expiration
- **Simple API**: Easy-to-use GET/SET commands with expiration support

## How It Works

1. **Normal Operation**: Cache hits return fresh data immediately
2. **Grace Period**: When cache is near expiration (within grace period):
   - First client gets `null` and regenerates the cache
   - Subsequent clients get stale data while regeneration happens
3. **Cache Miss**: Returns `null` to trigger cache generation
4. **Lock Management**: Automatic cleanup of regeneration locks

## Commands

### `cache.guard.get <key> <grace_period_ms>`

Retrieves a cached value with intelligent grace period handling.

**Parameters:**
- `key`: The cache key to retrieve
- `grace_period_ms`: Time in milliseconds before expiration to start graceful degradation

**Returns:**
- Cached value if valid and not in grace period
- Stale cached value if another client is regenerating
- `null` if cache is missing or client should regenerate

**Example:**
```redis
cache.guard.get user:123 5000
```

### `cache.guard.set <key> <value> <expire_ms>`

Sets a cached value with expiration time.

**Parameters:**
- `key`: The cache key to set
- `value`: The value to cache
- `expire_ms`: Expiration time in milliseconds

**Returns:**
- `OK` on successful set

**Example:**
```redis
cache.guard.set user:123 "user_data_json" 60000
```

## Installation

### Prerequisites
- Redis 4.0+
- GCC or compatible C compiler
- Redis module development headers

### Building the Module

```bash
# Clone the repository
git clone <repository-url>
cd cache-redis

# Compile the module
gcc -fPIC -shared -o cacheguard.so cache-anit-tampede.c -I/path/to/redis/src

# Or if you have redis-server installed with headers
gcc -fPIC -shared -o cacheguard.so cache-anit-tampede.c
```

### Loading the Module

```bash
# Method 1: Load at Redis startup
redis-server --loadmodule ./cacheguard.so

# Method 2: Load dynamically
redis-cli> MODULE LOAD /path/to/cacheguard.so
```

## Usage Examples

### Basic Caching Pattern

```python
import redis

r = redis.Redis()

# Check cache
result = r.execute_command('cache.guard.get', 'expensive_data', 5000)

if result is None:
    # Cache miss or in grace period - regenerate
    expensive_data = compute_expensive_operation()
    r.execute_command('cache.guard.set', 'expensive_data', expensive_data, 60000)
    return expensive_data
else:
    # Cache hit - return cached data
    return result
```

### Web Application Integration

```python
def get_user_profile(user_id):
    cache_key = f"user_profile:{user_id}"
    grace_period = 10000  # 10 seconds
    
    # Try to get from cache
    cached_profile = redis_client.execute_command(
        'cache.guard.get', cache_key, grace_period
    )
    
    if cached_profile is None:
        # Generate fresh data
        profile = fetch_user_from_database(user_id)
        
        # Cache for 5 minutes
        redis_client.execute_command(
            'cache.guard.set', cache_key, 
            json.dumps(profile), 300000
        )
        return profile
    else:
        # Return cached (possibly stale) data
        return json.loads(cached_profile)
```

## Architecture Details

### Grace Period Logic

```
Cache Lifetime: |-------- VALID --------|-- GRACE --|
Time:          0                      TTL-Grace   TTL

- VALID: Return cached value immediately
- GRACE: First client regenerates, others get stale data
- EXPIRED: All clients get null (cache miss)
```

### Lock Mechanism

- Regeneration locks use the pattern: `{original_key}:regen_lock`
- Locks expire automatically using the grace period duration
- Automatic cleanup when new data is set
- Prevents multiple concurrent regenerations

### Memory Management

- Uses Redis module automatic memory management
- Efficient string operations with zero-copy where possible
- Minimal memory overhead for lock keys

## Configuration

The module accepts the following parameters during load:

```redis
MODULE LOAD /path/to/cacheguard.so
```

Currently, no additional configuration parameters are supported.

## Performance Considerations

- **Cache Stampede Prevention**: Dramatically reduces database load during cache expiration
- **Low Latency**: Stale data serving keeps response times consistent
- **Memory Efficient**: Minimal overhead for lock management
- **Thread Safe**: Fully compatible with Redis's single-threaded model

## Error Handling

The module returns appropriate Redis errors for:
- Invalid argument counts
- Invalid numeric parameters (negative grace periods, invalid expiration times)
- Memory allocation failures

## Monitoring

Monitor the effectiveness of Cache Guard by tracking:
- Cache hit rates
- Regeneration lock creation frequency
- Application response time consistency
- Database load patterns

## Contributing

1. Fork the repository
2. Create a feature branch
3. Add tests for new functionality
4. Submit a pull request

## License

MIT License

Copyright (c) 2024

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Support

For issues and questions:
- Open an issue on GitHub
- Check existing documentation
- Review Redis module development guides

---

**Version**: 1.0  
**Compatibility**: Redis 4.0+  
**Module Name**: `cacheguard` 
