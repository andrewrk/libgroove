#include "queue.h"

#include <libavutil/mem.h>
#include <SDL/SDL_thread.h>

typedef struct ItemList {
    void *obj;
    struct ItemList *next;
} ItemList;

typedef struct GrooveQueuePrivate {
    ItemList *first;
    ItemList *last;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int abort_request;
} GrooveQueuePrivate;

GrooveQueue * groove_queue_create() {
    GrooveQueuePrivate *q = av_mallocz(sizeof(GrooveQueuePrivate));
    GrooveQueue *queue = av_mallocz(sizeof(GrooveQueue));
    if (!q || !queue) {
        av_free(q);
        av_free(queue);
        return NULL;
    }
    queue->internals = q;
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_free(q);
        av_free(queue);
        return NULL;
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_free(q);
        av_free(queue);
        SDL_DestroyMutex(q->mutex);
        return NULL;
    }
    queue->cleanup = groove_queue_cleanup_default;
    return queue;
}

void groove_queue_flush(GrooveQueue *queue) {
    GrooveQueuePrivate *q = queue->internals;

    SDL_LockMutex(q->mutex);

    ItemList *el;
    ItemList *el1;
    for (el = q->first; el != NULL; el = el1) {
        el1 = el->next;
        if (queue->cleanup)
            queue->cleanup(queue, el->obj);
        av_free(el);
    }
    q->first = NULL;
    q->last = NULL;
    if (queue->flush)
        queue->flush(queue);
    SDL_UnlockMutex(q->mutex);
}

void groove_queue_destroy(GrooveQueue *queue) {
    groove_queue_flush(queue);
    GrooveQueuePrivate *q = queue->internals;
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
    av_free(q);
    av_free(queue);
}

void groove_queue_abort(GrooveQueue *queue) {
    GrooveQueuePrivate *q = queue->internals;

    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

int groove_queue_put(GrooveQueue *queue, void *obj) {
    ItemList * el1 = av_mallocz(sizeof(ItemList));

    if (!el1)
        return -1;

    el1->obj = obj;

    GrooveQueuePrivate *q = queue->internals;
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

int groove_queue_get(GrooveQueue *queue, void **obj_ptr, int block) {
    ItemList *ev1;
    int ret;

    GrooveQueuePrivate *q = queue->internals;
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

void groove_queue_purge(GrooveQueue *queue) {
    GrooveQueuePrivate *q = queue->internals;

    SDL_LockMutex(q->mutex);
    ItemList *node = q->first;
    ItemList *prev = NULL;
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
                ItemList *next = node->next;
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

void groove_queue_cleanup_default(GrooveQueue *queue, void *obj) {
    av_free(obj);
}

