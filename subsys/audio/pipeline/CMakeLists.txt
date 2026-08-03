zephyr_library()

zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_AUDIO_PIPELINE
	audio_pipeline_core.c
	audio_pipeline_config.c
	audio_pipeline_events.c
	audio_node_core.c
	audio_wav.c
)

# One symbol per shipped node, so a node nobody defines contributes no text.
# The list grows with the nodes; keep it one line per node.
zephyr_library_sources_ifdef(CONFIG_AUDIO_PIPELINE_NODE_FILE_READER nodes/file_reader_node.c)
zephyr_library_sources_ifdef(CONFIG_AUDIO_PIPELINE_NODE_FILE_WRITER nodes/file_writer_node.c)
zephyr_library_sources_ifdef(CONFIG_AUDIO_PIPELINE_NODE_GAIN_FILTER nodes/gain_filter_node.c)
zephyr_library_sources_ifdef(CONFIG_AUDIO_PIPELINE_NODE_NULL_SINK nodes/null_sink_node.c)
