cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_pipeline_tests)

# The subsystem arrives as a Zephyr module (zephyr/module.yml) gated on
# CONFIG_AUDIO_PIPELINE, which prj.conf enables. The module's own CMakeLists
# exports include/ via zephyr_include_directories(), so the tests only list
# their own sources - compiling the subsystem here as well would give every
# symbol a second definition at link time.
target_sources(app PRIVATE
	test_lifecycle.c
	test_roundtrip.c
	test_error_paths.c
	test_static_define.c
	test_events.c
	test_file_reader.c
	test_file_writer.c
	fake_nodes.c
	wav_fixture.c
)
