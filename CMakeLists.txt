# CMakeLists.txt - simple-xudp (SX) 备选构建
#
# 用法：
#   cmake -S . -B build-cmake && cmake --build build-cmake -j
#
# 说明：主构建仍推荐 Makefile（自动探测 clang BPF 目标）。CMake 版本提供
#       库、示例、测试目标，并生成内嵌 BPF 字节数组。

cmake_minimum_required(VERSION 3.16)
project(simplexudp C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

add_compile_definitions(_GNU_SOURCE SX_XDP_EMBEDDED)

pkg_check_modules(LIBBPF REQUIRED libbpf)
pkg_check_modules(LIBXDP REQUIRED libxdp)

find_program(BPF_CLANG NAMES clang REQUIRED)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-multiarch
    OUTPUT_VARIABLE MULTIARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(BPF_OBJ ${CMAKE_BINARY_DIR}/sx_xdp_kern.o)
add_custom_command(
    OUTPUT ${BPF_OBJ}
    COMMAND ${BPF_CLANG} -target bpf -O2 -g -I/usr/include/${MULTIARCH} -c
            ${CMAKE_SOURCE_DIR}/bpf/sx_xdp_kern.c -o ${BPF_OBJ}
    DEPENDS ${CMAKE_SOURCE_DIR}/bpf/sx_xdp_kern.c
    COMMENT "编译 XDP BPF 目标 sx_xdp_kern.o")

set(BLOB_C ${CMAKE_BINARY_DIR}/sx_xdp_kern_blob.c)
add_custom_command(
    OUTPUT ${BLOB_C}
    COMMAND sh ${CMAKE_SOURCE_DIR}/scripts/gen_blob.sh ${BPF_OBJ} ${BLOB_C}
            sx_xdp_kern_o
    DEPENDS ${BPF_OBJ}
    COMMENT "生成内嵌 BPF 字节数组")
set_source_files_properties(${BLOB_C} PROPERTIES GENERATED TRUE)

set(SX_SOURCES
    src/sx_util.c
    src/sx_proto.c
    src/sx_frame.c
    src/sx_arp.c
    src/sx_kcp.c
    src/sx_group.c
    src/sx_io_udp.c
    src/sx_io_xdp.c
    src/sx_endpoint.c
    ${BLOB_C})

add_library(simplexudp STATIC ${SX_SOURCES})
add_library(simplexudp_shared SHARED ${SX_SOURCES})
set_target_properties(simplexudp_shared PROPERTIES OUTPUT_NAME simplexudp)

foreach(tgt simplexudp simplexudp_shared)
    target_include_directories(${tgt}
        PUBLIC ${CMAKE_SOURCE_DIR}/include
        PRIVATE ${CMAKE_SOURCE_DIR}/src ${LIBBPF_INCLUDE_DIRS}
                ${LIBXDP_INCLUDE_DIRS})
    target_link_libraries(${tgt} PUBLIC ${LIBBPF_LIBRARIES}
                                        ${LIBXDP_LIBRARIES} pthread)
endforeach()

add_executable(sx_p2p examples/sx_p2p.c)
target_link_libraries(sx_p2p PRIVATE simplexudp ${LIBBPF_LIBRARIES}
                                            ${LIBXDP_LIBRARIES} pthread)

add_executable(sx_group examples/sx_group.c)
target_link_libraries(sx_group PRIVATE simplexudp ${LIBBPF_LIBRARIES}
                                              ${LIBXDP_LIBRARIES} pthread)

enable_testing()
foreach(t frame proto kcp loopback)
    add_executable(test_${t} tests/test_${t}.c)
    target_include_directories(test_${t} PRIVATE src)
    target_link_libraries(test_${t} PRIVATE simplexudp ${LIBBPF_LIBRARIES}
                                                 ${LIBXDP_LIBRARIES} pthread)
    add_test(NAME test_${t}
             COMMAND test_${t} ${CMAKE_BINARY_DIR}/test_${t}.log)
endforeach()
