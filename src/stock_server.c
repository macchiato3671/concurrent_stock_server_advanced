/*
 * stock_server.c - baseline server lifecycle
 */
#include "csapp.h"
#include "stock_server.h"

#include <limits.h>
#include <stdint.h>

#define STOCK_WORKER_COUNT 8
#define STOCK_QUEUE_SIZE 64
#define STOCK_QUEUE_STOP (-1)
#define STOCK_REQUEST_MAX_TOKENS 4

typedef struct {
    int fds[STOCK_QUEUE_SIZE];
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} conn_queue_t;

typedef enum {
    REQUEST_UNKNOWN = 0,
    REQUEST_SHOW,
    REQUEST_BUY,
    REQUEST_SELL,
    REQUEST_EXIT
} request_command_t;

typedef struct {
    request_command_t command;
    int argc;
    int id;
    int count;
    int parse_error;
} stock_request_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_builder_t;

static stock_status_t conn_queue_init(conn_queue_t *queue);
static void conn_queue_destroy(conn_queue_t *queue);
static void conn_queue_insert(conn_queue_t *queue, int connfd);
static int conn_queue_remove(conn_queue_t *queue);
static stock_status_t create_workers(pthread_t *tids, int *created);
static void join_workers(pthread_t *tids, int count);
static void stop_workers(conn_queue_t *queue, pthread_t *tids, int count);
static void request_shutdown(int signum);
static stock_status_t install_shutdown_handlers(void);
static void *worker_main(void *arg);
static void handle_client(int connfd);
static void parse_request(const char *line, stock_request_t *request);
static stock_status_t validate_request(const stock_request_t *request);
static stock_status_t execute_request(const stock_request_t *request,
                                      response_builder_t *response);
static stock_status_t handle_show_request(const stock_request_t *request,
                                          response_builder_t *response);
static stock_status_t handle_buy_sell_request(const stock_request_t *request);
static stock_status_t append_stock_line(stock_item_t *item, void *arg);
static int parse_int_token(const char *token, int *value);
static const char *status_response(stock_status_t status);
static void response_init(response_builder_t *response);
static void response_destroy(response_builder_t *response);
static stock_status_t response_append(response_builder_t *response,
                                      const char *text);
static stock_status_t response_appendf(response_builder_t *response,
                                       const char *format, ...);
static stock_status_t response_ensure_capacity(response_builder_t *response,
                                               size_t additional);

static volatile sig_atomic_t shutdown_requested = 0;
static conn_queue_t g_queue;
static stock_store_t *g_store;

/*
 * Runs the baseline accept loop backed by a fixed-size worker pool.
 *
 * The main thread only accepts connections and enqueues descriptors. Each
 * worker owns one client connection until that client disconnects.
 */
stock_status_t run_server_stub(const char *port, stock_store_t *store)
{
    int listenfd;
    int connfd;
    int created;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];
    stock_status_t status;
    pthread_t tids[STOCK_WORKER_COUNT];

    if (store == NULL) {
        return STOCK_ERR_INTERNAL;
    }

    status = conn_queue_init(&g_queue);
    if (status != STOCK_OK) {
        return status;
    }

    g_store = store;

    listenfd = Open_listenfd((char *)port);
    shutdown_requested = 0;
    Signal(SIGPIPE, SIG_IGN);
    status = install_shutdown_handlers();
    if (status != STOCK_OK) {
        Close(listenfd);
        conn_queue_destroy(&g_queue);
        g_store = NULL;
        return status;
    }

    created = 0;
    status = create_workers(tids, &created);
    if (status != STOCK_OK) {
        stop_workers(&g_queue, tids, created);
        Close(listenfd);
        conn_queue_destroy(&g_queue);
        g_store = NULL;
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
        conn_queue_insert(&g_queue, connfd);
    }

    stop_workers(&g_queue, tids, created);
    Close(listenfd);
    conn_queue_destroy(&g_queue);
    g_store = NULL;
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
static stock_status_t create_workers(pthread_t *tids, int *created)
{
    int i;
    int rc;

    for (i = 0; i < STOCK_WORKER_COUNT; i++) {
        rc = pthread_create(&tids[i], NULL, worker_main, NULL);
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

    (void)arg;
    for (;;) {
        connfd = conn_queue_remove(&g_queue);
        if (connfd == STOCK_QUEUE_STOP) {
            break;
        }
        handle_client(connfd);
    }

    return NULL;
}

/*
 * Handles one client connection until exit or EOF.
 *
 * Worker shutdown is controlled only by the queue poison pill. After this
 * connection closes, the worker returns to the queue for the next connfd.
 */
static void handle_client(int connfd)
{
    ssize_t n;
    rio_t rio;
    char buf[MAXLINE];
    stock_request_t request;
    response_builder_t response;
    stock_status_t status;
    stock_status_t response_status;

    Rio_readinitb(&rio, connfd);
    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) > 0) {
        printf("request: %s", buf);
        fflush(stdout);

        response_init(&response);
        parse_request(buf, &request);
        status = validate_request(&request);
        if (status == STOCK_OK) {
            status = execute_request(&request, &response);
        }
        if (status != STOCK_OK) {
            response.len = 0;
            response.data[0] = '\0';
            response_status = response_append(&response,
                                              status_response(status));
            if (response_status != STOCK_OK) {
                (void)response_append(&response, "internal error\n");
            }
        }

        Rio_writen(connfd, response.data, response.len);
        response_destroy(&response);

        if (request.command == REQUEST_EXIT && status == STOCK_OK) {
            break;
        }
    }

    Close(connfd);
}

/*
 * Splits one request line into a normalized request struct.
 */
static void parse_request(const char *line, stock_request_t *request)
{
    int argc;
    char local[MAXLINE];
    char *saveptr;
    char *token;
    char *tokens[STOCK_REQUEST_MAX_TOKENS + 1];

    request->command = REQUEST_UNKNOWN;
    request->argc = 0;
    request->id = 0;
    request->count = 0;
    request->parse_error = 0;

    strncpy(local, line, sizeof(local) - 1);
    local[sizeof(local) - 1] = '\0';

    argc = 0;
    token = strtok_r(local, " \t\r\n", &saveptr);
    while (token != NULL) {
        if (argc == STOCK_REQUEST_MAX_TOKENS) {
            request->parse_error = 1;
            break;
        }
        tokens[argc] = token;
        argc++;
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    request->argc = argc;
    if (argc == 0 || request->parse_error) {
        return;
    }

    if (strcmp(tokens[0], "show") == 0) {
        request->command = REQUEST_SHOW;
    } else if (strcmp(tokens[0], "buy") == 0) {
        request->command = REQUEST_BUY;
    } else if (strcmp(tokens[0], "sell") == 0) {
        request->command = REQUEST_SELL;
    } else if (strcmp(tokens[0], "exit") == 0) {
        request->command = REQUEST_EXIT;
    } else {
        return;
    }

    if (argc >= 2 && !parse_int_token(tokens[1], &request->id)) {
        request->parse_error = 1;
        return;
    }
    if (argc >= 3 && !parse_int_token(tokens[2], &request->count)) {
        request->parse_error = 1;
    }
}

/*
 * Applies command shape and argument validation after parsing.
 */
static stock_status_t validate_request(const stock_request_t *request)
{
    if (request->parse_error) {
        return STOCK_ERR_INVALID;
    }

    switch (request->command) {
    case REQUEST_SHOW:
        if (request->argc == 1 || request->argc == 2) {
            return STOCK_OK;
        }
        return STOCK_ERR_INVALID;
    case REQUEST_BUY:
    case REQUEST_SELL:
        if (request->argc == 3 && request->count > 0) {
            return STOCK_OK;
        }
        return STOCK_ERR_INVALID;
    case REQUEST_EXIT:
        if (request->argc == 1) {
            return STOCK_OK;
        }
        return STOCK_ERR_INVALID;
    default:
        return STOCK_ERR_INVALID;
    }
}

/*
 * Dispatches a validated request and writes the protocol response text.
 */
static stock_status_t execute_request(const stock_request_t *request,
                                      response_builder_t *response)
{
    stock_status_t status;

    if (g_store == NULL) {
        return STOCK_ERR_INTERNAL;
    }

    switch (request->command) {
    case REQUEST_SHOW:
        return handle_show_request(request, response);
    case REQUEST_BUY:
    case REQUEST_SELL:
        status = handle_buy_sell_request(request);
        if (status == STOCK_OK) {
            return response_append(response, "success\n");
        }
        return status;
    case REQUEST_EXIT:
        return response_append(response, "success\n");
    default:
        return STOCK_ERR_INVALID;
    }
}

/*
 * Builds either the full in-order stock listing or one stock row.
 */
static stock_status_t handle_show_request(const stock_request_t *request,
                                          response_builder_t *response)
{
    int rc;
    stock_item_t *item;
    stock_status_t status;

    rc = pthread_mutex_lock(&g_store->mutex);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(rc));
        return STOCK_ERR_INTERNAL;
    }

    if (request->argc == 1) {
        status = stock_store_inorder_walk(g_store->root, append_stock_line,
                                          response);
    } else {
        status = stock_store_find_nolock(g_store, request->id, &item);
        if (status == STOCK_OK) {
            status = response_appendf(response, "%d %d %d\n", item->id,
                                      item->left_stock, item->price);
        }
    }

    rc = pthread_mutex_unlock(&g_store->mutex);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_unlock failed: %s\n", strerror(rc));
        return STOCK_ERR_INTERNAL;
    }

    return status;
}

/*
 * Applies the baseline in-memory mutation for buy/sell.
 */
static stock_status_t handle_buy_sell_request(const stock_request_t *request)
{
    int rc;
    int delta;
    stock_status_t status;

    delta = request->count;
    if (request->command == REQUEST_BUY) {
        delta = -request->count;
    }

    rc = pthread_mutex_lock(&g_store->mutex);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(rc));
        return STOCK_ERR_INTERNAL;
    }

    status = stock_store_apply_delta_nolock(g_store, request->id, delta);

    rc = pthread_mutex_unlock(&g_store->mutex);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_unlock failed: %s\n", strerror(rc));
        return STOCK_ERR_INTERNAL;
    }

    return status;
}

/*
 * Appends one BST row using the protocol's show format.
 */
static stock_status_t append_stock_line(stock_item_t *item, void *arg)
{
    response_builder_t *response;

    response = (response_builder_t *)arg;
    return response_appendf(response, "%d %d %d\n", item->id,
                            item->left_stock, item->price);
}

/*
 * Converts one integer token without accepting trailing junk or overflow.
 */
static int parse_int_token(const char *token, int *value)
{
    long parsed;
    char *endptr;

    errno = 0;
    parsed = strtol(token, &endptr, 10);
    if (token == endptr || *endptr != '\0' || errno == ERANGE ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}

/*
 * Maps request statuses to protocol response strings.
 */
static const char *status_response(stock_status_t status)
{
    switch (status) {
    case STOCK_ERR_INVALID:
        return "invalid command\n";
    case STOCK_ERR_NOT_FOUND:
        return "stock not found\n";
    case STOCK_ERR_INSUFFICIENT:
        return "not enough left stock\n";
    case STOCK_ERR_IO:
    case STOCK_ERR_INTERNAL:
    default:
        return "internal error\n";
    }
}

/*
 * Initializes a response buffer.
 */
static void response_init(response_builder_t *response)
{
    response->cap = MAXLINE;
    response->len = 0;
    response->data = Malloc(response->cap);
    response->data[0] = '\0';
}

/*
 * Releases response storage.
 */
static void response_destroy(response_builder_t *response)
{
    Free(response->data);
    response->data = NULL;
    response->len = 0;
    response->cap = 0;
}

/*
 * Appends literal protocol text to a response.
 */
static stock_status_t response_append(response_builder_t *response,
                                      const char *text)
{
    size_t text_len;
    stock_status_t status;

    text_len = strlen(text);
    status = response_ensure_capacity(response, text_len);
    if (status != STOCK_OK) {
        return status;
    }

    memcpy(response->data + response->len, text, text_len + 1);
    response->len += text_len;
    return STOCK_OK;
}

/*
 * Appends printf-formatted protocol text to a response.
 */
static stock_status_t response_appendf(response_builder_t *response,
                                       const char *format, ...)
{
    int written;
    va_list args;
    va_list args_copy;
    stock_status_t status;

    va_start(args, format);
    va_copy(args_copy, args);
    written = vsnprintf(response->data + response->len,
                        response->cap - response->len, format, args);
    va_end(args);

    if (written < 0) {
        va_end(args_copy);
        return STOCK_ERR_INTERNAL;
    }

    if ((size_t)written >= response->cap - response->len) {
        status = response_ensure_capacity(response, (size_t)written);
        if (status != STOCK_OK) {
            va_end(args_copy);
            return status;
        }
        written = vsnprintf(response->data + response->len,
                            response->cap - response->len, format, args_copy);
        if (written < 0 ||
            (size_t)written >= response->cap - response->len) {
            va_end(args_copy);
            return STOCK_ERR_INTERNAL;
        }
    }

    va_end(args_copy);
    response->len += (size_t)written;
    return STOCK_OK;
}

/*
 * Ensures there is room for additional bytes plus a trailing NUL.
 */
static stock_status_t response_ensure_capacity(response_builder_t *response,
                                               size_t additional)
{
    size_t needed;
    size_t new_cap;

    if (additional > SIZE_MAX - response->len - 1) {
        return STOCK_ERR_INTERNAL;
    }

    needed = response->len + additional + 1;
    if (needed <= response->cap) {
        return STOCK_OK;
    }

    new_cap = response->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }

    response->data = Realloc(response->data, new_cap);
    response->cap = new_cap;
    return STOCK_OK;
}
