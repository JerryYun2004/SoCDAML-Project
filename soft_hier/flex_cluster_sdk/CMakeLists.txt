cmake_minimum_required(VERSION 3.13)

project(SoftHier LANGUAGES C)

# --- Toolchain (override from the command line if you want) ---
set(RISCV_COMPILER "riscv32-unknown-elf-gcc" CACHE STRING "RISC-V GCC")
set(RISCV_OBJDUMP  "riscv32-unknown-elf-objdump" CACHE STRING "RISC-V objdump")

# --- App source dir (you already pass -DSRC_DIR=... from make) ---
set(SRC_DIR "${SRC_DIR}" CACHE PATH "Source directory")
if (NOT SRC_DIR)
  set(SRC_DIR "${CMAKE_SOURCE_DIR}/app_example")
endif()
add_subdirectory(${SRC_DIR} ${CMAKE_BINARY_DIR}/app)

# --- Include dirs (default to runtime/include) ---
if (NOT INCLUDE_DIRS)
  set(INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/runtime/include)
endif()

# --- Linker script ---
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/runtime/flex_memory.ld)

# --- Build ELF ---
add_custom_command(
  OUTPUT softhier.elf
  COMMAND ${RISCV_COMPILER}
          -I ${CMAKE_SOURCE_DIR}/runtime/include
          -I ${INCLUDE_DIRS}
          -T ${LINKER_SCRIPT}
          -nostartfiles
          -mabi=ilp32d -mcmodel=medlow -march=${RISCV_ARCH}
          -g -O3 -ffast-math -fno-builtin -fno-tree-vectorize -fno-common
          -ffunction-sections -Wl,--gc-sections
          -o softhier.elf
          ${CMAKE_SOURCE_DIR}/runtime/flex_start.s
          ${SOURCES}
  VERBATIM
  COMMENT "Generating softhier.elf"
)
add_custom_target(build_output ALL DEPENDS softhier.elf)

# --- Disassembly ---
add_custom_command(
  OUTPUT softhier.dump
  DEPENDS softhier.elf
  COMMAND ${RISCV_OBJDUMP} -D softhier.elf > softhier.dump
  VERBATIM
  COMMENT "Creating disassembly softhier.dump"
)
add_custom_target(dump ALL DEPENDS softhier.dump)
