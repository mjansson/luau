// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lobject.h"

#define sizeCclosure(n) (offsetof(Closure, c.upvals) + sizeof(TValue) * (n))
#define sizeLclosure(n) (offsetof(Closure, l.uprefs) + sizeof(TValue) * (n))

struct lua_State;
struct lua_Page;

LUAI_FUNC Proto* luaF_newproto(struct lua_State* L);
LUAI_FUNC Closure* luaF_newLclosure(struct lua_State* L, int nelems, Table* e, Proto* p);
LUAI_FUNC Closure* luaF_newCclosure(struct lua_State* L, int nelems, Table* e);
LUAI_FUNC UpVal* luaF_findupval(struct lua_State* L, StkId level);
LUAI_FUNC void luaF_close(struct lua_State* L, StkId level);
LUAI_FUNC void luaF_closeupval(struct lua_State* L, UpVal* uv, int dead);
LUAI_FUNC void luaF_freeproto(struct lua_State* L, Proto* f, struct lua_Page* page);
LUAI_FUNC void luaF_freeclosure(struct lua_State* L, Closure* c, struct lua_Page* page);
LUAI_FUNC void luaF_freeupval(struct lua_State* L, UpVal* uv, struct lua_Page* page);
LUAI_FUNC const LocVar* luaF_getlocal(const Proto* func, int local_number, int pc);
LUAI_FUNC const LocVar* luaF_findlocal(const Proto* func, int local_reg, int pc);
