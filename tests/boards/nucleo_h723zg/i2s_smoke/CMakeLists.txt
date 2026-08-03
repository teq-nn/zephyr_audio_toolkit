cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nucleo_h723zg_i2s_smoke)

# boards/nucleo_h723zg.overlay is picked up by Zephyr's standard board-overlay
# lookup, so it is not listed here; it includes the canonical overlay at
# dts/boards/nucleo_h723zg.overlay, whose bindings directory reaches this
# build through the module's dts_root setting. The subsystem arrives as a Zephyr module
# gated on CONFIG_AUDIO_PIPELINE, which prj.conf enables so this target also
# proves the module itself compiles for a Cortex-M7 board.
target_sources(app PRIVATE
	test_i2s_smoke.c
)
