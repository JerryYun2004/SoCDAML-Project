cmake_minimum_required(VERSION 3.16.3)
project(SoCDAMLDependencyProbe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(SystemCLanguage 2.3.3.20181013 EXACT CONFIG REQUIRED)
add_executable(dependency-probe probe.cpp)
target_compile_definitions(dependency-probe PRIVATE SC_USE_PTHREADS)
target_link_libraries(dependency-probe PRIVATE SystemC::systemc ${CMAKE_DL_LIBS})

if(APPLE)
    # The shared SystemC runtime resolves sc_main from this executable.
    target_link_options(dependency-probe PRIVATE "-Wl,-export_dynamic")
endif()
