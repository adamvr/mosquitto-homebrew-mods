install(CODE 
    "EXECUTE_PROCESS(
	COMMAND python ./setup.py install --prefix=${CMAKE_INSTALL_PREFIX}
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/python
    )
    EXECUTE_PROCESS(
	COMMAND python ./setup.py clean --all
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/python
    )
")
