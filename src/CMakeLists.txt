add_library(audio_pipeline STATIC
    audio_pipeline.c
)

target_include_directories(audio_pipeline
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/../include
)

target_link_libraries(audio_pipeline
    PUBLIC
        pthread
)
