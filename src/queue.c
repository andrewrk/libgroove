#include "queue.h"

#include <libavutil/mem.h>
#include <SDL/SDL_thread.h>

typedef struct ItemList {
    void *obj;
    struct ItemList *next;
} ItemList;

typedef struct Queue {
    ItemList *first;
    ItemList *last;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int abort_request;
    int (*cleanup)(void *);
} Queue;

GrooveQueue * groove_queue_create(int (*cleanup)(void *)) {
    Queue *q = av_mallocz(sizeof(Queue));
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
    return q;
}

void groove_queue_flush(GrooveQueue *groove_queue) {
    Queue *q = groove_queue;

    SDL_LockMutex(q->mutex);

    ItemList *el;
    ItemList *el1;
    for (el = q->first; el != NULL; el = el1) {
        el1 = el->next;
        if (q->cleanup)
            q->cleanup(el->obj);
        av_free(el);
    }
    q->first = NULL;
    SDL_UnlockMutex(q->mutex);
}

void groove_queue_destroy(GrooveQueue *groove_queue) {
    groove_queue_flush(groove_queue);
    Queue *q = groove_queue;
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
    av_free(q);
}

void groove_queue_abort(GrooveQueue *groove_queue) {
    Queue *q = groove_queue;

    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

int groove_queue_put(GrooveQueue *groove_queue, void *obj) {
    ItemList * el1 = av_mallocz(sizeof(ItemList));

    if (!el1)
        return -1;

    el1->obj = obj;

    Queue *q = groove_queue;
    SDL_LockMutex(q->mutex);

    if (!q->last)
        q->first = el1;
    else
        q->last->next = el1;
    q->last = el1;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
}

int groove_queue_get(GrooveQueue *groove_queue, void **obj_ptr, int block) {
    ItemList *ev1;
    int ret;

    Queue *q = groove_queue;
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

int groove_queue_cleanup_free(void *obj) {
    av_free(obj);
    return 0;
}
