# # Specify the source files for the "src" module
# set(SRC_SOURCES
#     ${CMAKE_CURRENT_SOURCE_DIR}/main.c
# )

# # Make sources and include paths visible to the main CMakeLists.txt
# set(SOURCES ${SRC_SOURCES} PARENT_SCOPE)
# set(nfvpNCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include PARENT_SCOPE)

set(SRC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main_test.c
    # ${CMAKE_CURRENT_SOURCE_DIR}/../../flex_cluster_sdk/runtime/include/fix_proj.c
)

set(SOURCES ${SRC_SOURCES} PARENT_SCOPE)
set(INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../flex_cluster_sdk/runtime/include
    PARENT_SCOPE)
