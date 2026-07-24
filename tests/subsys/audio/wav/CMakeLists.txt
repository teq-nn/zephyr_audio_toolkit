cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_wav_tests)

get_filename_component(AUDIO_PIPELINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)

# The WAV module is compiled straight into the test image so the suite stays a
# standalone unit test and does not depend on the rest of the subsystem. Its
# header needs no include path of its own: the module exports include/ through
# zephyr_include_directories() whether or not CONFIG_AUDIO_PIPELINE is set.
target_sources(app PRIVATE
	test_wav.c
	${AUDIO_PIPELINE_ROOT}/subsys/audio/pipeline/audio_wav.c
)
