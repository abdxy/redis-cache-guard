#include "redismodule.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

int CacheSetSMCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) return RedisModule_WrongArity(ctx);

    RedisModule_AutoMemory(ctx);

    RedisModuleString *key = argv[1];
    RedisModuleString *value = argv[2];
    long long expire;
    if (RedisModule_StringToLongLong(argv[3], &expire) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR invalid expire time");

    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_WRITE);
    RedisModule_StringSet(k, value);
    RedisModule_SetExpire(k, expire);
    RedisModule_CloseKey(k);

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int CacheGetSMCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    RedisModule_AutoMemory(ctx);
    RedisModuleString *key = argv[1];
    RedisModuleKey *k = RedisModule_OpenKey(ctx, key, REDISMODULE_READ | REDISMODULE_WRITE);

    if (RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_EMPTY) {
        return RedisModule_ReplyWithNull(ctx); // no value, caller should regenerate
    }

    mstime_t ttl = RedisModule_GetExpire(k);
    if (ttl == REDISMODULE_NO_EXPIRE || ttl > 0) {
        RedisModuleString *val;
        RedisModule_StringGet(k, &val);
        return RedisModule_ReplyWithString(ctx, val);
    }

    // key expired: delete it so only 1 thread gets null
    RedisModule_DeleteKey(k);
    return RedisModule_ReplyWithNull(ctx);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "cachemod", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "cache.setsm", CacheSetSMCommand, "write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "cache.getsm", CacheGetSMCommand, "read write", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
