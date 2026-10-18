#!/usr/bin/env bash
# Pull request gate: builds and runs the unit tests that need neither the full GPU library
# (tlggpu), SDL3/FFmpeg, game files nor a GPU. Flags mirror the targets in port/gpu/CMakeLists.txt
# and `build.sh --test`. Every test runs; the script fails at the end if any of them failed.
#
# Needs: gcc-14/g++-14, python3, libboost-dev, libfmt-dev, libvulkan-dev, mesa-vulkan-drivers
# (lavapipe, for async-pipeline-cache) and the vulkan-headers + magic_enum submodules.
set -uo pipefail
cd -- "$(dirname -- "$0")/../../port"
CC=${CC:-gcc-14}
CXX=${CXX:-g++-14}
out=${TEST_OUT:-out/unit-tests}
mkdir -p "$out"
out=$(cd "$out" && pwd)
passed=() failed=()

# run NAME COMMAND...: one test step, its output kept in $out/NAME.log.
run() {
    local name=$1; shift
    if "$@" >"$out/$name.log" 2>&1; then
        passed+=("$name"); echo "ok    $name"
    else
        failed+=("$name"); echo "FAIL  $name"; sed 's/^/    /' "$out/$name.log" | tail -40
    fi
}

# --- C tests (build.sh --test) ---------------------------------------------------------------
cflags=(-std=c11 -O2 -g -Wall -Wextra -Werror)
c_test() {  # c_test NAME "ARGS..." SOURCES/FLAGS...
    local name=$1 args=$2; shift 2
    # shellcheck disable=SC2086
    run "$name" bash -c '"$@" && '"$out/$name $args" _ "$CC" "${cflags[@]}" "$@" -o "$out/$name"
}
run isa-objects bash -c "$CC ${cflags[*]} -mgeneral-regs-only -c src/isa_emulate.c -o $out/isa_emulate.o &&
    $CC ${cflags[*]} -I. -Isrc -c src/isa_rewrite.c -o $out/isa_rewrite.o"
isa=("$out/isa_rewrite.o" "$out/isa_emulate.o")
c_test mouse-stick-test "" tests/test_mouse_stick.c -lm
c_test mouse-touch-test "" -UNDEBUG -Isrc tests/test_mouse_touch.c -lm
c_test rwlock-rp-test "" -pthread -Isrc tests/test_rwlock_rp.c
c_test content-test "" -D_GNU_SOURCE -I. -Isrc tests/test_content.c src/runtime_content.c
c_test services-test "" -pthread -I. -Isrc tests/test_services.c
c_test cpu-check-test "" -UNDEBUG -I. -Isrc tests/test_cpu_check.c
c_test tsc-clock-test "" -UNDEBUG -pthread -I. -Isrc tests/test_tsc_clock.c
c_test isa-emulate-test "" -mbmi -mlzcnt -mmovbe -I. -Isrc tests/test_isa_emulate.c "$out/isa_emulate.o"
c_test isa-stub-test "tests/data/isa_fixture.bin" -mbmi -mlzcnt -I. -Isrc tests/test_isa_stub.c "${isa[@]}"
c_test red-zone-test "tests/data/red_zone_image.bin tests/data/red_zone_table.bin" \
    -I. -Isrc tests/test_red_zone.c "${isa[@]}"
c_test isa-sse4a-test "" -msse4a -I. -Isrc tests/test_isa_sse4a.c "${isa[@]}"
c_test isa-f16c-test "" -mavx -mf16c -I. -Isrc tests/test_isa_f16c.c "${isa[@]}"

# --- C++ tests (port/gpu/CMakeLists.txt, RelWithDebInfo) -------------------------------------
# Paths are relative to port/gpu, as in CMake; the tests run from port/.
cxxflags=(-std=gnu++23 -O2 -g -fno-omit-frame-pointer -pthread -UNDEBUG)
strict=(-Wall -Wextra -Werror)
gpu_inc=(-Ishim -Ishadps4 -Ithird_party/gcn/include -Ithird_party/magic_enum/include
         -Ithird_party/magic_enum/include/magic_enum -isystem third_party/vulkan-headers/include)
cxx_test() {  # cxx_test NAME SOURCES/FLAGS...
    local name=$1; shift
    run "$name" bash -c 'cd gpu && "$@" && cd .. && '"$out/$name" _ \
        "$CXX" "${cxxflags[@]}" "$@" -o "$out/$name"
}
cxx_test motion-history-test "${strict[@]}" -I.. ../tests/test_motion_history.cpp
cxx_test ui-composition-test "${strict[@]}" -I.. ../tests/test_ui_composition.cpp
cxx_test async-compiler-test -Ishadps4 ../tests/test_async_compiler.cpp
cxx_test wait-diagnostics-test -Ishadps4 ../tests/test_wait_diagnostics.cpp
for name in warmup-inbox wave-stats pass-census bc-decode amd-paths readback-queue; do
    cxx_test "$name-test" "${strict[@]}" -Ishadps4 "../tests/test_${name//-/_}.cpp"
done
cxx_test lighting-test-knob-test "${strict[@]}" -Ishadps4 ../tests/test_lighting_test.cpp
# Ubuntu's _FORTIFY_SOURCE marks fread warn_unused_result; the test ignores it on purpose.
cxx_test readback-sources-test "${strict[@]}" -Wno-error=unused-result -Ishadps4 ../tests/test_readback_sources.cpp
cxx_test lru-cache-test "${strict[@]}" -I../tests/stubs -Ishim -Ishadps4 ../tests/test_lru_cache.cpp
cxx_test page-state-test "${strict[@]}" -I../tests/stubs -Ishim -Ishadps4 ../tests/test_page_state.cpp
cxx_test bit-array-test "${strict[@]}" -mavx2 -I../tests/stubs -Ishim -Ishadps4 ../tests/test_bit_array.cpp
cxx_test upscaler-support-test -Ishim ../tests/test_upscaler_support.cpp shim/tlg_settings.cpp
cxx_test stat-multipliers-test -Ishim ../tests/test_stat_multipliers.cpp shim/tlg_settings.cpp
cxx_test aliasing-test "${gpu_inc[@]}" ../tests/test_aliasing.cpp \
    shadps4/video_core/texture_cache/aliasing.cpp shadps4/video_core/texture_cache/host_compatibility.cpp -lfmt
cxx_test blend-rewrite-test "${gpu_inc[@]}" ../tests/test_blend_rewrite.cpp -lfmt
cxx_test depth-stencil-state-test "${gpu_inc[@]}" ../tests/test_depth_stencil_state.cpp -lfmt
cxx_test frame-capture-test "${gpu_inc[@]}" ../tests/test_frame_capture.cpp \
    shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp -lfmt
cxx_test no-msaa-test "${gpu_inc[@]}" ../tests/test_no_msaa.cpp shadps4/video_core/amdgpu/pixel_format.cpp -lfmt
cxx_test async-fetch-snapshot-test "${gpu_inc[@]}" ../tests/test_async_fetch_snapshot.cpp \
    shadps4/shader_recompiler/frontend/fetch_shader.cpp shadps4/shader_recompiler/frontend/decode.cpp \
    shadps4/shader_recompiler/frontend/format.cpp -lfmt
# Creates a real Vulkan instance: lavapipe (software) on the runner.
cxx_test async-pipeline-cache-test -Ishadps4 -isystem third_party/vulkan-headers/include \
    ../tests/test_async_pipeline_cache.cpp -lvulkan

# --- Python tests (tests/test_*.py, unittest) ------------------------------------------------
# New test modules are picked up automatically. These are known broken on Linux today and are
# skipped until fixed (leftovers from the port this one was forked from, or Windows-only):
#   test_launcher_stream_memory, test_upscaler_assets: import modules that no longer exist
#   test_patches: expects patches for another game; test_open_external, test_qt_launcher,
#   test_qt_screens_a: assertions out of date with the launcher.
skip=" test_launcher_stream_memory test_upscaler_assets test_patches test_open_external test_qt_launcher test_qt_screens_a "
for file in tests/test_*.py; do
    module=$(basename "$file" .py)
    [[ $skip == *" $module "* ]] && continue
    run "$module" bash -c "cd tests && python3 -m unittest $module"
done

echo
echo "${#passed[@]} passed, ${#failed[@]} failed"
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
    {
        echo "### Unit tests: ${#passed[@]} passed, ${#failed[@]} failed"
        for name in "${failed[@]}"; do echo "- :x: \`$name\`"; done
    } >> "$GITHUB_STEP_SUMMARY"
fi
[[ ${#failed[@]} -eq 0 ]]
