#include "LuaVm.h"
#include "core/Engine.h"
#include "core/Prune.h"

#include <cmath>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

namespace symcirc {

// ---------------------------------------------------------------------------
namespace {
const char* kSelfKey = "symcirc.vm";

int l_mag(lua_State* L) {
    auto* vm = LuaVm::self(L);
    double f = luaL_checknumber(L, 1);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet -- run the analysis first");
    lua_pushnumber(L, syms::mag_db_at(*vm->result(), 2 * M_PI * f));
    return 1;
}

int l_phase(lua_State* L) {
    auto* vm = LuaVm::self(L);
    double f = luaL_checknumber(L, 1);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet -- run the analysis first");
    lua_pushnumber(L, syms::phase_deg_at(*vm->result(), 2 * M_PI * f));
    return 1;
}

int l_H(lua_State* L) {
    auto* vm = LuaVm::self(L);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet");
    lua_pushstring(L, vm->result()->pruned.text.c_str());
    return 1;
}

int l_Hpoly(lua_State* L) {
    auto* vm = LuaVm::self(L);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet");
    lua_pushstring(L, vm->result()->pruned.text_poly.c_str());
    return 1;
}

int l_report(lua_State* L) {
    auto* vm = LuaVm::self(L);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet");
    lua_pushstring(L, vm->result()->report.c_str());
    return 1;
}

int l_latex(lua_State* L) {
    auto* vm = LuaVm::self(L);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet");
    lua_pushstring(L, vm->result()->pruned.latex.c_str());
    return 1;
}

int l_roots(lua_State* L) {
    auto* vm = LuaVm::self(L);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet");
    const char* which = luaL_optstring(L, 1, "poles");
    const auto& vec = std::string(which) == "zeros"
                          ? vm->result()->pruned.zeros
                          : vm->result()->pruned.poles;
    lua_newtable(L);
    for (size_t i = 0; i < vec.size(); ++i) {
        lua_newtable(L);
        lua_pushnumber(L, vec[i].omega);
        lua_setfield(L, -2, "w");
        lua_pushnumber(L, vec[i].tau);
        lua_setfield(L, -2, "tau");
        lua_pushboolean(L, vec[i].real_root);
        lua_setfield(L, -2, "real");
        lua_pushnumber(L, vec[i].q);
        lua_setfield(L, -2, "q");
        lua_pushstring(L, vec[i].label.c_str());
        lua_setfield(L, -2, "label");
        lua_pushstring(L, vec[i].factor_text.c_str());
        lua_setfield(L, -2, "factor");
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}

int l_estimate(lua_State* L) {
    auto* vm = LuaVm::self(L);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet");
    const char* name = luaL_checkstring(L, 1);
    const auto& est = vm->result()->params.est;
    auto it = est.find(name);
    if (it == est.end()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, it->second);
    return 1;
}

int l_dropped(lua_State* L) {
    auto* vm = LuaVm::self(L);
    if (!vm || !vm->result())
        return luaL_error(L, "no analysis result yet");
    lua_newtable(L);
    int n = 0;
    for (const auto& d : vm->result()->pruned.dropped) {
        lua_newtable(L);
        lua_pushstring(L, d.location.c_str());
        lua_setfield(L, -2, "location");
        lua_pushstring(L, d.term.c_str());
        lua_setfield(L, -2, "term");
        lua_pushnumber(L, d.db_rel);
        lua_setfield(L, -2, "db");
        lua_rawseti(L, -2, lua_Integer(++n));
    }
    return 1;
}
} // namespace

// ---------------------------------------------------------------------------
LuaVm::LuaVm() {
    L_ = luaL_newstate();
    luaL_openlibs(L_);

    // stash `this` where self() can find it
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, kSelfKey);

    // capture print()
    lua_pushcfunction(L_, &LuaVm::l_print);
    lua_setglobal(L_, "print");

    push_api();
}

LuaVm::~LuaVm() {
    if (L_) lua_close(L_);
}

LuaVm* LuaVm::self(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSelfKey);
    auto* vm = static_cast<LuaVm*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return vm;
}

void LuaVm::push_api() {
    struct Entry { const char* name; lua_CFunction fn; };
    const Entry entries[] = {
        {"mag", l_mag},        {"phase", l_phase},
        {"H", l_H},            {"Hpoly", l_Hpoly},
        {"latex", l_latex},    {"report", l_report},
        {"roots", l_roots},    {"estimate", l_estimate},
        {"dropped", l_dropped},
    };
    for (const auto& e : entries) {
        lua_pushcfunction(L_, e.fn);
        lua_setglobal(L_, e.name);
    }
}

int LuaVm::l_print(lua_State* L) {
    int n = lua_gettop(L);
    std::string line;
    lua_getglobal(L, "tostring");
    for (int i = 1; i <= n; ++i) {
        lua_pushvalue(L, -1);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        line += lua_tostring(L, -1);
        if (i < n) line += "\t";
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    LuaVm* vm = self(L);
    if (vm) {
        if (vm->sink_) vm->sink_(line + "\n");
        else vm->output_ += line + "\n";
    }
    return 0;
}

bool LuaVm::run(const std::string& code, std::string& err) {
    if (luaL_loadbuffer(L_, code.data(), code.size(), "console") != LUA_OK) {
        err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    // run with a message handler for tracebacks
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

std::string LuaVm::take_output() {
    std::string s = std::move(output_);
    output_.clear();
    return s;
}

} // namespace symcirc
