# Name:        vita.cmake
# Description: the console game (OutRunners) for the PlayStation Vita, built with VitaSDK
# Status:      FIRST DRAFT: not built and not run on a Vita yet. Modelled on ports/anbernic/anbernic.cmake.
#
#   export VITASDK=/usr/local/vitasdk && export PATH=$VITASDK/bin:$PATH
#   cmake -S . -B build/vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
#         -DORUNNERS_VITA=ON -DCMAKE_BUILD_TYPE=Release -DORUNNERS_REFERENCE_TOOLS=OFF
#   cmake --build build/vita --target OutRunners.vpk
#
# The VPK holds the game and CONTROLS' button pictures, nothing from the ROM. The assets pack of the player's
# own ROM set (outrunners.assets, made on a computer by OutRunners Anbernic) goes to ux0:data/OutRunners/
# (ports/vita/README.md): never share it.

if(NOT DEFINED ENV{VITASDK})
	message(FATAL_ERROR "VITASDK is not set: install VitaSDK (https://vitasdk.org) and export VITASDK")
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(ORUNNERS_VITA_TITLEID "ORUN00001" CACHE STRING "The Vita's title id: 4 capital letters and 5 digits")
set(ORUNNERS_VITA_VERSION "01.00" CACHE STRING "The version the Vita shows: XX.YY")

# (as the Anbernic build: no first run, so no letters.c)
add_executable(OutRunners ports/sdl2/main.c ports/sdl2/forces.c ports/sdl2/controls.c ports/vita/vita_runtime.c)

# The handheld's way, as anbernic.cmake has it (the picture fixed, the log, no first run, CONTROLS in OPTIONS),
# on the console library, which console/*.c is built in, and WIDE 16:9 as the Switch's: the Vita's screen is
# 960 x 544. Left out: the Anbernic's own ORUNNERS_HANDHELD_KEYS (MENU and the volume keys) and
# ORUNNERS_ANBERNIC_BUTTONS (its button defaults): the Vita gets SDL's game controller defaults (Cross the gas,
# Circle the brake, the shoulders shift), which OPTIONS > CONTROLS changes
target_compile_definitions(orunners_game_console PUBLIC ORUNNERS_HANDHELD ORUNNERS_CONTROLS_PAGE ORUNNERS_SCREEN_WIDE)
target_compile_definitions(OutRunners PRIVATE ORUNNERS_VITA)

# VitaSDK's SDL2 is a static library in its own sysroot
target_include_directories(OutRunners PRIVATE $ENV{VITASDK}/arm-vita-eabi/include/SDL2)

target_link_libraries(OutRunners PRIVATE orunners_game_console orunners_sound
	SDL2
	# what SDL2's Vita backend and the game's own code (arcade_lan's sockets, the clocks) call
	SceDisplay_stub SceCtrl_stub SceAudio_stub SceAudioIn_stub SceSysmodule_stub SceGxm_stub
	SceCommonDialog_stub SceAppMgr_stub SceTouch_stub SceHid_stub SceMotion_stub
	SceNet_stub SceNetCtl_stub ScePower_stub
	m pthread)    # (VitaSDK's toolchain is not UNIX to CMake: the runtime's own m and Threads are not linked)
target_link_options(OutRunners PRIVATE -Wl,-q)

vita_create_self(OutRunners.self OutRunners)
vita_create_vpk(OutRunners.vpk ${ORUNNERS_VITA_TITLEID} OutRunners.self
	VERSION ${ORUNNERS_VITA_VERSION}
	NAME "OutRunners"
	FILE ${CMAKE_SOURCE_DIR}/ports/sdl2/prompts/input_prompts.bmp input_prompts.bmp)
