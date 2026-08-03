cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nucleo_h723zg_i2s_in_node)

# boards/nucleo_h723zg.overlay is picked up by Zephyr's standard board-overlay
# lookup, so it is not listed here. The node arrives with the subsystem, which
# is a Zephyr module gated on CONFIG_AUDIO_PIPELINE_NODE_I2S_IN.
target_sources(app PRIVATE
	test_i2s_in_node.c
)
