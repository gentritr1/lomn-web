cmake_minimum_required(VERSION 3.28)
project(lomn LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pinned third-party inputs are fetched by tools/toolchain.sh into .toolchain/ (see deps.lock).
set(LOMN_TOOLCHAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.toolchain" CACHE PATH "pinned toolchain root")
option(LOMN_WEBGPU "build the WebGPU backend" ON)
enable_testing()

add_library(lomn_core STATIC src/fs/memory_fs.cpp src/rhi/null_device.cpp src/app/loop.cpp)
target_include_directories(lomn_core PUBLIC include PRIVATE "${LOMN_TOOLCHAIN_DIR}/include")
target_compile_options(lomn_core PRIVATE -Wall -Wextra)
# Native clang fuses a*b+c into FMA by default and wasm never does; forbid it so native == wasm32 bit for bit.
# PUBLIC: every target linking lomn_core inherits it.
target_compile_options(lomn_core PUBLIC -ffp-contract=off)

# M1a-1 data path (no GPU): SAGE blockfiles, lookup chain, SLB tables, binary .x meshes, DDS/TGA decoders.
add_library(lomn_sage STATIC src/sage/blockfile.cpp src/sage/blockchain.cpp src/sage/slb.cpp src/sage/xfile.cpp src/sage/image.cpp)
target_link_libraries(lomn_sage PUBLIC lomn_core)
target_compile_options(lomn_sage PRIVATE -Wall -Wextra)

# M1a-2 first pixels: CPU mips, D3D-convention matrices, area loader + unlit renderer on rhi, the A7 synthetic scene.
add_library(lomn_area STATIC src/view/mips.cpp src/view/math.cpp src/view/area.cpp src/view/synthetic.cpp)
target_link_libraries(lomn_area PUBLIC lomn_sage)
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
    add_executable(lomn_web src/app/main_web.cpp)
    target_link_libraries(lomn_web PRIVATE lomn_webgpu)
    target_link_options(lomn_web PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB)
  endif()
  # Null-backend runner under Node: its trace/calls must be byte-identical to the native runner's.
  add_executable(lomn_native src/app/main_native.cpp)
  target_link_libraries(lomn_native PRIVATE lomn_core)
  target_link_options(lomn_native PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1)
  add_test(NAME determinism_null COMMAND ${CMAKE_COMMAND} -DRUNNER=node -DEXE=$<TARGET_FILE:lomn_native>
           -DOUT=${CMAKE_CURRENT_BINARY_DIR}/determinism -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/determinism.cmake)
  # M1a-2 A7 under Node: the same synthetic scene's null call log must equal the same checked-in expected log, so the
  # native and wasm32 logs are byte-identical (make test-wasm also cmp's the two files directly).
  add_executable(lomn_view tools/lomn_view.cpp)
  target_link_libraries(lomn_view PRIVATE lomn_area)
  target_include_directories(lomn_view PRIVATE src/app)
  target_link_options(lomn_view PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1)
  add_test(NAME a7_null_calls COMMAND ${CMAKE_COMMAND} -DRUNNER=node -DEXE=$<TARGET_FILE:lomn_view> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/a7
           -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a7_calls.txt -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a7_calls.cmake)
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
  # A7 contract (no game data): the synthetic scene's null call log must equal the checked-in expected log, twice.
  add_test(NAME a7_null_calls COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:lomn_view> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/a7
           -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/a7_calls.txt -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/a7_calls.cmake)
  add_test(NAME view_cli_dangling_flag COMMAND sh -c
           "\"$0\" --synthetic 1 --backend null --out \"$1\" --fov >/dev/null 2>&1; [ $? -eq 2 ]"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_dangling.txt)
  add_test(NAME view_cli_missing_install COMMAND sh -c
           "\"$0\" --install /nonexistent/lomn --level lev1 --area vllg --backend null --out \"$1\" >/dev/null 2>&1; [ $? -eq 3 ]"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_missing.txt)
  # Task 6 C1: non-finite and out-of-range camera/render floats are usage errors (exit 2), never exit 0 + invalid JSON;
  # edge values pass and --yaw is normalised to (-180, 180].
  add_test(NAME view_cli_nonfinite COMMAND sh -c
           "for a in '--far inf' '--far nan' '--near inf' '--near -1' '--near 0' '--near 5000' '--far 1e8' '--fov inf' '--fov 0' '--fov 180' '--yaw inf' '--yaw nan' '--pitch nan' '--pitch 90' '--pitch -89.95' '--eye nan,0,0' '--eye 0,inf,0' '--eye 2e6,0,0' '--eye-height inf' '--eye-height -2e4' '--alpha-ref nan' '--alpha-ref 256' '--alpha-ref -1'; do \"$0\" --synthetic 1 --backend null --out \"$1\" $a >/dev/null 2>&1; rc=$?; if [ $rc -ne 2 ]; then echo \"$a: rc=$rc (want 2)\"; exit 1; fi; done; \"$0\" --synthetic 1 --backend null --out \"$1\" --yaw 400 --pitch 89.9 --far 1e7 --eye-height 1e4 --alpha-ref 255 >/dev/null 2>&1 || { echo 'edge values rejected'; exit 1; }; grep -q '\"yaw_deg\":40.000' \"$1.json\" || { echo 'yaw not normalised'; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_nonfinite.txt)
  # C2: --synthetic / --survey take exactly 1; conflicting modes are usage errors.
  add_test(NAME view_cli_modes COMMAND sh -c
           "for a in '--synthetic 0 --install /nonexistent/lomn --level lev1 --area vllg' '--synthetic yes' '--synthetic 1 --install /nonexistent/lomn' '--synthetic 1 --level lev1' '--synthetic 1 --area vllg' '--survey 0 --install /nonexistent/lomn' '--synthetic 1 --survey 1 --install /nonexistent/lomn'; do \"$0\" --backend null --out \"$1\" $a >/dev/null 2>&1; rc=$?; if [ $rc -ne 2 ]; then echo \"$a: rc=$rc (want 2)\"; exit 1; fi; done"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_modes.txt)
  # C3: integers need a leading digit and only digits (no space, sign, hex); parsed in u64, then range-checked.
  add_test(NAME view_cli_ints COMMAND sh -c
           "for v in ' 5' '+5' '5 ' '0x10' '-1' '' '18446744073709551621'; do \"$0\" --synthetic 1 --backend null --out \"$1\" --width \"$v\" >/dev/null 2>&1; rc=$?; if [ $rc -ne 2 ]; then echo \"--width '$v': rc=$rc (want 2)\"; exit 1; fi; done; \"$0\" --synthetic 1 --backend null --out \"$1\" --frames +2 >/dev/null 2>&1; [ $? -eq 2 ] || { echo '--frames +2 accepted'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$1\" --width 0064 >/dev/null 2>&1 || { echo '--width 0064 rejected'; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_ints.txt)
  # C4: --out and --out.json are removed up front, so a failed run (usage, missing install, unwritable out) never
  # leaves a stale output behind; an --out that cannot be written is exit 4.
  add_test(NAME view_cli_stale_outputs COMMAND sh -c
           "o=\"$1/out.txt\"; rm -rf \"$1\"; mkdir -p \"$1/dir.txt\" || exit 1; printf stale > \"$o\"; printf stale > \"$o.json\"; \"$0\" --install /nonexistent/lomn --level lev1 --area vllg --backend null --out \"$o\" >/dev/null 2>&1; rc=$?; [ $rc -eq 3 ] || { echo \"missing install rc=$rc\"; exit 1; }; [ ! -e \"$o\" ] && [ ! -e \"$o.json\" ] || { echo 'stale output left after exit 3'; exit 1; }; printf stale > \"$o\"; printf stale > \"$o.json\"; \"$0\" --synthetic 1 --backend null --out \"$o\" --fov 0 >/dev/null 2>&1; [ $? -eq 2 ] && [ ! -e \"$o\" ] && [ ! -e \"$o.json\" ] || { echo 'stale output left after exit 2'; exit 1; }; \"$0\" --synthetic 1 --backend null --out \"$1/dir.txt\" >/dev/null 2>&1; rc=$?; [ $rc -eq 4 ] && [ -d \"$1/dir.txt\" ] || { echo \"directory out rc=$rc\"; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_stale)
  # C5: the null backend keeps only the first frame's call log (bounded memory): --frames 3 writes the --frames 1 log.
  add_test(NAME view_cli_null_frames COMMAND sh -c
           "\"$0\" --synthetic 1 --backend null --out \"$1.1\" >/dev/null 2>&1 && \"$0\" --synthetic 1 --backend null --frames 3 --out \"$1.3\" >/dev/null 2>&1 && cmp \"$1.1\" \"$1.3\" && grep -q '\"frames\":3,\"frames_rendered\":3' \"$1.3.json\""
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_frames.txt)
  # C8: a survey that finds no area is an environment error (exit 3), not an empty success.
  add_test(NAME view_cli_survey_empty COMMAND sh -c
           "rm -rf \"$1\"; mkdir -p \"$1/Data/Levels/lev1\" || exit 1; \"$0\" --install \"$1\" --survey 1 --out \"$1/s.json\" >/dev/null 2>&1; rc=$?; [ $rc -eq 3 ] || { echo \"rc=$rc (want 3)\"; exit 1; }"
           $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/view_survey_empty)
  # C9: camera.fallback reflects eye_source (the synthetic eye is the POS start: data, not a fallback).
  add_test(NAME view_cli_camera_json COMMAND sh -c
           "\"$0\" --synthetic 1 --backend null --out \"$1\" >/dev/null 2>&1 && grep -q '\"camera\":{\"fallback\":false,' \"$1.json\" && grep -q '\"eye_source\":\"pos_start\"' \"$1.json\""
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
    add_test(NAME a7_gpu_no_adapter COMMAND sh -c
             "out=$(\"$0\" --synthetic 1 --out \"$1\" 2>&1); rc=$?; printf '%s\\n' \"$out\"; [ \"$rc\" -eq 3 ] && printf '%s' \"$out\" | grep -q \"webgpu unavailable: no WebGPU adapter\""
             $<TARGET_FILE:lomn_view> ${CMAKE_CURRENT_BINARY_DIR}/a7_noadapter.png)
    set_tests_properties(a7_gpu_no_adapter PROPERTIES ENVIRONMENT "LOMN_WEBGPU_BACKEND=d3d12" LABELS gpu)
  endif()
endif()

# Unit tests: natively, and under Node when cross-compiled with emcmake (CMAKE_CROSSCOMPILING_EMULATOR=node).
add_executable(lomn_tests tests/test_main.cpp tests/test_fs.cpp tests/test_rhi_null.cpp tests/test_loop.cpp tests/test_sage_blockfile.cpp tests/test_sage_blockchain.cpp tests/test_sage_slb.cpp tests/test_sage_xfile.cpp tests/test_sage_image.cpp
               tests/test_rhi_mesh.cpp tests/test_view_mips.cpp tests/test_view_area.cpp)
target_link_libraries(lomn_tests PRIVATE lomn_area)
target_include_directories(lomn_tests PRIVATE "${LOMN_TOOLCHAIN_DIR}/include")
if(EMSCRIPTEN)
  target_link_options(lomn_tests PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1)
endif()
add_test(NAME unit COMMAND lomn_tests)

# Internal JSON string escaper used by metrics.json (header lives in src/app, not in the public include dir).
target_sources(lomn_tests PRIVATE tests/test_json_escape.cpp)
target_include_directories(lomn_tests PRIVATE src/app)
