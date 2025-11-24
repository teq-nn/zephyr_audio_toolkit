#include <zephyr/ztest.h>

#include <zephyr/audio/audio_pipeline.h>

ZTEST(audio_pipeline, test_invalid_config_rejected)
{
	struct audio_pipeline_config cfg = { 0 };

	zassert_false(audio_pipeline_config_is_valid(&cfg), "invalid config accepted");
}
