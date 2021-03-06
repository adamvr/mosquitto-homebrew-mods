# This is a cmake script.
# It is only intended for building libmosquitto and the clients and can be
# used to generate Visual Studio projects, allowing them to be compiled
# natively on Windows.

project(mosquitto)

cmake_minimum_required(VERSION 2.6)

set (VERSION 0.9)

if (WIN32)
	execute_process(COMMAND echo "%DATE% %TIME%" OUTPUT_VARIABLE TIMESTAMP
			OUTPUT_STRIP_TRAILING_WHITESPACE)
else (WIN32)
	execute_process(COMMAND date "+%F %T%z" OUTPUT_VARIABLE TIMESTAMP
			OUTPUT_STRIP_TRAILING_WHITESPACE)
endif (WIN32)

add_definitions (-DVERSION=\"${VERSION}\" -DTIMESTAMP=\"${TIMESTAMP}\")

if (WIN32)
    set (BINDEST .)
    set (LIBDEST .)
    set (SHAREDEST .)
    set (MANDEST .)
    set (CONFDEST .)
else (WIN32)
    set (BINDEST bin)
    set (LIBDEST lib${LIB_SUFFIX})
    set (SHAREDEST share/mosquitto)
    set (MANDEST share/man)
    set (CONFDEST etc)
endif (WIN32)

# ========================================
# Check for required compatibility headers
# ========================================
include (CheckIncludeFile)

check_include_file(stdbool.h HAVE_STDBOOL_H)
if (NOT HAVE_STDBOOL_H)
	set (STDBOOL_H_PATH ${mosquitto_SOURCE_DIR}/compat/stdbool)
endif (NOT HAVE_STDBOOL_H)

check_include_file(stdint.h HAVE_STDINT_H)
if (NOT HAVE_STDINT_H)
	set (STDINT_H_PATH ${mosquitto_SOURCE_DIR}/compat/stdint)
endif (NOT HAVE_STDINT_H)

# ========================================
# Include projects
# ========================================

add_subdirectory(lib)
add_subdirectory(client)
add_subdirectory(src)
add_subdirectory(man)

# ========================================
# Install config file
# ========================================

install(FILES ./mosquitto.conf DESTINATION ${CONFDEST}/)
