#pragma once
#include <lua.hpp>

#include <functional>
#include <string>

namespace syms {
struct AnalysisResult;
}

namespace symcirc {

// Minimal Lua VM for the console tab and user scripting.
// Registered API (all read the most recent AnalysisResult):
//   mag(f_hz) -> dB          mag of unpruned H
//   phase(f_hz) -> deg       phase of unpruned H
//   H() -> string            factored low-entropy form
//   Hpoly() -> string        expanded pruned form
//   report() -> string       full report text
//   poles() -> { {w=, label=}, ... }
//   zeros() -> { ... }
//   estimate(name) -> number numeric estimate of a component/param
//   print(...)               prints to the console output
class LuaVm {
public:
    LuaVm();
    ~LuaVm();

    LuaVm(const LuaVm&) = delete;
    LuaVm& operator=(const LuaVm&) = delete;

    void set_result(const syms::AnalysisResult* r) { res_ = r; }
    const syms::AnalysisResult* result() const { return res_; }

    // Runs a chunk. Returns false and fills `err` on syntax/runtime error.
    bool run(const std::string& code, std::string& err);

    // Console output produced by print() since the last call.
    std::string take_output();

    // stdout/stderr of the hosted process redirected here too (best effort).
    void set_sink(std::function<void(const std::string&)> sink) {
        sink_ = std::move(sink);
    }

    // Used by the registered C functions to recover the VM instance.
    static LuaVm* self(lua_State* L);

private:
    void push_api();

    lua_State* L_ = nullptr;
    const syms::AnalysisResult* res_ = nullptr;
    std::string output_;
    std::function<void(const std::string&)> sink_;

    static int l_print(lua_State* L);
};

} // namespace symcirc
