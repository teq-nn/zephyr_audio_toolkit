cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ak4619_tests)

# The driver under test lives with the sample that owns it (issue #42), so the
# suite reaches across to it rather than the other way round. This is the only
# place that path is spelled out.
set(AK4619_DRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../samples/audio/ak4619_loopback/drivers)

target_sources(app PRIVATE
	${AK4619_DRIVER_DIR}/ak4619.c
	src/ak4619_emul.c
	src/test_ak4619.c
)

target_include_directories(app PRIVATE ${AK4619_DRIVER_DIR} src)
