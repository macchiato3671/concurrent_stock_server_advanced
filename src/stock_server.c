/*
 * stock_server.c - server lifecycle stubs
 */
#include "stock_server.h"

static stock_status_t run_server_stub_internal(const char *port);

/*
 * Enters the server lifecycle entry point for one listen port.
 *
 * The thin wrapper keeps the public API stable while the internal server
 * implementation grows beyond the current stub.
 */
stock_status_t run_server_stub(const char *port)
{
    return run_server_stub_internal(port);
}

/*
 * Placeholder for the future accept loop, queue, and worker pool bootstrap.
 *
 * The current baseline returns success without serving clients so startup and
 * shutdown persistence paths can be exercised independently.
 */
static stock_status_t run_server_stub_internal(const char *port)
{
    (void)port;

    /*
     * TODO(baseline-step2): initialize the bounded queue, create the pthread
     * worker pool, open the listen socket, and start the accept loop.
     */
    return STOCK_OK;
}
