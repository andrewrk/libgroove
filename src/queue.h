typedef struct GrooveQueue {
    void *context;
    void (*cleanup)(void *obj); // defaults to freeing the memory
    void (*put)(struct GrooveQueue*, void *obj);
    void (*get)(struct GrooveQueue*, void *obj);
    void (*flush)(struct GrooveQueue*);
    int (*purge)(struct GrooveQueue*, void *obj);
    void *internals;
} GrooveQueue;

GrooveQueue * groove_queue_create();

void groove_queue_flush(GrooveQueue *queue);

void groove_queue_destroy(GrooveQueue *queue);

void groove_queue_abort(GrooveQueue *queue);

int groove_queue_put(GrooveQueue *queue, void *obj);

int groove_queue_get(GrooveQueue *queue, void **obj_ptr, int block);

void groove_queue_purge(GrooveQueue *queue);
