/*
 * stock_store.c - in-memory stock store
 */
#include "stock_store.h"

static void mutex_lock_or_die(pthread_mutex_t *mutex);
static void mutex_unlock_or_die(pthread_mutex_t *mutex);
static stock_item_t *make_stock_item(int id, int left_stock, int price);
static stock_item_t *find_stock_item(stock_item_t *node, int id);
static stock_item_t *upsert_stock_item(stock_item_t *node, int id,
                                       int left_stock, int price);
static stock_status_t count_stock_item(stock_item_t *item, void *arg);
static void free_stock_tree(stock_item_t *node);

/*
 * Acquires a mutex and treats any pthread error as fatal.
 *
 * The store code uses this for invariants that should never fail at runtime.
 */
static void mutex_lock_or_die(pthread_mutex_t *mutex)
{
    int rc;

    rc = pthread_mutex_lock(mutex);
    if (rc != 0) {
        posix_error(rc, "pthread_mutex_lock error");
    }
}

/*
 * Releases a mutex and treats any pthread error as fatal.
 *
 * This keeps lock handoff failures from being silently ignored.
 */
static void mutex_unlock_or_die(pthread_mutex_t *mutex)
{
    int rc;

    rc = pthread_mutex_unlock(mutex);
    if (rc != 0) {
        posix_error(rc, "pthread_mutex_unlock error");
    }
}

/*
 * Maps internal status codes to stable human-readable names.
 *
 * Logs and startup errors use this instead of duplicating message strings.
 */
const char *stock_status_name(stock_status_t status)
{
    switch (status) {
    case STOCK_OK:
        return "ok";
    case STOCK_ERR_NOT_FOUND:
        return "not found";
    case STOCK_ERR_INSUFFICIENT:
        return "insufficient stock";
    case STOCK_ERR_INVALID:
        return "invalid data";
    case STOCK_ERR_IO:
        return "io error";
    case STOCK_ERR_INTERNAL:
        return "internal error";
    default:
        return "unknown";
    }
}

/*
 * Allocates one BST node with its child links cleared.
 *
 * Upsert uses this when a new stock id first enters the store.
 */
static stock_item_t *make_stock_item(int id, int left_stock, int price)
{
    stock_item_t *item;

    item = Malloc(sizeof(stock_item_t));
    item->id = id;
    item->left_stock = left_stock;
    item->price = price;
    item->left = NULL;
    item->right = NULL;

    return item;
}

/*
 * Searches the BST for one stock id.
 *
 * Caller must ensure the tree is not concurrently rewired during recursion.
 */
static stock_item_t *find_stock_item(stock_item_t *node, int id)
{
    if (node == NULL) {
        return NULL;
    }

    if (id < node->id) {
        return find_stock_item(node->left, id);
    }
    if (id > node->id) {
        return find_stock_item(node->right, id);
    }

    return node;
}

/*
 * Inserts or replaces one BST record while preserving id ordering.
 *
 * Existing nodes keep their position; only payload fields are overwritten.
 */
static stock_item_t *upsert_stock_item(stock_item_t *node, int id,
                                       int left_stock, int price)
{
    if (node == NULL) {
        return make_stock_item(id, left_stock, price);
    }

    if (id < node->id) {
        node->left = upsert_stock_item(node->left, id, left_stock, price);
        return node;
    }

    if (id > node->id) {
        node->right = upsert_stock_item(node->right, id, left_stock, price);
        return node;
    }

    node->left_stock = left_stock;
    node->price = price;
    return node;
}

/*
 * Prepares an empty store that recovery or request handlers can populate.
 *
 * On success the root is NULL and the mutex is ready for global store access.
 */
stock_status_t stock_store_init(stock_store_t *store)
{
    int rc;

    store->root = NULL;
    rc = pthread_mutex_init(&store->mutex, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(rc));
        return STOCK_ERR_INTERNAL;
    }

    return STOCK_OK;
}

/*
 * Tears down the full in-memory store during shutdown.
 *
 * Caller must ensure no concurrent access remains before this frees the tree
 * and destroys the mutex.
 */
void stock_store_destroy(stock_store_t *store)
{
    int rc;

    free_stock_tree(store->root);
    store->root = NULL;

    rc = pthread_mutex_destroy(&store->mutex);
    if (rc != 0) {
        posix_error(rc, "pthread_mutex_destroy error");
    }
}

/*
 * Returns the matching item pointer without taking the store mutex.
 *
 * Caller must ensure the store is already protected; the returned pointer is
 * only valid while that stability guarantee holds.
 */
stock_status_t stock_store_find_nolock(stock_store_t *store, int id,
                                       stock_item_t **item)
{
    *item = find_stock_item(store->root, id);
    if (*item == NULL) {
        return STOCK_ERR_NOT_FOUND;
    }

    return STOCK_OK;
}

/*
 * Inserts or replaces one record without acquiring the store mutex.
 *
 * This is used by recovery and other exclusive update paths that already own
 * whatever synchronization keeps the tree stable.
 */
stock_status_t stock_store_upsert_nolock(stock_store_t *store, int id,
                                         int left_stock, int price)
{
    store->root = upsert_stock_item(store->root, id, left_stock, price);
    return STOCK_OK;
}

/*
 * Applies a signed stock delta to an existing item without locking.
 *
 * Negative deltas fail with insufficient stock before mutation, so callers
 * never observe a partially applied buy/sell update.
 */
stock_status_t stock_store_apply_delta_nolock(stock_store_t *store, int id,
                                              int delta)
{
    stock_item_t *item;

    item = find_stock_item(store->root, id);
    if (item == NULL) {
        return STOCK_ERR_NOT_FOUND;
    }

    if (delta < 0 && item->left_stock < -delta) {
        return STOCK_ERR_INSUFFICIENT;
    }

    item->left_stock += delta;
    return STOCK_OK;
}

/*
 * Walks the BST in ascending id order and forwards each node to a callback.
 *
 * Traversal stops on the first callback error so persistence and counting code
 * can abort cleanly without continuing past a failed visit.
 */
stock_status_t stock_store_inorder_walk(stock_item_t *node,
                                        stock_visit_fn visit, void *arg)
{
    stock_status_t status;

    if (node == NULL) {
        return STOCK_OK;
    }

    status = stock_store_inorder_walk(node->left, visit, arg);
    if (status != STOCK_OK) {
        return status;
    }

    status = visit(node, arg);
    if (status != STOCK_OK) {
        return status;
    }

    return stock_store_inorder_walk(node->right, visit, arg);
}

/*
 * Counts one visited node for stock_store_count().
 *
 * The callback ignores node contents because only cardinality matters.
 */
static stock_status_t count_stock_item(stock_item_t *item, void *arg)
{
    int *count;

    (void)item;
    count = (int *)arg;
    (*count)++;
    return STOCK_OK;
}

/*
 * Returns the current item count under the store mutex.
 *
 * This gives startup logging a consistent snapshot of the live tree size.
 */
stock_status_t stock_store_count(stock_store_t *store, int *count)
{
    stock_status_t status;

    *count = 0;
    mutex_lock_or_die(&store->mutex);
    status = stock_store_inorder_walk(store->root, count_stock_item, count);
    mutex_unlock_or_die(&store->mutex);

    return status;
}

/*
 * Recursively releases every node in one BST subtree.
 *
 * Destruction is post-order so child memory is reclaimed before the parent.
 */
static void free_stock_tree(stock_item_t *node)
{
    if (node == NULL) {
        return;
    }

    free_stock_tree(node->left);
    free_stock_tree(node->right);
    Free(node);
}
