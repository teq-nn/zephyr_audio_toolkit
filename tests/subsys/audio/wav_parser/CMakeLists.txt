cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wav_parser_tests)

get_filename_component(AUDIO_PIPELINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
set(AUDIO_PIPELINE_UTIL_DIR ${AUDIO_PIPELINE_ROOT}/subsys/audio/pipeline/util)

# The parser is compiled straight into the test image so the suite stays a
# standalone unit test and does not depend on the rest of the subsystem.
target_sources(app PRIVATE
	test_wav_parser.c
	${AUDIO_PIPELINE_UTIL_DIR}/wav_parser.c
)

target_include_directories(app PRIVATE ${AUDIO_PIPELINE_UTIL_DIR})
