cmake_minimum_required(VERSION 3.20.0)

if (NOT DEFINED ZEPHYR_BASE)
	message(STATUS "ZEPHYR_BASE not set; this module is intended for Zephyr builds.")
	return()
endif()

project(zephyr_audio_pipeline)

add_subdirectory(subsys/audio/pipeline)
