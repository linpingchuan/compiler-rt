# Build for the XRay runtime support library.

set(XRAY_SOURCES
  xray_init.cc
	xray_interface.cc
	xray_flags.cc
)

set(x86_64_SOURCES
		xray_trampoline_x86.S
		${XRAY_SOURCES})

include_directories(..)
include_directories(../../include)

set(XRAY_CFLAGS ${SANITIZER_COMMON_CFLAGS})

set(XRAY_COMMON_DEFINITIONS XRAY_HAS_EXCEPTIONS=1)

add_compiler_rt_object_libraries(RTXray
		ARCHS ${XRAY_SUPPORTED_ARCH}
		SOURCES ${XRAY_SOURCES} CFLAGS ${XRAY_CFLAGS}
		DEFS ${XRAY_COMMON_DEFINITIONS})

add_custom_target(xray)
set(XRAY_COMMON_RUNTIME_OBJECT_LIBS
		RTSanitizerCommon
		RTSanitizerCommonLibc)

foreach (arch ${XRAY_SUPPORTED_ARCH})
		if (CAN_TARGET_${arch})
				add_compiler_rt_runtime(clang_rt.xray
						STATIC
						ARCHS ${arch}
						SOURCES ${${arch}_SOURCES}
						CFLAGS ${XRAY_CFLAGS}
						DEFS ${XRAY_COMMON_DEFINITIONS}
						OBJECT_LIBS ${XRAY_COMMON_RUNTIME_OBJECT_LIBS}
						PARENT_TARGET xray)
		endif ()
endforeach()

add_dependencies(compiler-rt xray)

# if(COMPILER_RT_INCLUDE_TESTS)
#   add_subdirectory(tests)
# endif()
