cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_i2s_in_node_tests)

# app.overlay and dts/bindings/ are picked up by Zephyr's standard application
# lookup - the application directory is always part of DTS_ROOT - so neither is
# listed here. The node arrives with the subsystem, which is a Zephyr module
# gated on CONFIG_AUDIO_PIPELINE_NODE_I2S_IN.
target_sources(app PRIVATE
	fake_i2s.c
	test_i2s_in_node.c
)
