cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_i2s_wire_tests)

# The wire seam arrives with the subsystem, which is a Zephyr module gated on
# CONFIG_AUDIO_PIPELINE, so this suite lists only its own source. It needs no
# I2S device and no node: that the arithmetic is reachable without either is
# what makes it testable on native_sim at all.
target_sources(app PRIVATE
	test_i2s_wire.c
)
