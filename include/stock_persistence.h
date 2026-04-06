#ifndef STOCK_PERSISTENCE_H
#define STOCK_PERSISTENCE_H

#include "stock_store.h"

/*
 * Rebuilds the live store from the authoritative snapshot/AOF generation.
 *
 * Called during startup before any client traffic is accepted.
 */
stock_status_t recover_stock_store(stock_store_t *live_store);

/*
 * Writes a new snapshot/AOF generation and then makes it authoritative.
 *
 * Caller must ensure the store remains stable for the full materialization.
 */
stock_status_t materialize_store_generation(stock_store_t *store);

#endif
