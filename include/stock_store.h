#ifndef STOCK_STORE_H
#define STOCK_STORE_H

#include "csapp.h"
#include "stock.h"

typedef struct stock_item {
    int id;
    int left_stock;
    int price;
    struct stock_item *left;
    struct stock_item *right;
} stock_item_t;

typedef struct {
    stock_item_t *root;
    pthread_mutex_t mutex;
} stock_store_t;

typedef stock_status_t (*stock_visit_fn)(stock_item_t *item, void *arg);

/*
 * Initializes an empty in-memory store and its global mutex.
 *
 * Call this once before recovery or request handling touches the store.
 */
stock_status_t stock_store_init(stock_store_t *store);

/*
 * Releases every BST node and then destroys the store mutex.
 *
 * Caller must ensure no worker still holds or will acquire the mutex.
 */
void stock_store_destroy(stock_store_t *store);

/*
 * Looks up one item without taking the store mutex.
 *
 * Caller must ensure the BST is stable for the full lookup.
 */
stock_status_t stock_store_find_nolock(stock_store_t *store, int id,
                                       stock_item_t **item);

/*
 * Inserts a new item or replaces the current values for an existing id.
 *
 * Caller must ensure exclusive access because the BST may be rewired.
 */
stock_status_t stock_store_upsert_nolock(stock_store_t *store, int id,
                                         int left_stock, int price);

/*
 * Applies a signed stock delta to one existing item without locking.
 *
 * Returns not found or insufficient stock instead of partially updating state.
 */
stock_status_t stock_store_apply_delta_nolock(stock_store_t *store, int id,
                                              int delta);

/*
 * Visits the BST in sorted id order and stops on the first callback error.
 *
 * Caller must ensure the traversed subtree is not concurrently mutated.
 */
stock_status_t stock_store_inorder_walk(stock_item_t *node,
                                        stock_visit_fn visit, void *arg);

/*
 * Counts the current number of items under the store mutex.
 *
 * The returned count reflects one lock-protected traversal of the live tree.
 */
stock_status_t stock_store_count(stock_store_t *store, int *count);

#endif
