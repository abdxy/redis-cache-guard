#include "redismodule.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

// Configuration constants
#define REGEN_LOCK_SUFFIX ":regen_lock"
#define MAX_KEY_LENGTH 512
#define MODULE_VERSION "1.0.1"
#define MIN_GRACE_PERIOD_MS 100
#define MAX_GRACE_PERIOD_MS (24 * 60 * 60 * 1000) // 24 hours
#define MIN_EXPIRE_MS 1000
#define MAX_EXPIRE_MS (7 * 24 * 60 * 60 * 1000) // 7 days

// Module context for configuration
static struct {
    int log_level;
    long long default_grace_period;
    long long max_lock_duration;
} module_config = {
    .log_level = 1,  // 0=debug, 1=notice, 2=warning, 3=error
    .default_grace_period = 5000,
    .max_lock_duration = 30000
};

// Logging macros
#define LOG_DEBUG(ctx, fmt, ...) \
    if (module_config.log_level <= 0) \
        RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_DEBUG, "CacheGuard: " fmt, ##__VA_ARGS__)

#define LOG_NOTICE(ctx, fmt, ...) \
    if (module_config.log_level <= 1) \
        RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_NOTICE, "CacheGuard: " fmt, ##__VA_ARGS__)

#define LOG_WARNING(ctx, fmt, ...) \
    if (module_config.log_level <= 2) \
        RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_WARNING, "CacheGuard: " fmt, ##__VA_ARGS__)

// Enhanced lock key generation with safety checks
static RedisModuleString *CreateLockKey(RedisModuleCtx *ctx, RedisModuleString *key) {
    size_t len;
    const char *keystr = RedisModule_StringPtrLen(key, &len);
    
    // Validate key length to prevent buffer overflow
    if (len == 0) {
        LOG_WARNING(ctx, "Empty key provided");
        return NULL;
    }
    
    if (len > MAX_KEY_LENGTH - sizeof(REGEN_LOCK_SUFFIX)) {
        LOG_WARNING(ctx, "Key too long: %zu bytes", len);
        return NULL;
    }
    
    // Safe buffer allocation and construction
    size_t lockNameLen = len + sizeof(REGEN_LOCK_SUFFIX) - 1;
    char *lockName = RedisModule_Alloc(lockNameLen + 1);
    if (!lockName) {
        LOG_WARNING(ctx, "Failed to allocate memory for lock key");
        return NULL;
    }
    
    memcpy(lockName, keystr, len);
    memcpy(lockName + len, REGEN_LOCK_SUFFIX, sizeof(REGEN_LOCK_SUFFIX) - 1);
    lockName[lockNameLen] = '\0';
    
    RedisModuleString *lockKey = RedisModule_CreateString(ctx, lockName, lockNameLen);
    RedisModule_Free(lockName);
    
    return lockKey;
}

// Enhanced lock acquisition with better error handling
int TryAcquireLock(RedisModuleCtx *ctx, RedisModuleString *key, long long lockExpireMs) {
    if (!key) {
        LOG_WARNING(ctx, "NULL key provided to TryAcquireLock");
        return 0;
    }
    
    // Validate lock expiration time
    if (lockExpireMs < MIN_GRACE_PERIOD_MS || lockExpireMs > module_config.max_lock_duration) {
        LOG_WARNING(ctx, "Invalid lock expiration: %lld ms", lockExpireMs);
        return 0;
    }
    
    RedisModuleString *lockKey = CreateLockKey(ctx, key);
    if (!lockKey) {
        return 0;
    }
    
    RedisModuleKey *lock = RedisModule_OpenKey(ctx, lockKey, REDISMODULE_WRITE);
    if (!lock) {
        LOG_WARNING(ctx, "Failed to open lock key");
        return 0;
    }
    
    int acquired = 0;
    if (RedisModule_KeyType(lock) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModuleString *lockValue = RedisModule_CreateStringFromLongLong(ctx, 1);
        if (RedisModule_StringSet(lock, lockValue) == REDISMODULE_OK) {
            if (RedisModule_SetExpire(lock, lockExpireMs) == REDISMODULE_OK) {
                acquired = 1;
                LOG_DEBUG(ctx, "Lock acquired for key, expires in %lld ms", lockExpireMs);
            } else {
                LOG_WARNING(ctx, "Failed to set lock expiration");
                RedisModule_DeleteKey(lock);
            }
        } else {
            LOG_WARNING(ctx, "Failed to set lock value");
        }
    } else {
        LOG_DEBUG(ctx, "Lock already exists for key");
    }
    
    RedisModule_CloseKey(lock);
    return acquired;
}

// Enhanced GET command with comprehensive validation
int CacheGuardGetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_AutoMemory(ctx);
    
    RedisModuleString *key = argv[1];
    if (!key) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid key");
    }
    
    // Validate grace period
    long long gracePeriodMs;
    if (RedisModule_StringToLongLong(argv[2], &gracePeriodMs) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid grace period format");
    }
    
    if (gracePeriodMs < MIN_GRACE_PERIOD_MS || gracePeriodMs > MAX_GRACE_PERIOD_MS) {
        return RedisModule_ReplyWithError(ctx, 
            "ERR grace period must be between 100ms and 24 hours");
    }

    // Validate key length
    size_t keyLen;
    RedisModule_StringPtrLen(key, &keyLen);
    if (keyLen == 0) {
        return RedisModule_ReplyWithError(ctx, "ERR empty key not allowed");
    }
    if (keyLen > MAX_KEY_LENGTH) {
        return RedisModule_ReplyWithError(ctx, "ERR key too long");
    }

    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_READ | REDISMODULE_WRITE);
    if (!k) {
        LOG_WARNING(ctx, "Failed to open key");
        return RedisModule_ReplyWithError(ctx, "ERR failed to access key");
    }
    
    if (RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        LOG_DEBUG(ctx, "Cache miss - key not found");
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithNull(ctx);
    }

    // Check if key contains string data
    if (RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_STRING) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR key contains non-string data");
    }

    mstime_t ttl = RedisModule_GetExpire(k);
    
    // Get the value with error checking
    size_t valueLen;
    const char *valuePtr = RedisModule_StringDMA(k, &valueLen, REDISMODULE_READ);
    if (!valuePtr) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR failed to read value");
    }
    
    RedisModuleString *val = RedisModule_CreateString(ctx, valuePtr, valueLen);

    if (ttl == REDISMODULE_NO_EXPIRE || ttl > gracePeriodMs) {
        // Cache valid and NOT within grace period
        LOG_DEBUG(ctx, "Cache hit - returning fresh data (TTL: %lld ms)", ttl);
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithString(ctx, val);
    }

    // Cache within grace period or expired: try to acquire regeneration lock
    LOG_DEBUG(ctx, "Cache in grace period (TTL: %lld ms, grace: %lld ms)", ttl, gracePeriodMs);
    
    int lockAcquired = TryAcquireLock(ctx, key, gracePeriodMs);
    RedisModule_CloseKey(k);

    if (lockAcquired) {
        LOG_DEBUG(ctx, "Lock acquired - requesting regeneration");
        return RedisModule_ReplyWithNull(ctx);
    } else {
        LOG_DEBUG(ctx, "Lock held by another client - returning stale data");
        return RedisModule_ReplyWithString(ctx, val);
    }
}

// Enhanced SET command with validation and cleanup
int CacheGuardSetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_AutoMemory(ctx);

    RedisModuleString *key = argv[1];
    RedisModuleString *value = argv[2];
    
    if (!key || !value) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid key or value");
    }
    
    // Validate key length
    size_t keyLen;
    RedisModule_StringPtrLen(key, &keyLen);
    if (keyLen == 0) {
        return RedisModule_ReplyWithError(ctx, "ERR empty key not allowed");
    }
    if (keyLen > MAX_KEY_LENGTH) {
        return RedisModule_ReplyWithError(ctx, "ERR key too long");
    }
    
    // Validate value length (prevent excessive memory usage)
    size_t valueLen;
    RedisModule_StringPtrLen(value, &valueLen);
    if (valueLen > 10 * 1024 * 1024) { // 10MB limit
        return RedisModule_ReplyWithError(ctx, "ERR value too large");
    }
    
    // Validate expiration time
    long long expire;
    if (RedisModule_StringToLongLong(argv[3], &expire) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid expire time format");
    }
    
    if (expire < MIN_EXPIRE_MS || expire > MAX_EXPIRE_MS) {
        return RedisModule_ReplyWithError(ctx, 
            "ERR expire time must be between 1 second and 7 days");
    }

    // Set the main cache key
    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_WRITE);
    if (!k) {
        return RedisModule_ReplyWithError(ctx, "ERR failed to access key");
    }
    
    if (RedisModule_StringSet(k, value) != REDISMODULE_OK) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR failed to set value");
    }
    
    if (RedisModule_SetExpire(k, expire) != REDISMODULE_OK) {
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithError(ctx, "ERR failed to set expiration");
    }
    
    RedisModule_CloseKey(k);

    // Clean up regeneration lock
    RedisModuleString *lockKey = CreateLockKey(ctx, key);
    if (lockKey) {
        RedisModuleKey *lock = RedisModule_OpenKey(ctx, lockKey, REDISMODULE_WRITE);
        if (lock) {
            if (RedisModule_KeyType(lock) != REDISMODULE_KEYTYPE_EMPTY) {
                RedisModule_DeleteKey(lock);
                LOG_DEBUG(ctx, "Regeneration lock released");
            }
            RedisModule_CloseKey(lock);
        }
    }

    LOG_DEBUG(ctx, "Cache set successfully (expires in %lld ms)", expire);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// Module info command for observability
int CacheGuardInfoCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    
    RedisModule_ReplyWithArray(ctx, 8);
    
    RedisModule_ReplyWithSimpleString(ctx, "module");
    RedisModule_ReplyWithSimpleString(ctx, "cacheguard");
    
    RedisModule_ReplyWithSimpleString(ctx, "version");
    RedisModule_ReplyWithSimpleString(ctx, MODULE_VERSION);
    
    RedisModule_ReplyWithSimpleString(ctx, "max_key_length");
    RedisModule_ReplyWithLongLong(ctx, MAX_KEY_LENGTH);
    
    RedisModule_ReplyWithSimpleString(ctx, "max_lock_duration_ms");
    RedisModule_ReplyWithLongLong(ctx, module_config.max_lock_duration);
    
    return REDISMODULE_OK;
}

// Configuration command
int CacheGuardConfigCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }
    
    size_t cmdLen;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmdLen);
    
    if (strcasecmp(cmd, "GET") == 0) {
        if (argc != 3) return RedisModule_WrongArity(ctx);
        
        size_t paramLen;
        const char *param = RedisModule_StringPtrLen(argv[2], &paramLen);
        
        if (strcasecmp(param, "log_level") == 0) {
            return RedisModule_ReplyWithLongLong(ctx, module_config.log_level);
        } else if (strcasecmp(param, "max_lock_duration") == 0) {
            return RedisModule_ReplyWithLongLong(ctx, module_config.max_lock_duration);
        } else {
            return RedisModule_ReplyWithError(ctx, "ERR unknown parameter");
        }
    } else if (strcasecmp(cmd, "SET") == 0) {
        if (argc != 4) return RedisModule_WrongArity(ctx);
        
        size_t paramLen;
        const char *param = RedisModule_StringPtrLen(argv[2], &paramLen);
        
        long long value;
        if (RedisModule_StringToLongLong(argv[3], &value) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "ERR invalid value");
        }
        
        if (strcasecmp(param, "log_level") == 0) {
            if (value < 0 || value > 3) {
                return RedisModule_ReplyWithError(ctx, "ERR log level must be 0-3");
            }
            module_config.log_level = value;
            return RedisModule_ReplyWithSimpleString(ctx, "OK");
        } else if (strcasecmp(param, "max_lock_duration") == 0) {
            if (value < 1000 || value > 300000) {
                return RedisModule_ReplyWithError(ctx, "ERR max lock duration must be 1s-5m");
            }
            module_config.max_lock_duration = value;
            return RedisModule_ReplyWithSimpleString(ctx, "OK");
        } else {
            return RedisModule_ReplyWithError(ctx, "ERR unknown parameter");
        }
    } else {
        return RedisModule_ReplyWithError(ctx, "ERR unknown subcommand");
    }
}

// Module initialization with enhanced error handling
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    
    if (RedisModule_Init(ctx, "cacheguard", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    // Register main commands
    if (RedisModule_CreateCommand(ctx, "cache.guard.get", CacheGuardGetCommand, 
                                 "write fast", 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "cache.guard.set", CacheGuardSetCommand, 
                                 "write", 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    // Register utility commands
    if (RedisModule_CreateCommand(ctx, "cache.guard.info", CacheGuardInfoCommand, 
                                 "readonly fast", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    if (RedisModule_CreateCommand(ctx, "cache.guard.config", CacheGuardConfigCommand, 
                                 "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    LOG_NOTICE(ctx, "Cache Guard module loaded successfully (version %s)", MODULE_VERSION);
    return REDISMODULE_OK;
} 
