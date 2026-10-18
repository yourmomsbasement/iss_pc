// SPDX-License-Identifier: GPL-2.0-or-later
// GetProgram's variant reuse (TLG_VARIANT_REUSE) skips the specialization when its inputs are
// unchanged. The flattened user data holds the fetch shader's address only: a fetch shader
// replaced in place gives another specialization with the same user data, so the reuse also
// compares the fetch shader's code.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include "shader_recompiler/specialization.h"
#include "common/logging/log.h"

extern "C" { uint64_t runtime_disabled_optimizations = 0; }
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
namespace Common::Log {
bool ShouldLog(Level) { return false; }
void Write(std::string_view, Level, const char*, int, const char*, std::string&&) {}
}

int main() {
    using namespace Shader;
    // s_load_dwordx4 s[8:11], s[2:3], 0; tbuffer_load_format_xyzw v[0:3]; s_setpc_b64.
    std::array<u32, 4> code{0xC0000000u | (2u << 22) | (8u << 15) | (1u << 9) | (1u << 8),
                            0xE8000000u | (3u << 16), 2u << 16, 0xBE802000u};
    AmdGpu::Buffer buffer{};
    buffer.base_address = 0x12340;
    buffer.stride = 16;
    std::array<u32, 4> ud{};
    const auto* code_pointer = code.data();
    const auto* buffer_pointer = &buffer;
    std::memcpy(ud.data(), &code_pointer, sizeof(code_pointer));
    std::memcpy(ud.data() + 2, &buffer_pointer, sizeof(buffer_pointer));
    Info info;
    info.hw_stage = HwStage::Vertex;
    info.sw_stage = SwStage::Vertex;
    info.has_fetch_shader = true;
    info.fetch_shader_sgpr_base = 0;
    info.user_data = ud;
    const RuntimeInfo runtime_info{};
    const Profile profile{};

    const StageSpecialization before(info, runtime_info, profile, {});
    assert(before.fetch_shader_data && before.fetch_shader_data->size == code.size() * sizeof(u32));
    const auto saved = code;

    // Same address and user data, same V#; the load now writes v[8:11].
    code[2] = (2u << 16) | (8u << 8);
    const StageSpecialization after(info, runtime_info, profile, {});
    assert(after.fetch_shader_data->attributes[0].GetSharp(info).base_address ==
           before.fetch_shader_data->attributes[0].GetSharp(info).base_address);
    assert(!(before == after));
    // What the reuse compares: the code words the stored variant was parsed from.
    assert(std::memcmp(code.data(), saved.data(), before.fetch_shader_data->size) != 0);

    code = saved;
    const StageSpecialization again(info, runtime_info, profile, {});
    assert(before == again);
}
