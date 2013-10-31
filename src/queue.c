#include "queue.h"

#include <libavutil/mem.h>
#include <SDL2/SDL_thread.h>

struct ItemList {
    void *obj;
    struct ItemList *next;
};

struct GrooveQueuePrivate {
    struct GrooveQueue externals;
    struct ItemList *first;
    struct ItemList *last;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int abort_request;
};

struct GrooveQueue *groove_queue_create() {
    struct GrooveQueuePrivate *q = av_mallocz(sizeof(struct GrooveQueuePrivate));
    if (!q)
        return NULL;

    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_free(q);
        return NULL;
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_free(q);
        SDL_DestroyMutex(q->mutex);
        return NULL;
    }
    struct GrooveQueue *queue = &q->externals;
    queue->cleanup = groove_queue_cleanup_default;
    return queue;
}

void groove_queue_flush(struct GrooveQueue *queue) {
    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;

    SDL_LockMutex(q->mutex);

    struct ItemList *el;
    struct ItemList *el1;
    for (el = q->first; el != NULL; el = el1) {
        el1 = el->next;
        if (queue->cleanup)
            queue->cleanup(queue, el->obj);
        av_free(el);
    }
    q->first = NULL;
    q->last = NULL;
    SDL_UnlockMutex(q->mutex);
}

void groove_queue_destroy(struct GrooveQueue *queue) {
    groove_queue_flush(queue);
    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
    av_free(q);
}

void groove_queue_abort(struct GrooveQueue *queue) {
    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;

    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

void groove_queue_reset(struct GrooveQueue *queue) {
    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;

    SDL_LockMutex(q->mutex);

    q->abort_request = 0;

    SDL_UnlockMutex(q->mutex);
}

int groove_queue_put(struct GrooveQueue *queue, void *obj) {
    struct ItemList * el1 = av_mallocz(sizeof(struct ItemList));

    if (!el1)
        return -1;

    el1->obj = obj;

    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;
    SDL_LockMutex(q->mutex);

    if (!q->last)
        q->first = el1;
    else
        q->last->next = el1;
    q->last = el1;

    if (queue->put)
        queue->put(queue, obj);

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
}

int groove_queue_peek(struct GrooveQueue *queue, int block) {
    int ret;

    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;
    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        if (q->first) {
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);
    return ret;
}

int groove_queue_get(struct GrooveQueue *queue, void **obj_ptr, int block) {
    struct ItemList *ev1;
    int ret;

    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;
    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        ev1 = q->first;
        if (ev1) {
            q->first = ev1->next;
            if (!q->first)
                q->last = NULL;

            if (queue->get)
                queue->get(queue, ev1->obj);

            *obj_ptr = ev1->obj;
            av_free(ev1);
            ret = 1;
            break;
        } else if(!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);
    return ret;
}

void groove_queue_purge(struct GrooveQueue *queue) {
    struct GrooveQueuePrivate *q = (struct GrooveQueuePrivate *) queue;

    SDL_LockMutex(q->mutex);
    struct ItemList *node = q->first;
    struct ItemList *prev = NULL;
    while (node) {
        if (queue->purge(queue, node->obj)) {
            if (prev) {
                prev->next = node->next;
                if (queue->cleanup)
                    queue->cleanup(queue, node->obj);
                av_free(node);
                node = prev->next;
                if (!node)
                    q->last = prev;
            } else {
                struct ItemList *next = node->next;
                if (queue->cleanup)
                    queue->cleanup(queue, node->obj);
                av_free(node);
                q->first = next;
                node = next;
                if (!node)
                    q->last = NULL;
            }
        } else {
            prev = node;
            node = node->next;
        }
    }
    SDL_UnlockMutex(q->mutex);
}

void groove_queue_cleanup_default(struct GrooveQueue *queue, void *obj) {
    av_free(obj);
}

