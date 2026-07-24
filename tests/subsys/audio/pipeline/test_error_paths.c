#include <errno.h>

#include <zephyr/ztest.h>

#include <zephyr/audio/audio_pipeline.h>

ZTEST(audio_pipeline, test_invalid_config_rejected)
{
	struct audio_pipeline_config cfg = { 0 };

	zassert_false(audio_pipeline_config_is_valid(&cfg), "invalid config accepted");
}

ZTEST(audio_pipeline, test_lifecycle_rejects_null_pipeline)
{
	zassert_equal(audio_pipeline_start(NULL), -EINVAL, "start(NULL) accepted");
	zassert_equal(audio_pipeline_play(NULL), -EINVAL, "play(NULL) accepted");
	zassert_equal(audio_pipeline_stop(NULL), -EINVAL, "stop(NULL) accepted");
	zassert_equal(audio_pipeline_join(NULL), -EINVAL, "join(NULL) accepted");
	zassert_false(audio_pipeline_is_running(NULL), "is_running(NULL) true");
	zassert_false(audio_pipeline_is_playing(NULL), "is_playing(NULL) true");
}

ZTEST(audio_pipeline, test_lifecycle_rejects_uninitialised_pipeline)
{
	struct audio_pipeline pipeline = {0};

	zassert_equal(audio_pipeline_start(&pipeline), -EINVAL, "start before init accepted");
	zassert_equal(audio_pipeline_play(&pipeline), -EINVAL, "play before init accepted");
	zassert_equal(audio_pipeline_stop(&pipeline), -EINVAL, "stop before init accepted");
	zassert_equal(audio_pipeline_join(&pipeline), -EINVAL, "join before init accepted");
}
