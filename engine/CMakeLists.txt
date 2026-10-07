cmake_minimum_required(VERSION 3.28)
project(lomn LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pinned third-party inputs are fetched by tools/toolchain.sh into .toolchain/ (see deps.lock).
set(LOMN_TOOLCHAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.toolchain" CACHE PATH "pinned toolchain root")
option(LOMN_WEBGPU "build the WebGPU backend" ON)
# M1b: the browser viewer's initial wasm memory (bytes, a multiple of 64 KiB). Measured 2026-10-07 (REF4 REPORT "Memory
# budget", Chromium/Metal, 16 MiB start): boot set resident 16 MiB (no growth), after lev1/vllg 16, lev2/ghut 19.25,
# lev4/crss 40, lev0/frnt (largest texture set, 57.0 MB with mips) 83.06 MiB = the high-water mark; a sweep of all 122
# browser-addressable areas afterwards never grew it. 96 MiB = that mark + 15.6% headroom, so growth (a heap copy without
# pthreads, spec §4.6) does not happen in normal use; MAXIMUM_MEMORY stays 2 GB for anything larger.
set(LOMN_WEB_INITIAL_MEMORY 100663296)
enable_testing()

add_library(lomn_core STATIC src/fs/memory_fs.cpp src/fs/prefetch_fs.cpp src/rhi/null_device.cpp src/app/loop.cpp)
target_include_directories(lomn_core PUBLIC include PRIVATE "${LOMN_TOOLCHAIN_DIR}/include")
target_compile_options(lomn_core PRIVATE -Wall -Wextra)
# Native clang fuses a*b+c into FMA by default and wasm never does; forbid it so native == wasm32 bit for bit.
# PUBLIC: every target linking lomn_core inherits it.
target_compile_options(lomn_core PUBLIC -ffp-contract=off)

# M1a-1 data path (no GPU): SAGE blockfiles, lookup chain, SLB tables, binary .x meshes, DDS/TGA decoders.
add_library(lomn_sage STATIC src/sage/blockfile.cpp src/sage/blockchain.cpp src/sage/slb.cpp src/sage/xfile.cpp src/sage/image.cpp
            src/sage/osi.cpp)
target_link_libraries(lomn_sage PUBLIC lomn_core)
target_compile_options(lomn_sage PRIVATE -Wall -Wextra)

# M1a-3 faithful states: the D3D8 fixed-function state shim (state log) and its compiler to rhi (uber-shader + pipeline
# keys). D3D-convention matrices (view/math) live here so the shim can hold transforms.
add_library(lomn_d3d STATIC src/view/math.cpp src/d3d/device8.cpp src/d3d/ffp.cpp)
target_link_libraries(lomn_d3d PUBLIC lomn_core)
target_compile_options(lomn_d3d PRIVATE -Wall -Wextra)

# M1a-2 first pixels + M1a-3: CPU mips, area loader, the SAGE frame replay on the shim, the A7/A9 synthetic scene.
add_library(lomn_area STATIC src/view/mips.cpp src/view/area.cpp src/view/sage_frame.cpp src/view/synthetic.cpp
            src/view/fly_camera.cpp src/view/prefetch.cpp)  # M1b: free-fly camera, prefetch helpers
target_link_libraries(lomn_area PUBLIC lomn_sage lomn_d3d)
target_compile_options(lomn_area PRIVATE -Wall -Wextra)

if(EMSCRIPTEN)
  set(EMDAWN_PORT "--use-port=${LOMN_TOOLCHAIN_DIR}/emdawnwebgpu_pkg/emdawnwebgpu.port.py")
else()
  target_sources(lomn_core PRIVATE src/fs/disk_fs.cpp third_party/stb_impl.cpp)
endif()
# stb_image_write.h is third-party code with ~9 -Wmissing-field-initializers warnings; silence only that TU.
set_source_files_properties(third_party/stb_impl.cpp PROPERTIES COMPILE_OPTIONS -w)

if(LOMN_WEBGPU)
  add_library(lomn_webgpu STATIC src/rhi/webgpu_device.cpp)
  target_compile_definitions(lomn_webgpu PUBLIC LOMN_HAS_WEBGPU=1)
  target_link_libraries(lomn_webgpu PUBLIC lomn_core)
  if(EMSCRIPTEN)
    target_compile_options(lomn_webgpu PUBLIC ${EMDAWN_PORT})
    target_link_options(lomn_webgpu PUBLIC ${EMDAWN_PORT})
  else()
    find_package(Threads REQUIRED)   # Dawn's exported targets need Threads::Threads to exist first
    find_package(Dawn REQUIRED PATHS "${LOMN_TOOLCHAIN_DIR}/dawn" NO_DEFAULT_PATH)
    target_link_libraries(lomn_webgpu PUBLIC dawn::webgpu_dawn)
  endif()
endif()

if(EMSCRIPTEN)
  if(LOMN_WEBGPU)
    # M1b viewer: an ES module factory (web/app/engine.js imports it); INVOKE_RUN=0 so the boot set is prefetched into
    # the heap before main(). INITIAL_MEMORY is set from measurement (REF4 REPORT "Memory budget").
    # Optimisation (REF4 r2 decision D14, amends parent spec §4.6 "-O2 + wasm-opt"): the Release build (-O3; emcc runs
    # wasm-opt at link for both levels) measured against -O2, 4 interleaved reps per arm (web/tests/measure_arms.mjs,
    # loadavg 13-20): load of 4 areas median 613 vs 605 ms (spread 236 / 525 ms), frame CPU p50 0.7 vs 0.7 ms (spread
    # 0.1): indistinguishable; wasm 332,641 vs 344,390 B, the only difference beyond the (zero) null spread: -O3 kept.
    # -sDYNAMIC_EXECUTION=0: no eval/new Function in the glue (the page's CSP allows only 'wasm-unsafe-eval').
    add_executable(lomn_web src/app/main_web.cpp)
    target_link_libraries(lomn_web PRIVATE lomn_area lomn_webgpu)
    target_include_directories(lomn_web PRIVATE src/app)
    # The measured configuration, readable at runtime (lomn_build_info -> window.__lomn.metrics.build and the HUD).
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _lomn_bt)
    string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_lomn_bt}}" _lomn_flags)
    target_compile_definitions(lomn_web PRIVATE "LOMN_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"" "LOMN_BUILD_FLAGS=\"${_lomn_flags}\"")
    target_link_options(lomn_web PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB -sINITIAL_MEMORY=${LOMN_WEB_INITIAL_MEMORY}
                        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLomn -sINVOKE_RUN=0 -sENVIRONMENT=web -sDYNAMIC_EXECUTION=0
                        "SHELL:-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
                        "SHELL:-sEXPORTED_RUNTIME_METHODS=callMain,UTF8ToString,stringToNewUTF8,HEAPU8")
  endif()
  # Null-backend runner under Node: its trace/calls must be byte-identical to the native runner's.
  add_executable(lomn_native src/app/main_native.cpp)
  target_link_libraries(lomn_native PRIVATE lomn_core)
  target_link_options(lomn_native PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1)
  add_test(NAME determinism_null COMMAND ${CMAKE_COMMAND} -DRUNNER=node -DEXE=$<TARGET_FILE:lomn_native>
           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/determinism -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/determinism.cmake)
  # M1a-2 A7 under Node: the same synthetic scene's null call log must equal the same checked-in expected log, so the
  # native and wasm32 logs are byte-identical (make test-wasm also cmp's the two files directly).
  # M1a-3: lomn_view and lomn_datacheck under Node read a real install through the disk file system (NODERAWFS), so
  # real-area null logs and the datacheck run natively AND as wasm32 (M1a-1 carry-over R1).
  add_executable(lomn_view tools/lomn_view.cpp src/fs/disk_fs.cpp)
  target_link_libraries(lomn_view PRIVATE lomn_area)
  target_include_directories(lomn_view PRIVATE src/app)
  target_compile_definitions(lomn_view PRIVATE LOMN_NODE_DISK_FS=1)
  target_link_options(lomn_view PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB)
  add_executable(lomn_datacheck tools/lomn_datacheck.cpp src/fs/disk_fs.cpp)
  target_link_libraries(lomn_datacheck PRIVATE lomn_sage)
  target_compile_definitions(lomn_datacheck PRIVATE LOMN_NODE_DISK_FS=1)
  target_link_options(lomn_datacheck PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB)
  file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lomn_datacheck_node.sh
       CONTENT "#!/bin/sh\nexec node \"$<TARGET_FILE:lomn_datacheck>\" \"$@\"\n" FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  add_test(NAME a7_null_calls COMMAND ${CMAKE_COMMAND} -DRUNNER=node -DEXE=$<TARGET_FILE:lomn_view> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/a7
           -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a7_calls.txt -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a7_calls.cmake)
  # M1a-3 A9 under Node: the synthetic state scene's D3D8 state log equals the same checked-in expected log.
  add_test(NAME a9_state_log COMMAND ${CMAKE_COMMAND} -DRUNNER=node -DEXE=$<TARGET_FILE:lomn_view> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/a9
           -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a9_state.txt -DEXPECTED_F2=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a9_state_f2.txt
           -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a9_state.cmake)
  # Task 8B E2: --write-install is native-only: under Node it is a usage error (exit 2) that writes nothing.
  add_test(NAME view_cli_write_install_wasm COMMAND sh -c
           "rm -rf \"$1\"; node \"$0\" --synthetic 2 --backend null --out \"$1.txt\" --write-install \"$1\" >/dev/null 2>&1; rc=$?; [ $rc -eq 2 ] || { echo \"rc=$rc (want 2)\"; exit 1; }; [ ! -e \"$1\" ] || { echo 'install written'; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_write_install_wasm)
else()
  add_executable(lomn_native src/app/main_native.cpp)
  target_link_libraries(lomn_native PRIVATE $<IF:$<BOOL:${LOMN_WEBGPU}>,lomn_webgpu,lomn_core>)
  target_compile_definitions(lomn_native PRIVATE $<$<BOOL:${LOMN_WEBGPU}>:LOMN_HAS_WEBGPU=1>)
  add_test(NAME determinism_null COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:lomn_native>
           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/determinism -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/determinism.cmake)
  # Native-only data survey over a real install (LOMN_DATA); validated by tools/research/oracle/check_datacheck.py.
  add_executable(lomn_datacheck tools/lomn_datacheck.cpp)
  target_link_libraries(lomn_datacheck PRIVATE lomn_sage)
  target_compile_options(lomn_datacheck PRIVATE -Wall -Wextra)
  # M1a-2: lomn_view renders an area to PNG (webgpu) or a call log (null); --synthetic 1 is the A7 scene.
  add_executable(lomn_view tools/lomn_view.cpp)
  target_link_libraries(lomn_view PRIVATE lomn_area $<$<BOOL:${LOMN_WEBGPU}>:lomn_webgpu>)
  target_include_directories(lomn_view PRIVATE src/app "${LOMN_TOOLCHAIN_DIR}/include")
  target_compile_options(lomn_view PRIVATE -Wall -Wextra)
  # REF3 revision 2: the CPU reference of the D3D8 fixed-function formulas (the GPU probe's expectation) against an
  # independent Python implementation from the D3D8 documentation, on gpu_ffp's cases + 4000 seeded random samples.
  add_executable(lomn_ffp_cases tests/ffp_cases.cpp)
  target_link_libraries(lomn_ffp_cases PRIVATE lomn_d3d)
  add_test(NAME ffp_oracle COMMAND sh -c "\"$0\" 4000 1 > \"$1\" && timeout 300 python3 \"$2\" \"$1\""
           $<TARGET_FILE:lomn_ffp_cases> ${CMAKE_CURRENT_BINARY_DIR}/ffp_cases.jsonl ${CMAKE_CURRENT_SOURCE_DIR}/../tools/research/oracle/ffp_oracle.py)
  # A7 contract (no game data): the synthetic scene's null call log must equal the checked-in expected log, twice.
  add_test(NAME a7_null_calls COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:lomn_view> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/a7
           -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a7_calls.txt -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a7_calls.cmake)
  # M1a-3 A9 (no game data): the synthetic state scene's D3D8 state log equals the checked-in expected log, twice.
  add_test(NAME a9_state_log COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:lomn_view> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/a9
           -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a9_state.txt -DEXPECTED_F2=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a9_state_f2.txt
           -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a9_state.cmake)
  # A9 against the independent Python oracle (tools/research/oracle/a9_oracle.py, written from the decompiled code):
  # the synthetic state scene (written out as an install) always; vllg/ghut/crss when LOMN_DATA is set (else 77 = skip),
  # plus lev2/vllg and lev3/blr2 (Task 8A E1: Trans nodes with "sky1" and "watr"), lev1/bech (E2a: a TFACTOR alpha that
  # rounds differently in float32 and float64) and fren/frnt (E2b: no POS table). The full 123-area sweep is make a9-sweep.
  # Task 8A E2b: then the same area without its POS table (blk_without.py): make_camera's bounds-centre eye fallback.
  add_test(NAME a9_oracle_synthetic COMMAND sh -c
           "rm -rf \"$1\" && \"$0\" --synthetic 2 --backend null --out \"$1/c.txt\" --state-log \"$1/s.txt\" --write-install \"$1/inst\" >/dev/null && timeout 120 python3 \"$2\" --install \"$1/inst\" --level levt --area stat --eye-height 0 --compare \"$1/s.txt\" || exit 1; b=\"$1/inst/data/levels/levt/stat.blk\"; timeout 60 python3 \"$3\" \"$b\" \"$b.nopos\" stat_pos.slb && mv \"$b.nopos\" \"$b\" && \"$0\" --install \"$1/inst\" --level levt --area stat --eye-height 0 --backend null --out \"$1/c2.txt\" --state-log \"$1/s2.txt\" >/dev/null && grep -q '\"eye_source\":\"bounds_centre\"' \"$1/c2.txt.json\" && timeout 120 python3 \"$2\" --install \"$1/inst\" --level levt --area stat --eye-height 0 --compare \"$1/s2.txt\""
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/a9_oracle_syn ${CMAKE_CURRENT_SOURCE_DIR}/../tools/research/oracle/a9_oracle.py
           ${CMAKE_CURRENT_SOURCE_DIR}/../tools/research/oracle/blk_without.py)
  add_test(NAME a9_oracle_real COMMAND sh -c
           "if [ -z \"$LOMN_DATA\" ]; then echo 'SKIPPED (no LOMN_DATA)'; exit 77; fi; mkdir -p \"$1\"; for la in lev1:vllg lev2:ghut lev4:crss lev2:vllg lev3:blr2 lev1:bech fren:frnt; do l=\${la%%:*}; a=\${la##*:}; \"$0\" --install \"$LOMN_DATA/install\" --level $l --area $a --backend null --out \"$1/$l.$a.txt\" --state-log \"$1/$l.$a.state.txt\" >/dev/null || exit 1; timeout 120 python3 \"$2\" --install \"$LOMN_DATA/install\" --level $l --area $a --compare \"$1/$l.$a.state.txt\" || exit 1; done"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/a9_oracle_real ${CMAKE_CURRENT_SOURCE_DIR}/../tools/research/oracle/a9_oracle.py)
  set_tests_properties(a9_oracle_real PROPERTIES SKIP_RETURN_CODE 77)
  # M1a-3 Task 8A C1: C++ OsiFile::eval_calls vs the independent Python evaluator (tools/research/osi.py) over EVERY
  # class.method pair of the real Base.osi (8,406 on Beta 1.5.0): 0 mismatches. 77 = skip without LOMN_DATA.
  add_executable(lomn_osi_dump tools/lomn_osi_dump.cpp)
  target_link_libraries(lomn_osi_dump PRIVATE lomn_sage)
  target_compile_options(lomn_osi_dump PRIVATE -Wall -Wextra)
  add_test(NAME osi_all_methods COMMAND sh -c
           "if [ -z \"$LOMN_DATA\" ]; then echo 'SKIPPED (no LOMN_DATA)'; exit 77; fi; b=\"$LOMN_DATA/install/data/Base.osi\"; timeout 120 \"$0\" \"$b\" > \"$1\" || exit 1; timeout 120 python3 \"$2\" \"$b\" --compare-all \"$1\""
           $<TARGET_FILE:lomn_osi_dump> ${CMAKE_CURRENT_BINARY_DIR}/osi_all_methods.txt ${CMAKE_CURRENT_SOURCE_DIR}/../tools/research/osi.py)
  set_tests_properties(osi_all_methods PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME view_cli_dangling_flag COMMAND sh -c
           "\"$0\" --synthetic 1 --backend null --out \"$1\" --fov >/dev/null 2>&1; [ $? -eq 2 ]"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_dangling.txt)
  add_test(NAME view_cli_missing_install COMMAND sh -c
           "\"$0\" --install /nonexistent/lomn --level lev1 --area vllg --backend null --out \"$1\" >/dev/null 2>&1; [ $? -eq 3 ]"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_missing.txt)
  # Task 6 C1: non-finite and out-of-range camera/render floats are usage errors (exit 2), never exit 0 + invalid JSON;
  # edge values pass and --yaw is normalised to (-180, 180].
  add_test(NAME view_cli_nonfinite COMMAND sh -c
           "for a in '--far inf' '--far nan' '--near inf' '--near -1' '--near 0' '--near 5000' '--far 1e8' '--fov inf' '--fov 0' '--fov 180' '--yaw inf' '--yaw nan' '--pitch nan' '--pitch 90' '--pitch -89.95' '--eye nan,0,0' '--eye 0,inf,0' '--eye 2e6,0,0' '--eye-height inf' '--eye-height -2e4' '--lighting on' '--fog maybe' '--alpha-ref 1'; do \"$0\" --synthetic 1 --backend null --out \"$1\" $a >/dev/null 2>&1; rc=$?; if [ $rc -ne 2 ]; then echo \"$a: rc=$rc (want 2)\"; exit 1; fi; done; \"$0\" --synthetic 1 --backend null --out \"$1\" --yaw 400 --pitch 89.9 --far 1e7 --eye-height 1e4 --lighting forced --fog off >/dev/null 2>&1 || { echo 'edge values rejected'; exit 1; }; grep -q '\"yaw_deg\":40.000' \"$1.json\" || { echo 'yaw not normalised'; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_nonfinite.txt)
  # C2: --synthetic / --survey take exactly 1; conflicting modes are usage errors.
  add_test(NAME view_cli_modes COMMAND sh -c
           "for a in '--synthetic 0 --install /nonexistent/lomn --level lev1 --area vllg' '--synthetic yes' '--synthetic 1 --install /nonexistent/lomn' '--synthetic 1 --level lev1' '--synthetic 1 --area vllg' '--survey 0 --install /nonexistent/lomn' '--synthetic 1 --survey 1 --install /nonexistent/lomn'; do \"$0\" --backend null --out \"$1\" $a >/dev/null 2>&1; rc=$?; if [ $rc -ne 2 ]; then echo \"$a: rc=$rc (want 2)\"; exit 1; fi; done"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_modes.txt)
  # C3: integers need a leading digit and only digits (no space, sign, hex); parsed in u64, then range-checked.
  add_test(NAME view_cli_ints COMMAND sh -c
           "for v in ' 5' '+5' '5 ' '0x10' '-1' '' '18446744073709551621'; do \"$0\" --synthetic 1 --backend null --out \"$1\" --width \"$v\" >/dev/null 2>&1; rc=$?; if [ $rc -ne 2 ]; then echo \"--width '$v': rc=$rc (want 2)\"; exit 1; fi; done; \"$0\" --synthetic 1 --backend null --out \"$1\" --frames +2 >/dev/null 2>&1; [ $? -eq 2 ] || { echo '--frames +2 accepted'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$1\" --width 0064 >/dev/null 2>&1 || { echo '--width 0064 rejected'; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_ints.txt)
  # C4: stale --out / --out.json are removed only after argument validation succeeds (a usage error, exit 2, leaves
  # existing outputs untouched), before any install/device/load step (exit 3 removes them), so a failed run never
  # leaves a stale output behind; an --out that cannot be written is exit 4.
  add_test(NAME view_cli_stale_outputs COMMAND sh -c
           "o=\"$1/out.txt\"; rm -rf \"$1\"; mkdir -p \"$1/dir.txt\" || exit 1; printf stale > \"$o\"; printf stale > \"$o.json\"; \"$0\" --install /nonexistent/lomn --level lev1 --area vllg --backend null --out \"$o\" >/dev/null 2>&1; rc=$?; [ $rc -eq 3 ] || { echo \"missing install rc=$rc\"; exit 1; }; [ ! -e \"$o\" ] && [ ! -e \"$o.json\" ] || { echo 'stale output left after exit 3'; exit 1; }; printf stale > \"$o\"; printf stale > \"$o.json\"; \"$0\" --synthetic 1 --backend null --out \"$o\" --fov 0 >/dev/null 2>&1; [ $? -eq 2 ] || { echo 'bad --fov not exit 2'; exit 1; }; [ \"$(cat \"$o\")\" = stale ] && [ \"$(cat \"$o.json\")\" = stale ] || { echo 'exit 2 touched a pre-existing output'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$o\" --synthetic 3 >/dev/null 2>&1; [ $? -eq 2 ] && [ -e \"$o\" ] && [ -e \"$o.json\" ] || { echo 'mode usage error touched a pre-existing output'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$1/dir.txt\" >/dev/null 2>&1; rc=$?; [ $rc -eq 4 ] && [ -d \"$1/dir.txt\" ] || { echo \"directory out rc=$rc\"; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_stale)
  # C5: the null backend keeps only the first frame's call log (bounded memory): --frames 3 writes the --frames 1 log.
  add_test(NAME view_cli_null_frames COMMAND sh -c
           "\"$0\" --synthetic 1 --backend null --out \"$1.1\" >/dev/null 2>&1 && \"$0\" --synthetic 1 --backend null --frames 3 --out \"$1.3\" >/dev/null 2>&1 && cmp \"$1.1\" \"$1.3\" && grep -q '\"frames\":3,\"frames_rendered\":3' \"$1.3.json\""
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_frames.txt)
  # Task 8B E2: --state-log must not name --out or --out.json, compared CANONICALLY (./x, a/../x and x are one file);
  # --write-install needs --synthetic and a non-empty directory (exit 2, nothing written); a valid one still works.
  add_test(NAME view_cli_paths COMMAND sh -c
           "rm -rf \"$1\"; mkdir -p \"$1/sub\" && cd \"$1\" || exit 1; for a in '--state-log o.txt' '--state-log ./o.txt' '--state-log o.txt.json' '--state-log sub/../o.txt.json' '--write-install \"\"' '--state-log s.txt --write-install \"\"'; do eval \"\\\"$0\\\" --synthetic 2 --backend null --out o.txt $a\" >/dev/null 2>&1; rc=$?; if [ $rc -ne 2 ]; then echo \"$a: rc=$rc (want 2)\"; exit 1; fi; done; [ ! -e data ] || { echo 'empty --write-install wrote into the cwd'; exit 1; }; \"$0\" --install /nonexistent/lomn --level lev1 --area vllg --backend null --out o.txt --write-install w >/dev/null 2>&1; rc=$?; [ $rc -eq 2 ] || { echo \"--write-install without --synthetic: rc=$rc (want 2)\"; exit 1; }; [ ! -e w ] || { echo 'install written without --synthetic'; exit 1; }; \"$0\" --synthetic 2 --backend null --out o.txt --state-log sub/../s.txt --write-install w >/dev/null 2>&1 || { echo 'valid paths rejected'; exit 1; }; [ -s s.txt ] && [ -s w/data/levels/levt/stat.blk ] || { echo 'valid outputs missing'; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_paths)
  # Task 8B F2: lomn_datacheck never throws on a bad TMPDIR (temp_directory_path, is_regular_file and relative use the
  # error_code overloads): with TMPDIR pointing nowhere it still prints its JSON for a (synthetic) install.
  add_test(NAME datacheck_no_tmpdir COMMAND sh -c
           "rm -rf \"$2\"; \"$1\" --synthetic 2 --backend null --out \"$2.txt\" --write-install \"$2\" >/dev/null 2>&1 || exit 1; TMPDIR=/nonexistent/lomn-tmp \"$0\" \"$2\" --areas levt/stat > \"$2.json\" 2>\"$2.err\"; rc=$?; [ $rc -eq 0 ] || { echo \"rc=$rc\"; cat \"$2.err\"; exit 1; }; grep -q '\"blk\"' \"$2.json\" || { echo 'no JSON'; exit 1; }"
           $<TARGET_FILE:lomn_datacheck> $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/datacheck_tmpdir)
  # REF4 r2 M6: --manifest lists only the areas the game can address (view::list_areas: ids of 1..4 characters, the level
  # blockfile excluded), like the browser; a longer stem (lev4c on Beta 1.5.0) is reported under not_addressable.
  add_test(NAME view_cli_manifest_addressable COMMAND sh -c
           "rm -rf \"$1\"; \"$0\" --synthetic 2 --backend null --out \"$1.txt\" --write-install \"$1\" >/dev/null 2>&1 || exit 1; cp \"$1/data/levels/levt/stat.blk\" \"$1/data/levels/levt/statx.blk\" || exit 1; \"$0\" --install \"$1\" --manifest 1 --out \"$1.json\" >/dev/null 2>&1 || { echo 'manifest failed'; exit 1; }; grep -q 'levt/statx\"' \"$1.json\" || { echo 'statx not reported'; exit 1; }; grep -q '\"area\":\"levt/statx\"' \"$1.json\" && { echo 'levt/statx listed as an area'; exit 1; }; grep -q '\"not_addressable\":\\[\"levt/statx\"\\]' \"$1.json\" || { echo 'not_addressable missing'; exit 1; }; grep -q '\"areas\":2,' \"$1.json\" || { echo 'area count != 2'; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_manifest_addr)
  # C8: a survey that finds no area is an environment error (exit 3), not an empty success.
  add_test(NAME view_cli_survey_empty COMMAND sh -c
           "rm -rf \"$1\"; mkdir -p \"$1/Data/Levels/lev1\" || exit 1; \"$0\" --install \"$1\" --survey 1 --out \"$1/s.json\" >/dev/null 2>&1; rc=$?; [ $rc -eq 3 ] || { echo \"rc=$rc (want 3)\"; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_survey_empty)
  # C9: camera.fallback is true while ANY of eye/yaw/pitch/fov/near/far is a documented fallback (spec M1 §2.3), with
  # explicit per-field sources. Synthetic scene, no overrides: eye = POS start (data), yaw = scene-set, the rest fallback.
  # Every camera field given on the command line: nothing is a fallback.
  add_test(NAME view_cli_camera_json COMMAND sh -c
           "\"$0\" --synthetic 1 --backend null --out \"$1\" >/dev/null 2>&1 && grep -q '\"camera\":{\"fallback\":true,' \"$1.json\" && grep -q '\"eye_source\":\"pos_start\"' \"$1.json\" && grep -q '\"pitch_source\":\"fallback\"' \"$1.json\" && grep -q '\"fov_source\":\"fallback\"' \"$1.json\" && grep -q '\"clip_source\":\"fallback\"' \"$1.json\" || { echo 'default camera json wrong'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$1\" --fov 50 --pitch -5 --near 1 --far 2000 --yaw 10 --eye 0,0,0 >/dev/null 2>&1 && grep -q '\"camera\":{\"fallback\":false,' \"$1.json\" && grep -q '\"eye_source\":\"cli\"' \"$1.json\" && grep -q '\"yaw_source\":\"cli\"' \"$1.json\" && grep -q '\"pitch_source\":\"cli\"' \"$1.json\" && grep -q '\"fov_source\":\"cli\"' \"$1.json\" && grep -q '\"clip_source\":\"cli\"' \"$1.json\" || { echo 'full-override camera json wrong'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$1\" --fov 50 --pitch -5 --near 1 --far 2000 --yaw 10 >/dev/null 2>&1 && grep -q '\"camera\":{\"fallback\":false,' \"$1.json\" || { echo 'POS-start eye must not count as fallback'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$1\" --fov 50 --pitch -5 --near 1 --yaw 10 --eye 0,0,0 >/dev/null 2>&1 && grep -q '\"camera\":{\"fallback\":true,' \"$1.json\" && grep -q '\"clip_source\":\"cli_partial\"' \"$1.json\" || { echo 'near-only: clip_source cli_partial, still a fallback (synthetic far)'; exit 1; }; i=\"$1.inst\"; rm -rf \"$i\"; \"$0\" --synthetic 2 --backend null --out \"$1\" --write-install \"$i\" >/dev/null 2>&1 || exit 1; for c in 'exe:' 'cli_partial:--near 0.5' 'cli_partial:--far 900' 'cli:--near 0.5 --far 900'; do want=\${c%%:*}; \"$0\" --install \"$i\" --level levt --area stat --backend null --out \"$1\" \${c#*:} >/dev/null 2>&1 && grep -q \"\\\"clip_source\\\":\\\"$want\\\"\" \"$1.json\" || { echo \"install, '\${c#*:}': clip_source want $want\"; exit 1; }; done"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_camera.txt)
  # A trailing flag with no value must be rejected with exit 2 (usage), not silently ignored.
  add_test(NAME cli_dangling_flag COMMAND sh -c
           "\"$0\" --backend null --frames 10 --out \"$1\" --shot >/dev/null 2>&1; [ $? -eq 2 ]"
           $<TARGET_FILE:lomn_native> ${CMAKE_CURRENT_BINARY_DIR}/dangling)
  if(LOMN_WEBGPU)
    add_executable(lomn_gpu_test tests/test_gpu.cpp)
    target_link_libraries(lomn_gpu_test PRIVATE lomn_webgpu)
    add_test(NAME gpu_triangle COMMAND lomn_gpu_test)
    set_tests_properties(gpu_triangle PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    # No-adapter path: forcing the D3D12 backend on macOS/Linux yields no adapter. lomn_native must exit 3 (not crash)
    # and print a readable reason. No PASS_REGULAR_EXPRESSION here (ctest would then ignore the exit status): the sh
    # wrapper checks both the exit code and the message and reports the result through its own exit status.
    add_test(NAME gpu_no_adapter COMMAND sh -c
             "out=$(\"$0\" --backend webgpu --frames 1 --out \"$1\" 2>&1); rc=$?; printf '%s\\n' \"$out\"; echo \"rc=$rc\"; [ \"$rc\" -eq 3 ] && printf '%s' \"$out\" | grep -q \"webgpu unavailable: no WebGPU adapter\""
             $<TARGET_FILE:lomn_native> ${CMAKE_CURRENT_BINARY_DIR}/noadapter)
    set_tests_properties(gpu_no_adapter PROPERTIES ENVIRONMENT "LOMN_WEBGPU_BACKEND=d3d12" LABELS gpu)
    # A7 on Dawn: the synthetic scene rendered and checked at known pixels (77 = skip without an adapter).
    add_executable(lomn_gpu_area_test tests/test_gpu_area.cpp)
    target_link_libraries(lomn_gpu_area_test PRIVATE lomn_area lomn_webgpu)
    add_test(NAME a7_gpu_pixels COMMAND lomn_gpu_area_test)
    set_tests_properties(a7_gpu_pixels PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    # rhi mesh surface on Dawn without lomn_area: blend, M0 pipelines in depth targets, rejected M0 draws.
    add_executable(lomn_gpu_mesh_test tests/test_gpu_mesh.cpp)
    target_link_libraries(lomn_gpu_mesh_test PRIVATE lomn_webgpu)
    add_test(NAME gpu_mesh COMMAND lomn_gpu_mesh_test)
    set_tests_properties(gpu_mesh PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    # M1a-3: the fixed-function uber-shader against the CPU reference of the D3D8 formulas (lighting, fog, alpha).
    add_executable(lomn_gpu_ffp_test tests/test_gpu_ffp.cpp)
    target_link_libraries(lomn_gpu_ffp_test PRIVATE lomn_d3d lomn_webgpu)
    add_test(NAME gpu_ffp COMMAND lomn_gpu_ffp_test)
    set_tests_properties(gpu_ffp PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    # M1b: destroy_* on Dawn (dependency rule, ids never reused, live counts), a depth-target re-creation leak loop,
    # surface calls rejected headless, and AreaRenderer::release over N uploads of the synthetic scene (flat counts,
    # pixels unchanged).
    add_executable(lomn_gpu_destroy_test tests/test_gpu_destroy.cpp)
    target_link_libraries(lomn_gpu_destroy_test PRIVATE lomn_area lomn_webgpu)
    add_test(NAME gpu_destroy COMMAND lomn_gpu_destroy_test)
    set_tests_properties(gpu_destroy PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_test(NAME a7_gpu_no_adapter COMMAND sh -c
             "out=$(\"$0\" --synthetic 1 --out \"$1\" 2>&1); rc=$?; printf '%s\\n' \"$out\"; [ \"$rc\" -eq 3 ] && printf '%s' \"$out\" | grep -q \"webgpu unavailable: no WebGPU adapter\""
             $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/a7_noadapter.png)
    set_tests_properties(a7_gpu_no_adapter PROPERTIES ENVIRONMENT "LOMN_WEBGPU_BACKEND=d3d12" LABELS gpu)
  endif()
endif()

# Unit tests: natively, and under Node when cross-compiled with emcmake (CMAKE_CROSSCOMPILING_EMULATOR=node).
add_executable(lomn_tests tests/test_main.cpp tests/test_fs.cpp tests/test_rhi_null.cpp tests/test_loop.cpp tests/test_sage_blockfile.cpp tests/test_sage_blockchain.cpp tests/test_sage_slb.cpp tests/test_sage_xfile.cpp tests/test_sage_image.cpp
               tests/test_rhi_mesh.cpp tests/test_view_mips.cpp tests/test_view_area.cpp tests/test_d3d.cpp tests/test_sage_osi.cpp
               tests/test_area_states.cpp tests/test_sage_frame.cpp
               tests/test_rhi_destroy.cpp tests/test_prefetch_fs.cpp tests/test_fly_camera.cpp)  # M1b
target_link_libraries(lomn_tests PRIVATE lomn_area)
target_include_directories(lomn_tests PRIVATE "${LOMN_TOOLCHAIN_DIR}/include")
if(EMSCRIPTEN)
  target_link_options(lomn_tests PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1)
endif()
add_test(NAME unit COMMAND lomn_tests)
# REF3 revision 2: crafted-input time/memory probes of the OSI reader, natively and as wasm32 (2 GB linear memory).
add_executable(lomn_osi_probe tests/probe_osi.cpp)
target_link_libraries(lomn_osi_probe PRIVATE lomn_sage)
if(EMSCRIPTEN)
  target_link_options(lomn_osi_probe PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB)
endif()
add_test(NAME osi_probe COMMAND lomn_osi_probe)
# Task 8B C1: the same time/memory probe for textures.slb (a 64 MiB table must hit kMaxSpecTextures), native and wasm32.
add_executable(lomn_slb_probe tests/probe_slb.cpp)
target_link_libraries(lomn_slb_probe PRIVATE lomn_sage)
if(EMSCRIPTEN)
  target_link_options(lomn_slb_probe PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB)
endif()
add_test(NAME slb_probe COMMAND lomn_slb_probe)

# Internal JSON string escaper used by metrics.json (header lives in src/app, not in the public include dir).
target_sources(lomn_tests PRIVATE tests/test_json_escape.cpp)
target_include_directories(lomn_tests PRIVATE src/app)
