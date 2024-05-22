#include "delay_buffer.h"

#include <assert.h>
#include <stdlib.h>

#include <libavutil/avutil.h>
#include <libavformat/avformat.h>

#include "util/log.h"

#define SC_BUFFERING_NDEBUG // comment to debug

/** Downcast frame_sink to sc_delay_buffer */
#define DOWNCAST(SINK) container_of(SINK, struct sc_delay_buffer, frame_sink)

static bool
sc_delayed_frame_init(struct sc_delayed_frame* dframe, const AVFrame* frame) {
	dframe->frame = av_frame_alloc();
	if (!dframe->frame) {
		LOG_OOM();
		return false;
	}

	if (av_frame_ref(dframe->frame, frame)) {
		LOG_OOM();
		av_frame_free(&dframe->frame);
		return false;
	}

	return true;
}

static void
sc_delayed_frame_destroy(struct sc_delayed_frame* dframe) {
	av_frame_unref(dframe->frame);
	av_frame_free(&dframe->frame);
}

static struct sc_delayed_frame delay_buffer_sc_vecdeque_popref(struct sc_delayed_frame_queue* pv)
{
	assert(!((pv)->size == 0));
	size_t pos = (pv)->origin;
	(pv)->origin = ((pv)->origin + 1) % (pv)->cap;
	--(pv)->size;
	&(pv)->data[pos];
	return *(pv->data);
}
static struct sc_delayed_frame* delay_buffer_sc_vecdeque_pop(struct sc_delayed_frame_queue* pv)
{
	assert(!sc_vecdeque_is_empty(pv));
	size_t pos = (pv)->origin;
	(pv)->origin = ((pv)->origin + 1) % (pv)->cap;
	--(pv)->size;
	&(pv)->data[pos];
	return pv->data;
}

static int
run_buffering(void* data) {
	struct sc_delay_buffer* db = data;

	assert(db->delay > 0);

	for (;;) {
		sc_mutex_lock(&db->mutex);

		while (!db->stopped && sc_vecdeque_is_empty(&db->queue)) {
			sc_cond_wait(&db->queue_cond, &db->mutex);
		}

		if (db->stopped) {
			sc_mutex_unlock(&db->mutex);
			goto stopped;
		}

		struct sc_delayed_frame dframe = delay_buffer_sc_vecdeque_popref(&db->queue);

		sc_tick max_deadline = sc_tick_now() + db->delay;
		// PTS (written by the server) are expressed in microseconds
		sc_tick pts = SC_TICK_FROM_US(dframe.frame->pts);

		bool timed_out = false;
		while (!db->stopped && !timed_out) {
			sc_tick deadline = sc_clock_to_system_time(&db->clock, pts)
				+ db->delay;
			if (deadline > max_deadline) {
				deadline = max_deadline;
			}

			timed_out =
				!sc_cond_timedwait(&db->wait_cond, &db->mutex, deadline);
		}

		bool stopped = db->stopped;
		sc_mutex_unlock(&db->mutex);

		if (stopped) {
			sc_delayed_frame_destroy(&dframe);
			goto stopped;
		}

#ifndef SC_BUFFERING_NDEBUG
		LOGD("Buffering: %" PRItick ";%" PRItick ";%" PRItick,
			pts, dframe.push_date, sc_tick_now());
#endif

		bool ok = sc_frame_source_sinks_push(&db->frame_source, dframe.frame);
		sc_delayed_frame_destroy(&dframe);
		if (!ok) {
			LOGE("Delayed frame could not be pushed, stopping");
			sc_mutex_lock(&db->mutex);
			// Prevent to push any new frame
			db->stopped = true;
			sc_mutex_unlock(&db->mutex);
			goto stopped;
		}
	}

stopped:
	assert(db->stopped);

	// Flush queue
	while (!sc_vecdeque_is_empty(&db->queue)) {
		struct sc_delayed_frame* dframe = delay_buffer_sc_vecdeque_pop(&db->queue);
		sc_delayed_frame_destroy(dframe);
	}

	LOGD("Buffering thread ended");

	return 0;
}

void delay_buffer_sc_vecdeque_init(struct sc_delayed_frame_queue* pv)
{
	(pv)->cap = 0;
	(pv)->origin = 0;
	(pv)->size = 0;
	(pv)->data = NULL;
}

static bool
sc_delay_buffer_frame_sink_open(struct sc_frame_sink* sink,
	const AVCodecContext* ctx) {
	struct sc_delay_buffer* db = DOWNCAST(sink);
	(void)ctx;

	bool ok = sc_mutex_init(&db->mutex);
	if (!ok) {
		return false;
	}

	ok = sc_cond_init(&db->queue_cond);
	if (!ok) {
		goto error_destroy_mutex;
	}

	ok = sc_cond_init(&db->wait_cond);
	if (!ok) {
		goto error_destroy_queue_cond;
	}

	sc_clock_init(&db->clock);
	delay_buffer_sc_vecdeque_init(&db->queue);

	if (!sc_frame_source_sinks_open(&db->frame_source, ctx)) {
		goto error_destroy_wait_cond;
	}

	ok = sc_thread_create(&db->thread, run_buffering, "scrcpy-dbuf", db);
	if (!ok) {
		LOGE("Could not start buffering thread");
		goto error_close_sinks;
	}

	return true;

error_close_sinks:
	sc_frame_source_sinks_close(&db->frame_source);
error_destroy_wait_cond:
	sc_cond_destroy(&db->wait_cond);
error_destroy_queue_cond:
	sc_cond_destroy(&db->queue_cond);
error_destroy_mutex:
	sc_mutex_destroy(&db->mutex);

	return false;
}

static void
sc_delay_buffer_frame_sink_close(struct sc_frame_sink* sink) {
	struct sc_delay_buffer* db = DOWNCAST(sink);

	sc_mutex_lock(&db->mutex);
	db->stopped = true;
	sc_cond_signal(&db->queue_cond);
	sc_cond_signal(&db->wait_cond);
	sc_mutex_unlock(&db->mutex);

	sc_thread_join(&db->thread, NULL);

	sc_frame_source_sinks_close(&db->frame_source);

	sc_cond_destroy(&db->wait_cond);
	sc_cond_destroy(&db->queue_cond);
	sc_mutex_destroy(&db->mutex);
}

void delay_buffer_sc_vecdeque_push_noresize(struct sc_delayed_frame_queue* pv, const struct sc_delayed_frame item)
{
	assert(!sc_vecdeque_is_full(pv));
	++(pv)->size;
	(pv)->data[((pv)->origin + (pv)->size - 1) % (pv)->cap] = item;
}

bool delay_buffer_sc_vecdeque_realloc_(struct sc_delayed_frame_queue* pv, size_t newcap)
{
	void* p = sc_vecdeque_reallocdata_((pv)->data, newcap,
		sizeof(*(pv)->data), &(pv)->cap,
		&(pv)->origin, (pv)->size);
	if (p) {
		(pv)->data = p;
	}
	return (bool)p;
}

bool delay_buffer_sc_vecdeque_grow_(struct sc_delayed_frame_queue* pv)
{
	bool ok;
	if ((pv)->cap < sc_vecdeque_max_cap_(pv)) {
		size_t newsize = sc_vecdeque_growsize_((pv)->cap);
		newsize = CLAMP(newsize, SC_VECDEQUE_MINCAP_,
			sc_vecdeque_max_cap_(pv));
		ok = delay_buffer_sc_vecdeque_realloc_(pv, newsize);
	}
	else {
		ok = false;
	}
	return ok;
}

bool  delay_buffer_sc_vecdeque_grow_if_needed_(struct sc_delayed_frame_queue* pv)
{
	return (!sc_vecdeque_is_full(pv) || delay_buffer_sc_vecdeque_grow_(pv));
}

bool delay_buffer_sc_vecdeque_push(struct sc_delayed_frame_queue* pv, struct sc_delayed_frame item)
{
	bool ok = delay_buffer_sc_vecdeque_grow_if_needed_(pv);
	if (ok) {
		delay_buffer_sc_vecdeque_push_noresize(pv, item);
	}
	return ok;
}

static bool
sc_delay_buffer_frame_sink_push(struct sc_frame_sink* sink,
	const AVFrame* frame) {
	struct sc_delay_buffer* db = DOWNCAST(sink);

	sc_mutex_lock(&db->mutex);

	if (db->stopped) {
		sc_mutex_unlock(&db->mutex);
		return false;
	}

	sc_tick pts = SC_TICK_FROM_US(frame->pts);
	sc_clock_update(&db->clock, sc_tick_now(), pts);
	sc_cond_signal(&db->wait_cond);

	if (db->first_frame_asap && db->clock.range == 1) {
		sc_mutex_unlock(&db->mutex);
		return sc_frame_source_sinks_push(&db->frame_source, frame);
	}

	struct sc_delayed_frame dframe;
	bool ok = sc_delayed_frame_init(&dframe, frame);
	if (!ok) {
		sc_mutex_unlock(&db->mutex);
		return false;
	}

#ifndef SC_BUFFERING_NDEBUG
	dframe.push_date = sc_tick_now();
#endif

	ok = delay_buffer_sc_vecdeque_push(&db->queue, dframe);
	if (!ok) {
		sc_mutex_unlock(&db->mutex);
		LOG_OOM();
		return false;
	}

	sc_cond_signal(&db->queue_cond);

	sc_mutex_unlock(&db->mutex);

	return true;
}

void
sc_delay_buffer_init(struct sc_delay_buffer* db, sc_tick delay,
	bool first_frame_asap) {
	assert(delay > 0);

	db->delay = delay;
	db->first_frame_asap = first_frame_asap;

	sc_frame_source_init(&db->frame_source);

	static const struct sc_frame_sink_ops ops = {
		.open = sc_delay_buffer_frame_sink_open,
		.close = sc_delay_buffer_frame_sink_close,
		.push = sc_delay_buffer_frame_sink_push,
	};

	db->frame_sink.ops = &ops;
}
