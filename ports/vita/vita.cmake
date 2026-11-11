# Name:        vita.cmake
# Description: the build for the PlayStation Vita (vitasdk): the console game alone, as OutRunners.vpk
#
# Included by CMakeLists.txt with ORUNNERS_VITA (and vitasdk's toolchain file: -DCMAKE_TOOLCHAIN_FILE=
# $VITASDK/share/vita.toolchain.cmake). As the Anbernic build: the console game with its picture fixed
# (ORUNNERS_HANDHELD), the system's SDL2 (vitasdk's), and no assets in the package: outrunners.assets is made
# on a computer by OutRunners Anbernic from the player's ROM set and put in ux0:data/outrunners/

# vitasdk's macros (vita_create_self, vita_create_vpk): the toolchain file does not load them
if(NOT DEFINED ENV{VITASDK})
	message(FATAL_ERROR "VITASDK is not set: the Vita build needs vitasdk")
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

target_compile_definitions(orunners_game_console PUBLIC ORUNNERS_HANDHELD ORUNNERS_CONTROLS_PAGE ORUNNERS_VITA)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(OutRunners ports/sdl2/main.c ports/sdl2/forces.c ports/sdl2/controls.c ports/vita/vita_runtime.c)
target_include_directories(OutRunners PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(OutRunners PRIVATE orunners_game_console orunners_sound ${SDL2_LIBRARIES} ScePower_stub pthread m stdc++)

# vita_create_self looks for the ELF in this folder. CMakeLists.txt sends every executable to build/bin through
# the plain and the per-configuration variables (Release is the default), and a per-configuration property
# beats the plain one: all of them are set here
set(ORUNNERS_VITA_OUT ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(OutRunners PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ORUNNERS_VITA_OUT})
foreach(config IN ITEMS _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
	set_target_properties(OutRunners PROPERTIES RUNTIME_OUTPUT_DIRECTORY${config} ${ORUNNERS_VITA_OUT})
endforeach()

vita_create_self(OutRunners.self OutRunners)
vita_create_vpk(OutRunners.vpk ORUN00001 OutRunners.self
	VERSION 01.01 NAME "OutRunners"
	FILE ${CMAKE_SOURCE_DIR}/ports/vita/sce_sys sce_sys
	FILE ${CMAKE_SOURCE_DIR}/ports/sdl2/prompts/input_prompts.bmp input_prompts.bmp)
