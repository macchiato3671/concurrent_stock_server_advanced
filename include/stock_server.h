#ifndef STOCK_SERVER_H
#define STOCK_SERVER_H

#include "stock.h"

/*
 * Starts the server lifecycle for the given listen port.
 *
 * This is where the accept loop and worker infrastructure will be attached.
 */
stock_status_t run_server_stub(const char *port);

#endif
