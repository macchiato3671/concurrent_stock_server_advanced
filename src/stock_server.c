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
static void request_shutdown(int signum);
static stock_status_t install_shutdown_handlers(void);
static void *worker_main(void *arg);
static void handle_client(int connfd);
static ssize_t stock_readline(rio_t *rio, char *buf, size_t maxlen);
static ssize_t stock_writen(int fd, const void *buf, size_t n);

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
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];
    stock_status_t status;
    pthread_t tids[STOCK_WORKER_COUNT];
    conn_queue_t queue;

    status = conn_queue_init(&queue);
    if (status != STOCK_OK) {
        return status;
    }

    listenfd = Open_listenfd((char *)port);
    shutdown_requested = 0;
    Signal(SIGPIPE, SIG_IGN);
    status = install_shutdown_handlers();
    if (status != STOCK_OK) {
        Close(listenfd);
        conn_queue_destroy(&queue);
        return status;
    }

    created = 0;
    status = create_workers(tids, &queue, &created);
    if (status != STOCK_OK) {
        stop_workers(&queue, tids, created);
        Close(listenfd);
        conn_queue_destroy(&queue);
        return status;
    }

    while (!shutdown_requested) {
        /*
         * Use raw accept() instead of CSAPP Accept() so EINTR can be handled
         * directly and the graceful shutdown cleanup path can run.
         */
        clientlen = sizeof(struct sockaddr_storage);
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (connfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            status = STOCK_ERR_INTERNAL;
            break;
        }

        if (getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE,
                        client_port, MAXLINE,
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            printf("Connected to (%s, %s)\n", client_hostname, client_port);
        }
        conn_queue_insert(&queue, connfd);
    }

    stop_workers(&queue, tids, created);
    Close(listenfd);
    conn_queue_destroy(&queue);
    return status;
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
 * SIGINT/SIGTERM handler for graceful server shutdown.
 */
static void request_shutdown(int signum)
{
    shutdown_requested = 1;
}

/*
 * Registers shutdown handlers without SA_RESTART so blocking accept() can
 * return EINTR and the main thread can enter its cleanup path.
 */
static stock_status_t install_shutdown_handlers(void)
{
    struct sigaction action;

    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, NULL) < 0) {
        fprintf(stderr, "sigaction SIGINT failed: %s\n", strerror(errno));
        return STOCK_ERR_INTERNAL;
    }
    if (sigaction(SIGTERM, &action, NULL) < 0) {
        fprintf(stderr, "sigaction SIGTERM failed: %s\n", strerror(errno));
        return STOCK_ERR_INTERNAL;
    }

    return STOCK_OK;
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
 * Handles one client connection until EOF or a per-client I/O error.
 *
 * Worker shutdown is controlled only by the queue poison pill. A client I/O
 * failure ends this session, closes connfd, and lets the worker serve the next
 * queued connection.
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

    rio_readinitb(&rio, connfd);
    while ((n = stock_readline(&rio, buf, MAXLINE)) > 0) {
        printf("request: %s", buf);
        fflush(stdout);
        if (stock_writen(connfd, response, strlen(response)) < 0) {
            break;
        }
    }

    if (n < 0) {
        fprintf(stderr, "client read failed: %s\n", strerror(errno));
    }
    if (close(connfd) < 0) {
        fprintf(stderr, "client close failed: %s\n", strerror(errno));
    }
}

/*
 * Avoid CSAPP's uppercase Rio_* wrappers in worker I/O: they call
 * unix_error() on failure, which would terminate the whole thread-pool server
 * for one broken client connection.
 */
static ssize_t stock_readline(rio_t *rio, char *buf, size_t maxlen)
{
    return rio_readlineb(rio, buf, maxlen);
}

/*
 * Writes the full buffer unless the peer disconnects or another fd-local error
 * occurs. EINTR is retried and partial writes advance through the buffer.
 */
static ssize_t stock_writen(int fd, const void *buf, size_t n)
{
    size_t nleft;
    ssize_t nwritten;
    const char *bufp;

    nleft = n;
    bufp = (const char *)buf;

    while (nleft > 0) {
        nwritten = write(fd, bufp, nleft);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "client write failed: %s\n", strerror(errno));
            return -1;
        }
        if (nwritten == 0) {
            errno = EPIPE;
            fprintf(stderr, "client write failed: %s\n", strerror(errno));
            return -1;
        }
        nleft -= nwritten;
        bufp += nwritten;
    }

    return (ssize_t)n;
}
