#ifndef STOCK_SERVER_H
#define STOCK_SERVER_H

#include "stock.h"
#include "stock_store.h"

/*
 * Starts the server lifecycle for the given listen port.
 *
 * The current baseline uses a bounded queue and a fixed-size worker pool.
 */
stock_status_t run_server_stub(const char *port, stock_store_t *store);

#endif
