# Syntax:
#
# 	yosys_mockturtle_target(<target> <source-dir>)
#
# Creates an INTERFACE library `<target>` exposing the header-only mockturtle checkout at
# `<source-dir>` together with its one compiled dependency (libabcesop). The vendored header-only
# fmt v6 is used instead of the fmt 12 bundled for slang: lorina formats runtime strings, which
# fmt 12 rejects at compile time. Both live in distinct inline namespaces, so they coexist.
#
function(yosys_mockturtle_target arg_TARGET arg_SOURCE_DIR)
	add_library(${arg_TARGET} INTERFACE)
	target_include_directories(${arg_TARGET} SYSTEM INTERFACE
		${arg_SOURCE_DIR}/lib/fmt
		${arg_SOURCE_DIR}/include
		${arg_SOURCE_DIR}/lib/kitty
		${arg_SOURCE_DIR}/lib/lorina
		${arg_SOURCE_DIR}/lib/parallel_hashmap
		${arg_SOURCE_DIR}/lib/rang
		${arg_SOURCE_DIR}/lib/bill
		${arg_SOURCE_DIR}/lib/percy
	)
	target_compile_definitions(${arg_TARGET} INTERFACE FMT_HEADER_ONLY DISABLE_NAUTY)

	set(STATIC_LIBABC TRUE)
	set(ABC_USE_NAMESPACE "pabc")
	add_subdirectory(${arg_SOURCE_DIR}/lib/abcesop ${YOSYS_CMAKE_BINARY_DIR}/mockturtle/abcesop)
	add_subdirectory(${arg_SOURCE_DIR}/lib/abcsat ${YOSYS_CMAKE_BINARY_DIR}/mockturtle/abcsat)
	target_compile_definitions(libabcsat PUBLIC LIN64 ABC_NAMESPACE=pabc ABC_NO_USE_READLINE)
	set_target_properties(libabcesop libabcsat PROPERTIES YOSYS_IS_ABC ON)
	find_package(Threads REQUIRED)
	target_link_libraries(${arg_TARGET} INTERFACE libabcesop libabcsat Threads::Threads)
endfunction()
