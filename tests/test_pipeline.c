#include "audio_pipeline/audio_pipeline.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

struct generator_ctx {
	const int32_t *data;
	size_t samples;
	size_t offset;
};

static int generator_open(struct audio_node *node)
{
	struct generator_ctx *ctx = (struct generator_ctx *)node->context;

	ctx->offset = 0;

	return 0;
}

static ssize_t generator_process(struct audio_node *node, int32_t *buf,
				      size_t capacity, size_t *out_size)
{
	struct generator_ctx *ctx = (struct generator_ctx *)node->context;

	if (ctx->offset >= ctx->samples) {
		*out_size = 0;
		return 0;
	}

	size_t remaining = ctx->samples - ctx->offset;
	size_t to_copy = remaining > capacity ? capacity : remaining;
	memcpy(buf, ctx->data + ctx->offset, to_copy * sizeof(int32_t));
	ctx->offset += to_copy;
	*out_size = to_copy;

	return (ssize_t)to_copy;
}

static int generator_close(struct audio_node *node)
{
	(void)node;
	return 0;
}

static const struct audio_node_ops generator_ops = {
	.open = generator_open,
	.process = generator_process,
	.close = generator_close,
};

struct sink_ctx {
	int32_t *dest;
	size_t capacity;
	size_t written;
	bool fail_on_first;
};

static int sink_open(struct audio_node *node)
{
	struct sink_ctx *ctx = (struct sink_ctx *)node->context;

	ctx->written = 0;

	return 0;
}

static ssize_t sink_process(struct audio_node *node, int32_t *buf,
			      size_t capacity, size_t *out_size)
{
	struct sink_ctx *ctx = (struct sink_ctx *)node->context;

	if (ctx->fail_on_first && ctx->written == 0) {
		return -EIO;
	}

	ssize_t ret = node->upstream->ops->process(node->upstream, buf,
					 capacity, out_size);
	if (ret < 0) {
		return ret;
	}

	if (*out_size == 0) {
		return 0;
	}

	if (ctx->written + *out_size > ctx->capacity) {
		return -ENOMEM;
	}

	memcpy(ctx->dest + ctx->written, buf, *out_size * sizeof(int32_t));
	ctx->written += *out_size;

	return ret;
}

static int sink_close(struct audio_node *node)
{
	(void)node;
	return 0;
}

static const struct audio_node_ops sink_ops = {
	.open = sink_open,
	.process = sink_process,
	.close = sink_close,
};

static int test_sink_reports_eof(void)
{
	static const int32_t samples[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	static int32_t captured[8] = { 0 };

	struct generator_ctx gen_ctx = {
		.data = samples,
		.samples = ARRAY_SIZE(samples),
	};
	struct sink_ctx sink_ctx = {
		.dest = captured,
		.capacity = ARRAY_SIZE(captured),
	};
	struct audio_node source = {
		.role = AUDIO_NODE_ROLE_SOURCE,
		.ops = &generator_ops,
		.context = &gen_ctx,
	};
	struct audio_node sink = {
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &sink_ops,
		.context = &sink_ctx,
	};

	AUDIO_PIPELINE_DEFINE(pl, 4, 1024, 5);
	int ret = audio_pipeline_init(&pl);
	assert(ret == 0);

	ret = audio_pipeline_set_nodes(&pl, &source, NULL, 0, &sink);
	assert(ret == 0);

	struct audio_format fmt = {
		.sample_rate = 48000,
		.channels = AUDIO_PIPELINE_CHANNELS,
		.valid_bits_per_sample = 24,
		.format = AUDIO_SAMPLE_FORMAT_S32_LE,
	};
	ret = audio_pipeline_set_format(&pl, &fmt);
	assert(ret == 0);

	ret = audio_pipeline_start(&pl);
	assert(ret == 0);

	ret = audio_pipeline_play(&pl);
	assert(ret == 0);

	struct audio_pipeline_event evt;
	ret = audio_pipeline_get_event(&pl, &evt, K_MSEC(2000));
	assert(ret == 0);
	assert(evt.type == AUDIO_PIPELINE_EVENT_EOF);
	assert(memcmp(samples, captured, sizeof(samples)) == 0);

	ret = audio_pipeline_join(&pl);
	assert(ret == 0);

	return 0;
}

static int test_pipeline_error_event(void)
{
	static const int32_t samples[] = { 9, 10, 11, 12 };
	static int32_t captured[4];

	struct generator_ctx gen_ctx = {
		.data = samples,
		.samples = ARRAY_SIZE(samples),
	};
	struct sink_ctx sink_ctx = {
		.dest = captured,
		.capacity = ARRAY_SIZE(captured),
		.fail_on_first = true,
	};
	struct audio_node source = {
		.role = AUDIO_NODE_ROLE_SOURCE,
		.ops = &generator_ops,
		.context = &gen_ctx,
	};
	struct audio_node sink = {
		.role = AUDIO_NODE_ROLE_SINK,
		.ops = &sink_ops,
		.context = &sink_ctx,
	};

	AUDIO_PIPELINE_DEFINE(pl, 2, 1024, 5);
	int ret = audio_pipeline_init(&pl);
	assert(ret == 0);

	ret = audio_pipeline_set_nodes(&pl, &source, NULL, 0, &sink);
	assert(ret == 0);

	struct audio_pipeline_event evt;
	ret = audio_pipeline_start(&pl);
	assert(ret == 0);
	ret = audio_pipeline_play(&pl);
	assert(ret == 0);

	ret = audio_pipeline_get_event(&pl, &evt, K_MSEC(2000));
	assert(ret == 0);
	assert(evt.type == AUDIO_PIPELINE_EVENT_ERROR);

	ret = audio_pipeline_join(&pl);
	assert(ret == 0);

	return 0;
}

static void usage(void)
{
	printf("Usage: test_pipeline [sink_reports_eof|pipeline_error_event]\n");
}

int main(int argc, char **argv)
{
	int ret = 0;

	if (argc < 2) {
		ret |= test_sink_reports_eof();
		ret |= test_pipeline_error_event();
	} else if (strcmp(argv[1], "sink_reports_eof") == 0) {
		ret = test_sink_reports_eof();
	} else if (strcmp(argv[1], "pipeline_error_event") == 0) {
		ret = test_pipeline_error_event();
	} else {
		usage();
		return -1;
	}

	printf("All requested tests passed.\n");
	return ret;
}
