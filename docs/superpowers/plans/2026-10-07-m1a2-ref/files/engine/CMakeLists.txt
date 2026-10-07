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
    add_test(NAME a7_gpu_no_adapter COMMAND sh -c
             "out=$(\"$0\" --synthetic 1 --out \"$1\" 2>&1); rc=$?; printf '%s\\n' \"$out\"; [ \"$rc\" -eq 3 ]"
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
