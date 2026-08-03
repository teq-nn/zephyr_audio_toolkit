cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_pipeline_no_file_nodes_tests)

# The subsystem arrives as a Zephyr module gated on CONFIG_AUDIO_PIPELINE and
# its nodes on one symbol each, so this suite lists only its own source: what
# it is here to check is which node objects the module contributes, and
# compiling any of them here would defeat that.
target_sources(app PRIVATE
	test_no_file_nodes.c
)
