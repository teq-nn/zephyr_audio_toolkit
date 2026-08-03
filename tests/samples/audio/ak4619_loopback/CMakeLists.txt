cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ak4619_tests)

# The driver under test lives with the sample that owns it (issue #42), so the
# suite reaches across to it rather than the other way round. This is the only
# place that path is spelled out.
set(AK4619_SAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../samples/audio/ak4619_loopback)

target_sources(app PRIVATE
	${AK4619_SAMPLE_DIR}/drivers/ak4619.c
	${AK4619_SAMPLE_DIR}/src/loopback_verdict.c
	src/ak4619_emul.c
	src/test_ak4619.c
	src/test_loopback_verdict.c
)

# drivers/ for the driver's own header; src/ of the *sample* for
# loopback_format.h, the single definition of the audio format the codec and
# the two I2S blocks all come from (issue #46). The suite asserts what that
# definition programs, which is the only way to catch it drifting away from
# what the part can actually do without an evaluation board attached.
target_include_directories(app PRIVATE
	${AK4619_SAMPLE_DIR}/drivers
	${AK4619_SAMPLE_DIR}/src
	src
)
