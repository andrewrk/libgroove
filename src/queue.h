typedef struct GrooveQueue {
    void *context;
    // defaults to groove_queue_cleanup_default
    void (*cleanup)(struct GrooveQueue*, void *obj);
    void (*put)(struct GrooveQueue*, void *obj);
    void (*get)(struct GrooveQueue*, void *obj);
    int (*purge)(struct GrooveQueue*, void *obj);
    void *internals;
} GrooveQueue;

GrooveQueue * groove_queue_create();

void groove_queue_flush(GrooveQueue *queue);

void groove_queue_destroy(GrooveQueue *queue);

void groove_queue_abort(GrooveQueue *queue);

int groove_queue_put(GrooveQueue *queue, void *obj);

int groove_queue_get(GrooveQueue *queue, void **obj_ptr, int block);

int groove_queue_peek(GrooveQueue *queue, int block);

void groove_queue_purge(GrooveQueue *queue);

void groove_queue_cleanup_default(GrooveQueue *queue, void *obj);
