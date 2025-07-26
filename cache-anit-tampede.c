#include "redismodule.h"
#include <string.h>

#define REGEN_LOCK_SUFFIX ":regen_lock"

int TryAcquireLock(RedisModuleCtx *ctx, RedisModuleString *key, long long lockExpireMs) {
    size_t len;
    const char *keystr = RedisModule_StringPtrLen(key, &len);

    char lockName[len + sizeof(REGEN_LOCK_SUFFIX)];
    memcpy(lockName, keystr, len);
    memcpy(lockName + len, REGEN_LOCK_SUFFIX, sizeof(REGEN_LOCK_SUFFIX) - 1);
    lockName[len + sizeof(REGEN_LOCK_SUFFIX) - 1] = '\0';

    RedisModuleString *lockKey = RedisModule_CreateString(ctx, lockName, strlen(lockName));
    RedisModuleKey *lock = RedisModule_OpenKey(ctx, lockKey, REDISMODULE_WRITE);

    int acquired = 0;
    if (RedisModule_KeyType(lock) == REDISMODULE_KEYTYPE_EMPTY) {
        if (RedisModule_StringSetNX(lock, RedisModule_CreateStringFromLongLong(ctx, 1)) == REDISMODULE_OK) {
            RedisModule_SetExpire(lock, lockExpireMs);
            acquired = 1;
        }
    }

    RedisModule_CloseKey(lock);
    return acquired;
}

int CacheGuardGetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // Usage: cache.guard.get <key> <grace_period_ms>
    if (argc != 3) return RedisModule_WrongArity(ctx);

    RedisModule_AutoMemory(ctx);
    RedisModuleString *key = argv[1];
    long long gracePeriodMs;
    if (RedisModule_StringToLongLong(argv[2], &gracePeriodMs) != REDISMODULE_OK || gracePeriodMs < 0)
        return RedisModule_ReplyWithError(ctx, "ERR invalid grace period");

    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_READ | REDISMODULE_WRITE);
    if (RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        // Key missing => return null to regenerate
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithNull(ctx);
    }

    mstime_t ttl = RedisModule_GetExpire(k);
    RedisModuleString *val;
    RedisModule_StringGet(k, &val);

    if (ttl == REDISMODULE_NO_EXPIRE || ttl > gracePeriodMs) {
        // Cache valid and NOT within grace period, return cached value
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithString(ctx, val);
    }

    // Cache within grace period or expired: try to acquire regeneration lock
    int lockAcquired = TryAcquireLock(ctx, key, gracePeriodMs);

    if (lockAcquired) {
        // First client within grace period: return null to regenerate
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithNull(ctx);
    } else {
        // Lock held by another client regenerating => return stale cached value
        RedisModule_CloseKey(k);
        return RedisModule_ReplyWithString(ctx, val);
    }
}

int CacheGuardSetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // Usage: cache.guard.set <key> <value> <expire_ms>
    if (argc != 4) return RedisModule_WrongArity(ctx);

    RedisModule_AutoMemory(ctx);

    RedisModuleString *key = argv[1];
    RedisModuleString *value = argv[2];
    long long expire;
    if (RedisModule_StringToLongLong(argv[3], &expire) != REDISMODULE_OK || expire <= 0)
        return RedisModule_ReplyWithError(ctx, "ERR invalid expire time");

    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_WRITE);
    RedisModule_StringSet(k, value);
    RedisModule_SetExpire(k, expire);
    RedisModule_CloseKey(k);

    // Delete regeneration lock to release others
    size_t len;
    const char *keystr = RedisModule_StringPtrLen(key, &len);
    char lockName[len + sizeof(REGEN_LOCK_SUFFIX)];
    memcpy(lockName, keystr, len);
    memcpy(lockName + len, REGEN_LOCK_SUFFIX, sizeof(REGEN_LOCK_SUFFIX) - 1);
    lockName[len + sizeof(REGEN_LOCK_SUFFIX) - 1] = '\0';

    RedisModuleString *lockKey = RedisModule_CreateString(ctx, lockName, strlen(lockName));
    RedisModuleKey *lock = RedisModule_OpenKey(ctx, lockKey, REDISMODULE_WRITE);
    RedisModule_DeleteKey(lock);
    RedisModule_CloseKey(lock);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "cacheguard", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "cache.guard.get", CacheGuardGetCommand, "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "cache.guard.set", CacheGuardSetCommand, "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
