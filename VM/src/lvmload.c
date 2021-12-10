// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lvm.h"

#include "lstate.h"
#include "ltable.h"
#include "lfunc.h"
#include "lstring.h"
#include "lgc.h"
#include "lmem.h"
#include "lbytecode.h"
#include "lapi.h"

#include <string.h>

void luaV_getimport(lua_State* L, Table* env, TValue* k, uint32_t id, int propagatenil)
{
    int count = id >> 30;
    int id0 = count > 0 ? (int)(id >> 20) & 1023 : -1;
    int id1 = count > 1 ? (int)(id >> 10) & 1023 : -1;
    int id2 = count > 2 ? (int)(id)&1023 : -1;

    // allocate a stack slot so that we can do table lookups
    luaD_checkstack(L, 1);
    setnilvalue(L->top);
    L->top++;

    // global lookup into L->top-1
    TValue g;
    sethvalue(L, &g, env);
    luaV_gettable(L, &g, &k[id0], L->top - 1);

    // table lookup for id1
    if (id1 >= 0 && (!propagatenil || !ttisnil(L->top - 1)))
        luaV_gettable(L, L->top - 1, &k[id1], L->top - 1);

    // table lookup for id2
    if (id2 >= 0 && (!propagatenil || !ttisnil(L->top - 1)))
        luaV_gettable(L, L->top - 1, &k[id2], L->top - 1);
}

static uint8_t read_uint8_t(const char* data, size_t size, size_t* offset)
{
    (void)sizeof(size);
    uint8_t result;
    memcpy(&result, data + *offset, sizeof(uint8_t));
    *offset += sizeof(uint8_t);

    return result;
}

static uint32_t read_int32_t(const char* data, size_t size, size_t* offset)
{
    (void)sizeof(size);
    int32_t result;
    memcpy(&result, data + *offset, sizeof(int32_t));
    *offset += sizeof(int32_t);

    return result;
}

static uint32_t read_uint32_t(const char* data, size_t size, size_t* offset)
{
    (void)sizeof(size);
    uint32_t result;
    memcpy(&result, data + *offset, sizeof(uint32_t));
    *offset += sizeof(uint32_t);

    return result;
}

static double read_double(const char* data, size_t size, size_t* offset)
{
    (void)sizeof(size);
    double result;
    memcpy(&result, data + *offset, sizeof(double));
    *offset += sizeof(double);

    return result;
}

static unsigned int readVarInt(const char* data, size_t size, size_t* offset)
{
    unsigned int result = 0;
    unsigned int shift = 0;

    uint8_t byte;

    do
    {
        byte = read_uint8_t(data, size, offset);
        result |= (byte & 127) << shift;
        shift += 7;
    } while (byte & 128);

    return result;
}

static TString* readString(TString** strings, const char* data, size_t size, size_t* offset)
{
    unsigned int id = readVarInt(data, size, offset);

    return id == 0 ? 0 : strings[id - 1];
}

typedef struct ResolveImport
{
    TValue* k;
    uint32_t id;
} ResolveImport;

static void runResolveImport(lua_State* L, void* ud)
{
    ResolveImport* self = (ResolveImport*)ud;

    // note: we call getimport with nil propagation which means that accesses to table chains like A.B.C will resolve in nil
    // this is technically not necessary but it reduces the number of exceptions when loading scripts that rely on getfenv/setfenv for global
    // injection
    luaV_getimport(L, hvalue(gt(L)), self->k, self->id, /* propagatenil= */ 1);
}

static void resolveImportSafe(lua_State* L, Table* env, TValue* k, uint32_t id)
{
    (void)sizeof(env);
    ResolveImport ri = {k, id};
    if (hvalue(gt(L))->safeenv)
    {
        // luaD_pcall will make sure that if any C/Lua calls during import resolution fail, the thread state is restored back
        int oldTop = lua_gettop(L);
        int status = luaD_pcall(L, runResolveImport, &ri, savestack(L, L->top), 0);
        LUAU_ASSERT(oldTop + 1 == lua_gettop(L)); // if an error occurred, luaD_pcall saves it on stack

        if (status != 0)
        {
            // replace error object with nil
            setnilvalue(L->top - 1);
        }
    }
    else
    {
        setnilvalue(L->top);
        L->top++;
    }
}

int luau_load(lua_State* L, const char* chunkname, const char* data, size_t size, int env)
{
    size_t offset = 0;

    uint8_t version = read_uint8_t(data, size, &offset);

    // 0 means the rest of the bytecode is the error message
    if (version == 0 || version != LBC_VERSION)
    {
        char chunkid[LUA_IDSIZE];
        luaO_chunkid(chunkid, chunkname, LUA_IDSIZE);

        if (version == 0)
            lua_pushfstring(L, "%s%.*s", chunkid, (int)(size - offset), data + offset);
        else
            lua_pushfstring(L, "%s: bytecode version mismatch", chunkid);
        return 1;
    }

    // pause GC for the duration of deserialization - some objects we're creating aren't rooted
    // TODO: if an allocation error happens mid-load, we do not unpause GC!
    size_t GCthreshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;

    // env is 0 for current environment and a stack index otherwise
    Table* envt = (env == 0) ? hvalue(gt(L)) : hvalue(luaA_toobject(L, env));

    TString* source = luaS_new(L, chunkname);

    // string table
    unsigned int stringCount = readVarInt(data, size, &offset);
    TString** strings = luaM_new(L, TString*, sizeof(TString*) * stringCount, L->activememcat);

    for (unsigned int i = 0; i < stringCount; ++i)
    {
        unsigned int length = readVarInt(data, size, &offset);

        strings[i] = luaS_newlstr(L, data + offset, length);
        offset += length;
    }

    // proto table
    unsigned int protoCount = readVarInt(data, size, &offset);
    Proto** protos = luaM_new(L, Proto*, sizeof(Proto*) * protoCount, L->activememcat);

    for (unsigned int i = 0; i < protoCount; ++i)
    {
        Proto* p = luaF_newproto(L);
        p->source = source;

        p->maxstacksize = read_uint8_t(data, size, &offset);
        p->numparams = read_uint8_t(data, size, &offset);
        p->nups = read_uint8_t(data, size, &offset);
        p->is_vararg = read_uint8_t(data, size, &offset);

        p->sizecode = readVarInt(data, size, &offset);
        p->code = luaM_newarray(L, p->sizecode, Instruction, p->memcat);
        for (int j = 0; j < p->sizecode; ++j)
            p->code[j] = read_uint32_t(data, size, &offset);

        p->sizek = readVarInt(data, size, &offset);
        p->k = luaM_newarray(L, p->sizek, TValue, p->memcat);

#ifdef HARDMEMTESTS
        // this is redundant during normal runs, but resolveImportSafe can trigger GC checks under HARDMEMTESTS
        // because p->k isn't fully formed at this point, we pre-fill it with nil to make subsequent setup safe
        for (int j = 0; j < p->sizek; ++j)
        {
            setnilvalue(&p->k[j]);
        }
#endif

        for (int j = 0; j < p->sizek; ++j)
        {
            switch (read_uint8_t(data, size, &offset))
            {
            case LBC_CONSTANT_NIL:
                setnilvalue(&p->k[j]);
                break;

            case LBC_CONSTANT_BOOLEAN:
            {
                uint8_t v = read_uint8_t(data, size, &offset);
                setbvalue(&p->k[j], v);
                break;
            }

            case LBC_CONSTANT_NUMBER:
            {
                double v = read_double(data, size, &offset);
                setnvalue(&p->k[j], v);
                break;
            }

            case LBC_CONSTANT_STRING:
            {
                TString* v = readString(strings, data, size, &offset);
                setsvalue2n(L, &p->k[j], v);
                break;
            }

            case LBC_CONSTANT_IMPORT:
            {
                uint32_t iid = read_uint32_t(data, size, &offset);
                resolveImportSafe(L, envt, p->k, iid);
                setobj(L, &p->k[j], L->top - 1);
                L->top--;
                break;
            }

            case LBC_CONSTANT_TABLE:
            {
                int keys = readVarInt(data, size, &offset);
                Table* h = luaH_new(L, 0, keys);
                for (int ikey = 0; ikey < keys; ++ikey)
                {
                    int key = readVarInt(data, size, &offset);
                    TValue* val = luaH_set(L, h, &p->k[key]);
                    setnvalue(val, 0.0);
                }
                sethvalue(L, &p->k[j], h);
                break;
            }

            case LBC_CONSTANT_CLOSURE:
            {
                uint32_t fid = readVarInt(data, size, &offset);
                Closure* cl = luaF_newLclosure(L, protos[fid]->nups, envt, protos[fid]);
                cl->preload = (cl->nupvalues > 0);
                setclvalue(L, &p->k[j], cl);
                break;
            }

            default:
                LUAU_ASSERT(!"Unexpected constant kind");
            }
        }

        p->sizep = readVarInt(data, size, &offset);
        p->p = luaM_newarray(L, p->sizep, Proto*, p->memcat);
        for (int j = 0; j < p->sizep; ++j)
        {
            uint32_t fid = readVarInt(data, size, &offset);
            p->p[j] = protos[fid];
        }

        p->debugname = readString(strings, data, size, &offset);

        uint8_t lineinfo = read_uint8_t(data, size, &offset);

        if (lineinfo)
        {
            p->linegaplog2 = read_uint8_t(data, size, &offset);

            int intervals = ((p->sizecode - 1) >> p->linegaplog2) + 1;
            int absoffset = (p->sizecode + 3) & ~3;

            p->sizelineinfo = absoffset + intervals * sizeof(int);
            p->lineinfo = luaM_newarray(L, p->sizelineinfo, uint8_t, p->memcat);
            p->abslineinfo = (int*)(p->lineinfo + absoffset);

            uint8_t lastoffset = 0;
            for (int j = 0; j < p->sizecode; ++j)
            {
                lastoffset += read_uint8_t(data, size, &offset);
                p->lineinfo[j] = lastoffset;
            }

            int lastLine = 0;
            for (int j = 0; j < intervals; ++j)
            {
                lastLine += read_int32_t(data, size, &offset);
                p->abslineinfo[j] = lastLine;
            }
        }

        uint8_t debuginfo = read_uint8_t(data, size, &offset);

        if (debuginfo)
        {
            p->sizelocvars = readVarInt(data, size, &offset);
            p->locvars = luaM_newarray(L, p->sizelocvars, LocVar, p->memcat);

            for (int j = 0; j < p->sizelocvars; ++j)
            {
                p->locvars[j].varname = readString(strings, data, size, &offset);
                p->locvars[j].startpc = readVarInt(data, size, &offset);
                p->locvars[j].endpc = readVarInt(data, size, &offset);
                p->locvars[j].reg = read_uint8_t(data, size, &offset);
            }

            p->sizeupvalues = readVarInt(data, size, &offset);
            p->upvalues = luaM_newarray(L, p->sizeupvalues, TString*, p->memcat);

            for (int j = 0; j < p->sizeupvalues; ++j)
            {
                p->upvalues[j] = readString(strings, data, size, &offset);
            }
        }

        protos[i] = p;
    }

    // "main" proto is pushed to Lua stack
    uint32_t mainid = readVarInt(data, size, &offset);
    Proto* main = protos[mainid];

    luaC_checkthreadsleep(L);

    Closure* cl = luaF_newLclosure(L, 0, envt, main);
    setclvalue(L, L->top, cl);
    incr_top(L);

    luaM_free(L, strings, sizeof(TString*) * stringCount, L->activememcat);
    luaM_free(L, protos, sizeof(Proto*) * protoCount, L->activememcat);

    L->global->GCthreshold = GCthreshold;

    return 0;
}
