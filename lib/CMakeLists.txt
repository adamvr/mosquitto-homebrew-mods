add_subdirectory(cpp)
add_subdirectory(python)

include_directories(${mosquitto_SOURCE_DIR} ${mosquitto_SOURCE_DIR}/lib
			${STDBOOL_H_PATH} ${STDINT_H_PATH})
link_directories(${mosquitto_SOURCE_DIR}/lib)

add_library(libmosquitto SHARED
	logging_mosq.c logging_mosq.h
	memory_mosq.c memory_mosq.h
	messages_mosq.c messages_mosq.h
	mosquitto.c mosquitto.h
	mqtt3_protocol.h
	net_mosq.c net_mosq.h
	read_handle.c read_handle.h
	read_handle_client.c
	send_client_mosq.c
	send_mosq.c send_mosq.h
	util_mosq.c util_mosq.h)

if (WIN32)
	target_link_libraries(libmosquitto ws2_32)
endif (WIN32)

set_target_properties(libmosquitto PROPERTIES
	OUTPUT_NAME mosquitto
	VERSION ${VERSION}
	SOVERSION 0
)

install(TARGETS libmosquitto RUNTIME DESTINATION ${BINDEST} LIBRARY DESTINATION ${LIBDEST})

if (UNIX)
	install(CODE "EXEC_PROGRAM(/sbin/ldconfig)")
endif (UNIX)
