add_executable(test_pipeline
    test_pipeline.c
)

target_link_libraries(test_pipeline
    PRIVATE
        audio_pipeline
        pthread
)

add_test(NAME test_sink_reports_eof
    COMMAND test_pipeline sink_reports_eof)
add_test(NAME test_pipeline_error_event
    COMMAND test_pipeline pipeline_error_event)
