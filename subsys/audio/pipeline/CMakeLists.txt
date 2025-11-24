zephyr_library()

zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_AUDIO_PIPELINE
	audio_pipeline_core.c
	audio_pipeline_config.c
	audio_pipeline_events.c
	audio_node_core.c
	util/wav_parser.c
	nodes/file_reader_node.c
	nodes/file_writer_node.c
	nodes/gain_filter_node.c
	nodes/null_sink_node.c
)
