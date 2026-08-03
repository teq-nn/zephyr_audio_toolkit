cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ak4619_loopback)

# The codec driver ships with the sample rather than with the toolkit (issue
# #42), so it is an application source. Its binding lives on the module's
# DTS_ROOT because the board overlay that declares the node is shared with the
# board test suites - see dts/bindings/audio/asahi-kasei,ak4619.yaml.
target_sources_ifdef(CONFIG_AK4619 app PRIVATE drivers/ak4619.c)

target_include_directories(app PRIVATE drivers)

target_sources(app PRIVATE src/main.c)
