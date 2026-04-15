/*
 * stock_server.c - baseline server lifecycle
 */
#include "csapp.h"
#include "stock_server.h"

#define STOCK_WORKER_COUNT 8
#define STOCK_QUEUE_SIZE 64
#define STOCK_QUEUE_STOP -1

typedef struct {
    int fds[STOCK_QUEUE_SIZE];
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} conn_queue_t;

static stock_status_t run_server_stub_internal(const char *port);
static stock_status_t conn_queue_init(conn_queue_t *queue);
static void conn_queue_destroy(conn_queue_t *queue);
static void conn_queue_insert(conn_queue_t *queue, int connfd);
static int conn_queue_remove(conn_queue_t *queue);
static stock_status_t create_workers(pthread_t *tids, conn_queue_t *queue,
                                     int *created);
static void join_workers(pthread_t *tids, int count);
static void stop_workers(conn_queue_t *queue, pthread_t *tids, int count);
static void *worker_main(void *arg);
static void handle_client(int connfd);

/*
 * TODO(baseline-shutdown): wire this to a future shutdown path. This step only
 * adds the flag/pill structure needed to let workers exit when shutdown starts.
 */
static volatile sig_atomic_t shutdown_requested = 0;

/*
 * Enters the server lifecycle entry point for one listen port.
 *
 * The wrapper keeps the public API stable while the internal server
 * implementation grows.
 */
stock_status_t run_server_stub(const char *port)
{
    return run_server_stub_internal(port);
}

/*
 * Runs the baseline accept loop backed by a fixed-size worker pool.
 *
 * The main thread only accepts connections and enqueues descriptors. Each
 * worker owns one client connection until that client disconnects.
 */
static stock_status_t run_server_stub_internal(const char *port)
{
    int listenfd;
    int connfd;
    int created;
    stock_status_t status;
    pthread_t tids[STOCK_WORKER_COUNT];
    conn_queue_t queue;

    status = conn_queue_init(&queue);
    if (status != STOCK_OK) {
        return status;
    }

    created = 0;
    status = create_workers(tids, &queue, &created);
    if (status != STOCK_OK) {
        stop_workers(&queue, tids, created);
        conn_queue_destroy(&queue);
        return status;
    }

    listenfd = Open_listenfd((char *)port);
    shutdown_requested = 0;
    Signal(SIGPIPE, SIG_IGN);

    while (!shutdown_requested) {
        connfd = Accept(listenfd, NULL, NULL);
        conn_queue_insert(&queue, connfd);
    }

    stop_workers(&queue, tids, created);
    Close(listenfd);
    conn_queue_destroy(&queue);
    return STOCK_OK;
}

/*
 * Initializes the bounded connection queue with 64 slots.
 *
 * slots tracks free capacity, items tracks pending descriptors, and mutex
 * protects the circular-buffer indexes.
 */
static stock_status_t conn_queue_init(conn_queue_t *queue)
{
    queue->front = 0;
    queue->rear = 0;

    if (sem_init(&queue->mutex, 0, 1) < 0) {
        fprintf(stderr, "sem_init mutex failed: %s\n", strerror(errno));
        return STOCK_ERR_INTERNAL;
    }
    if (sem_init(&queue->slots, 0, STOCK_QUEUE_SIZE) < 0) {
        fprintf(stderr, "sem_init slots failed: %s\n", strerror(errno));
        sem_destroy(&queue->mutex);
        return STOCK_ERR_INTERNAL;
    }
    if (sem_init(&queue->items, 0, 0) < 0) {
        fprintf(stderr, "sem_init items failed: %s\n", strerror(errno));
        sem_destroy(&queue->slots);
        sem_destroy(&queue->mutex);
        return STOCK_ERR_INTERNAL;
    }

    return STOCK_OK;
}

/*
 * Releases semaphore resources owned by the queue.
 */
static void conn_queue_destroy(conn_queue_t *queue)
{
    sem_destroy(&queue->items);
    sem_destroy(&queue->slots);
    sem_destroy(&queue->mutex);
}

/*
 * Inserts one descriptor, blocking when the bounded queue is full.
 */
static void conn_queue_insert(conn_queue_t *queue, int connfd)
{
    P(&queue->slots);
    P(&queue->mutex);
    queue->fds[queue->rear] = connfd;
    queue->rear = (queue->rear + 1) % STOCK_QUEUE_SIZE;
    V(&queue->mutex);
    V(&queue->items);
}

/*
 * Removes one descriptor, blocking when the bounded queue is empty.
 */
static int conn_queue_remove(conn_queue_t *queue)
{
    int connfd;

    P(&queue->items);
    P(&queue->mutex);
    connfd = queue->fds[queue->front];
    queue->front = (queue->front + 1) % STOCK_QUEUE_SIZE;
    V(&queue->mutex);
    V(&queue->slots);

    return connfd;
}

/*
 * Starts the fixed-size worker pool.
 */
static stock_status_t create_workers(pthread_t *tids, conn_queue_t *queue,
                                     int *created)
{
    int i;
    int rc;

    for (i = 0; i < STOCK_WORKER_COUNT; i++) {
        rc = pthread_create(&tids[i], NULL, worker_main, queue);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
            return STOCK_ERR_INTERNAL;
        }
        (*created)++;
    }

    return STOCK_OK;
}

/*
 * Waits for all created workers to finish.
 */
static void join_workers(pthread_t *tids, int count)
{
    int i;
    int rc;

    for (i = 0; i < count; i++) {
        rc = pthread_join(tids[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join failed: %s\n", strerror(rc));
        }
    }
}

/*
 * Pushes one poison pill per worker and joins the pool.
 */
static void stop_workers(conn_queue_t *queue, pthread_t *tids, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        conn_queue_insert(queue, STOCK_QUEUE_STOP);
    }

    join_workers(tids, count);
}

/*
 * Worker loop: take one accepted client connection and serve it to completion.
 */
static void *worker_main(void *arg)
{
    int connfd;
    conn_queue_t *queue;

    queue = (conn_queue_t *)arg;
    for (;;) {
        connfd = conn_queue_remove(queue);
        if (connfd == STOCK_QUEUE_STOP) {
            break;
        }
        handle_client(connfd);
    }

    return NULL;
}

/*
 * Handles one client connection until the client closes it.
 *
 * TODO(baseline-protocol): parse show/buy/sell and send command-specific
 * responses.
 * TODO(baseline-persistence): append durable buy/sell log records before
 * returning success.
 */
static void handle_client(int connfd)
{
    ssize_t n;
    rio_t rio;
    char buf[MAXLINE];
    char response[] = "success\n";

    Rio_readinitb(&rio, connfd);
    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) > 0) {
        printf("request: %s", buf);
        fflush(stdout);
        Rio_writen(connfd, response, strlen(response));
    }

    Close(connfd);
}
