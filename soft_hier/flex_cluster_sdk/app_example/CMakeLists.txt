set(SOCDAML_APP "$ENV{SOCDAML_APP}")

if("${SOCDAML_APP}" STREQUAL "")
    set(SOCDAML_APP "diagnostics/timer")
endif()

set(APP_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/${SOCDAML_APP}.c"
)

if(NOT EXISTS "${APP_SOURCE}")
    message(FATAL_ERROR "Application source not found: ${APP_SOURCE}")
endif()

message(STATUS "SoCDAML application: ${SOCDAML_APP}")

set(SOURCES "${APP_SOURCE}" PARENT_SCOPE)
set(INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PARENT_SCOPE
)