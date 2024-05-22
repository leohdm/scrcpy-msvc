#include "file_pusher.h"

#include <assert.h>
#include <string.h>

#include "adb/adb.h"
#include "util/log.h"
#include "util/process_intr.h"

#define DEFAULT_PUSH_TARGET "/sdcard/Download/"

static void
sc_file_pusher_request_destroy(struct sc_file_pusher_request *req) {
    free(req->file);
}

void file_push_sc_vecdeque_init(struct sc_file_pusher_request_queue* pv)
{
    (pv)->cap = 0;
    (pv)->origin = 0;
    (pv)->size = 0;
    (pv)->data = NULL;
}

bool
sc_file_pusher_init(struct sc_file_pusher *fp, const char *serial,
                    const char *push_target) {
    assert(serial);

    file_push_sc_vecdeque_init(&fp->queue);

    bool ok = sc_mutex_init(&fp->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_cond_init(&fp->event_cond);
    if (!ok) {
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    ok = sc_intr_init(&fp->intr);
    if (!ok) {
        sc_cond_destroy(&fp->event_cond);
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    fp->serial = _strdup(serial);
    if (!fp->serial) {
        LOG_OOM();
        sc_intr_destroy(&fp->intr);
        sc_cond_destroy(&fp->event_cond);
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    // lazy initialization
    fp->initialized = false;

    fp->stopped = false;

    fp->push_target = push_target ? push_target : DEFAULT_PUSH_TARGET;

    return true;
}

static struct sc_file_pusher_request_queue* file_pusher_sc_vecdeque_popref(struct sc_file_pusher_request_queue* pv)
{
    assert(!((pv)->size == 0));
    size_t pos = (pv)->origin;
    (pv)->origin = ((pv)->origin + 1) % (pv)->cap;
    --(pv)->size;
    &(pv)->data[pos];
    return pv->data;
}

void
sc_file_pusher_destroy(struct sc_file_pusher *fp) {
    sc_cond_destroy(&fp->event_cond);
    sc_mutex_destroy(&fp->mutex);
    sc_intr_destroy(&fp->intr);
    free(fp->serial);

    while (!sc_vecdeque_is_empty(&fp->queue)) {
        struct sc_file_pusher_request *req = file_pusher_sc_vecdeque_popref(&fp->queue);
        assert(req);
        sc_file_pusher_request_destroy(req);
    }
}

bool file_pusher_sc_vecdeque_realloc_(struct sc_file_pusher_request_queue* pv, size_t newcap)
{
    void* p = sc_vecdeque_reallocdata_((pv)->data, newcap,
        sizeof(*(pv)->data), &(pv)->cap,
        &(pv)->origin, (pv)->size);
    if (p) {
        (pv)->data = p;
    }
    return (bool)p;
}

bool file_pusher_sc_vecdeque_grow_(struct sc_file_pusher_request_queue* pv)
{
    bool ok;
    if ((pv)->cap < sc_vecdeque_max_cap_(pv)) {
        size_t newsize = sc_vecdeque_growsize_((pv)->cap);
        newsize = CLAMP(newsize, SC_VECDEQUE_MINCAP_,
            sc_vecdeque_max_cap_(pv));
        ok = file_pusher_sc_vecdeque_realloc_(pv, newsize);
    }
    else {
        ok = false;
    }
    return ok;
}

bool file_pusher_sc_vecdeque_grow_if_needed_(struct sc_file_pusher_request_queue* pv)
{
    return (!sc_vecdeque_is_full(pv) || file_pusher_sc_vecdeque_grow_(pv));
}

void file_pusher_sc_vecdeque_push_noresize(struct sc_file_pusher_request_queue* pv, const struct sc_file_pusher_request item)
{
    assert(!sc_vecdeque_is_full(pv));
    ++(pv)->size;
    (pv)->data[((pv)->origin + (pv)->size - 1) % (pv)->cap] = item;
}

bool file_pusher_sc_vecdeque_push(struct sc_file_pusher_request_queue* pv, struct sc_file_pusher_request item)
{
    bool ok = file_pusher_sc_vecdeque_grow_if_needed_(pv);
    if (ok) {
        file_pusher_sc_vecdeque_push_noresize(pv, item);
    }
    return ok;
}

bool
sc_file_pusher_request(struct sc_file_pusher *fp,
                       enum sc_file_pusher_action action, char *file) {
    // start file_pusher if it's used for the first time
    if (!fp->initialized) {
        if (!sc_file_pusher_start(fp)) {
            return false;
        }
        fp->initialized = true;
    }

    LOGI("Request to %s %s", action == SC_FILE_PUSHER_ACTION_INSTALL_APK
                                 ? "install" : "push",
                             file);
    struct sc_file_pusher_request req = {
        .action = action,
        .file = file,
    };

    sc_mutex_lock(&fp->mutex);
    bool was_empty = sc_vecdeque_is_empty(&fp->queue);
    bool res = file_pusher_sc_vecdeque_push(&fp->queue, req);
    if (!res) {
        LOG_OOM();
        sc_mutex_unlock(&fp->mutex);
        return false;
    }

    if (was_empty) {
        sc_cond_signal(&fp->event_cond);
    }
    sc_mutex_unlock(&fp->mutex);

    return true;
}
 struct sc_file_pusher_request delay_buffer_sc_vecdeque_pop(struct sc_file_pusher_request_queue* pv)
{
    assert(!sc_vecdeque_is_empty(pv));
    size_t pos = (pv)->origin;
    (pv)->origin = ((pv)->origin + 1) % (pv)->cap;
    --(pv)->size;
    &(pv)->data[pos];
    return *(pv->data);
}

static int
run_file_pusher(void *data) {
    struct sc_file_pusher *fp = data;
    struct sc_intr *intr = &fp->intr;

    const char *serial = fp->serial;
    assert(serial);

    const char *push_target = fp->push_target;
    assert(push_target);

    for (;;) {
        sc_mutex_lock(&fp->mutex);
        while (!fp->stopped && sc_vecdeque_is_empty(&fp->queue)) {
            sc_cond_wait(&fp->event_cond, &fp->mutex);
        }
        if (fp->stopped) {
            // stop immediately, do not process further events
            sc_mutex_unlock(&fp->mutex);
            break;
        }

        assert(!sc_vecdeque_is_empty(&fp->queue));
        struct sc_file_pusher_request req = delay_buffer_sc_vecdeque_pop(&fp->queue);
        sc_mutex_unlock(&fp->mutex);

        if (req.action == SC_FILE_PUSHER_ACTION_INSTALL_APK) {
            LOGI("Installing %s...", req.file);
            bool ok = sc_adb_install(intr, serial, req.file, 0);
            if (ok) {
                LOGI("%s successfully installed", req.file);
            } else {
                LOGE("Failed to install %s", req.file);
            }
        } else {
            LOGI("Pushing %s...", req.file);
            bool ok = sc_adb_push(intr, serial, req.file, push_target, 0);
            if (ok) {
                LOGI("%s successfully pushed to %s", req.file, push_target);
            } else {
                LOGE("Failed to push %s to %s", req.file, push_target);
            }
        }

        sc_file_pusher_request_destroy(&req);
    }
    return 0;
}

bool
sc_file_pusher_start(struct sc_file_pusher *fp) {
    LOGD("Starting file_pusher thread");

    bool ok = sc_thread_create(&fp->thread, run_file_pusher, "scrcpy-file", fp);
    if (!ok) {
        LOGE("Could not start file_pusher thread");
        return false;
    }

    return true;
}

void
sc_file_pusher_stop(struct sc_file_pusher *fp) {
    if (fp->initialized) {
        sc_mutex_lock(&fp->mutex);
        fp->stopped = true;
        sc_cond_signal(&fp->event_cond);
        sc_intr_interrupt(&fp->intr);
        sc_mutex_unlock(&fp->mutex);
    }
}

void
sc_file_pusher_join(struct sc_file_pusher *fp) {
    if (fp->initialized) {
        sc_thread_join(&fp->thread, NULL);
    }
}
