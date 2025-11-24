#include "audio_pipeline/audio_pipeline.h"

#include <string.h>
#include <time.h>

static int audio_pipeline_enqueue_event(struct audio_pipeline *pl,
				     enum audio_pipeline_event_type type,
				     int err)
{
	struct audio_pipeline_event evt = {
		.type = type,
		.error = err,
	};

	return k_msgq_put(pl->event_queue, &evt, K_NO_WAIT);
}

static void audio_pipeline_close_nodes(struct audio_pipeline *pl)
{
	if (pl->sink && pl->sink->ops && pl->sink->ops->close) {
		pl->sink->ops->close(pl->sink);
	}

	for (size_t i = 0; pl->filters && i < pl->filter_count; i++) {
		struct audio_node *node = pl->filters[i];

		if (node && node->ops && node->ops->close) {
			node->ops->close(node);
		}
	}

	if (pl->source && pl->source->ops && pl->source->ops->close) {
		pl->source->ops->close(pl->source);
	}
}

static int audio_pipeline_open_nodes(struct audio_pipeline *pl)
{
	int ret;

	if (pl->source && pl->source->ops && pl->source->ops->open) {
		ret = pl->source->ops->open(pl->source);
		if (ret < 0) {
			return ret;
		}
	}

	for (size_t i = 0; pl->filters && i < pl->filter_count; i++) {
		struct audio_node *node = pl->filters[i];

		if (node && node->ops && node->ops->open) {
			ret = node->ops->open(node);
			if (ret < 0) {
				return ret;
			}
		}
	}

	if (pl->sink && pl->sink->ops && pl->sink->ops->open) {
		ret = pl->sink->ops->open(pl->sink);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int audio_pipeline_step(struct audio_pipeline *pl)
{
	size_t out_size = 0;
	ssize_t ret;

	ret = pl->sink->ops->process(pl->sink, pl->frame_buffer,
					 pl->frame_capacity, &out_size);
	if (ret < 0) {
		audio_pipeline_enqueue_event(pl, AUDIO_PIPELINE_EVENT_ERROR,
					       (int)ret);
		return -1;
	}

	if (out_size == 0) {
		audio_pipeline_enqueue_event(pl, AUDIO_PIPELINE_EVENT_EOF, 0);
		return 1;
	}

	return 0;
}

static int audio_pipeline_wait_for_play(struct audio_pipeline *pl)
{
	int ret = 0;

	while (!pl->playing && !pl->stop_request) {
		pthread_cond_wait(&pl->state_cond, &pl->state_lock);
	}

	if (pl->stop_request) {
		ret = -1;
	}

	return ret;
}

static void *audio_pipeline_thread(void *arg)
{
	struct audio_pipeline *pl = (struct audio_pipeline *)arg;

	pthread_mutex_lock(&pl->state_lock);
	while (!pl->stop_request) {
		if (!pl->playing) {
			if (audio_pipeline_wait_for_play(pl) < 0) {
				break;
			}
		}

		pthread_mutex_unlock(&pl->state_lock);

		int step = audio_pipeline_step(pl);

		pthread_mutex_lock(&pl->state_lock);
		if (step != 0) {
			pl->playing = false;
		}
	}

	pl->running = false;
	pthread_mutex_unlock(&pl->state_lock);

	return NULL;
}

int k_msgq_init(struct k_msgq *q, char *buffer, size_t msg_size, size_t max_msgs)
{
	if (!q || !buffer || msg_size == 0 || max_msgs == 0) {
		return -EINVAL;
	}

	memset(q, 0, sizeof(*q));
	q->buffer = buffer;
	q->msg_size = msg_size;
	q->max_msgs = max_msgs;
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->cond, NULL);

	return 0;
}

static int k_msgq_wait_common(struct k_msgq *q, pthread_cond_t *cond,
				 bool for_put, k_timeout_t timeout)
{
	int ret = 0;

	if (timeout == K_NO_WAIT) {
		return -EAGAIN;
	}

	if (timeout == K_FOREVER) {
		while ((for_put && q->count == q->max_msgs) ||
		       (!for_put && q->count == 0)) {
			pthread_cond_wait(cond, &q->lock);
		}
		return 0;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout / 1000;
	ts.tv_nsec += (timeout % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}

	while ((for_put && q->count == q->max_msgs) ||
	       (!for_put && q->count == 0)) {
		ret = pthread_cond_timedwait(cond, &q->lock, &ts);
		if (ret == ETIMEDOUT) {
			return -EAGAIN;
		}
	}

	return 0;
}

int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout)
{
	int ret = 0;

	if (!q || !data) {
		return -EINVAL;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count == q->max_msgs) {
		ret = k_msgq_wait_common(q, &q->cond, true, timeout);
		if (ret < 0) {
			pthread_mutex_unlock(&q->lock);
			return ret;
		}
	}

	char *dst = q->buffer + (q->write_idx * q->msg_size);
	memcpy(dst, data, q->msg_size);
	q->write_idx = (q->write_idx + 1U) % q->max_msgs;
	q->count++;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);

	return ret;
}

int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout)
{
	int ret = 0;

	if (!q || !data) {
		return -EINVAL;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count == 0) {
		ret = k_msgq_wait_common(q, &q->cond, false, timeout);
		if (ret < 0) {
			pthread_mutex_unlock(&q->lock);
			return ret;
		}
	}

	char *src = q->buffer + (q->read_idx * q->msg_size);
	memcpy(data, src, q->msg_size);
	q->read_idx = (q->read_idx + 1U) % q->max_msgs;
	q->count--;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->lock);

	return ret;
}

int audio_pipeline_init(struct audio_pipeline *pl)
{
	if (!pl || pl->initialized) {
		return -EINVAL;
	}

	int ret = k_msgq_init(pl->event_queue, pl->event_buffer,
				sizeof(struct audio_pipeline_event),
				pl->event_queue_len);
	if (ret < 0) {
		return ret;
	}

	memset(&pl->format, 0, sizeof(pl->format));
	pl->format.channels = AUDIO_PIPELINE_CHANNELS;
	pl->format.format = AUDIO_SAMPLE_FORMAT_S32_LE;

	pl->initialized = true;
	pl->thread_started = false;
	pl->running = false;
	pl->playing = false;
	pl->stop_request = false;
	pthread_mutex_init(&pl->state_lock, NULL);
	pthread_cond_init(&pl->state_cond, NULL);

	return 0;
}

int audio_pipeline_set_nodes(struct audio_pipeline *pl,
			     struct audio_node *source,
			     struct audio_node **filters,
			     size_t filter_count,
			     struct audio_node *sink)
{
	if (!pl || !source || !sink) {
		return -EINVAL;
	}

	pl->source = source;
	pl->filters = filters;
	pl->filter_count = filter_count;
	pl->sink = sink;

	for (size_t i = 0; i < filter_count; i++) {
		struct audio_node *node = filters[i];
		struct audio_node *up = (i == 0) ? source : filters[i - 1];

		if (!node) {
			return -EINVAL;
		}

		node->upstream = up;
	}

	sink->upstream = filter_count > 0 ? filters[filter_count - 1] : source;

	return 0;
}

int audio_pipeline_set_format(struct audio_pipeline *pl,
			  const struct audio_format *fmt)
{
	if (!pl || !fmt) {
		return -EINVAL;
	}

	pl->format = *fmt;

	return 0;
}

int audio_pipeline_start(struct audio_pipeline *pl)
{
	if (!pl || !pl->initialized) {
		return -EINVAL;
	}

	if (!pl->source || !pl->sink) {
		return -EINVAL;
	}

	int ret = audio_pipeline_open_nodes(pl);
	if (ret < 0) {
		return ret;
	}

	if (pl->thread_started) {
		return 0;
	}

	pl->stop_request = false;
	pl->running = true;
	ret = pthread_create(&pl->thread.thread, NULL, audio_pipeline_thread, pl);
	if (ret != 0) {
		pl->running = false;
		return -EIO;
	}

	pl->thread_started = true;

	return 0;
}

int audio_pipeline_play(struct audio_pipeline *pl)
{
	if (!pl || !pl->thread_started) {
		return -EINVAL;
	}

	pthread_mutex_lock(&pl->state_lock);
	pl->playing = true;
	pthread_cond_broadcast(&pl->state_cond);
	pthread_mutex_unlock(&pl->state_lock);

	return 0;
}

int audio_pipeline_stop(struct audio_pipeline *pl)
{
	if (!pl || !pl->thread_started) {
		return -EINVAL;
	}

	pthread_mutex_lock(&pl->state_lock);
	pl->playing = false;
	pthread_cond_broadcast(&pl->state_cond);
	pthread_mutex_unlock(&pl->state_lock);

	return 0;
}

int audio_pipeline_join(struct audio_pipeline *pl)
{
	if (!pl || !pl->thread_started) {
		return -EINVAL;
	}

	pthread_mutex_lock(&pl->state_lock);
	pl->stop_request = true;
	pthread_cond_broadcast(&pl->state_cond);
	pthread_mutex_unlock(&pl->state_lock);

	if (pl->running) {
		pthread_join(pl->thread.thread, NULL);
	}

	audio_pipeline_close_nodes(pl);
	pl->thread_started = false;

	return 0;
}

int audio_pipeline_get_event(struct audio_pipeline *pl,
			  struct audio_pipeline_event *evt,
			  k_timeout_t timeout)
{
	if (!pl || !evt) {
		return -EINVAL;
	}

	return k_msgq_get(pl->event_queue, evt, timeout);
}
