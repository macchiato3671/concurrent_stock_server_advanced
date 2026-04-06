/*
 * main.c - baseline stock server bootstrap and recovery
 */
#include "csapp.h"
#include "stock.h"
#include "stock_store.h"
#include "stock_persistence.h"
#include "stock_server.h"

static void usage(char **argv);

/*
 * Bootstraps the store, performs startup recovery, runs the server, and
 * materializes the final generation on clean shutdown.
 *
 * The process exits immediately on initialization, recovery, or persistence
 * failure so the server never continues from an untrusted state.
 */
int main(int argc, char **argv)
{
    int stock_count;
    stock_status_t status;
    stock_store_t g_store;

    if (argc != 2) {
        usage(argv);
    }

    status = stock_store_init(&g_store);
    if (status != STOCK_OK) {
        fprintf(stderr, "failed to initialize stock store: %s\n",
                stock_status_name(status));
        return 1;
    }

    status = recover_stock_store(&g_store);
    if (status != STOCK_OK) {
        fprintf(stderr, "stock state recovery failed: %s\n",
                stock_status_name(status));
        stock_store_destroy(&g_store);
        return 1;
    }

    status = stock_store_count(&g_store, &stock_count);
    if (status != STOCK_OK) {
        fprintf(stderr, "failed to count stock items: %s\n",
                stock_status_name(status));
        stock_store_destroy(&g_store);
        return 1;
    }

    printf("stock store ready: %d items loaded\n", stock_count);

    status = run_server_stub(argv[1]);
    if (status != STOCK_OK) {
        stock_store_destroy(&g_store);
        return 1;
    }

    status = materialize_store_generation(&g_store);
    if (status != STOCK_OK) {
        fprintf(stderr, "failed to persist store on shutdown: %s\n",
                stock_status_name(status));
        stock_store_destroy(&g_store);
        return 1;
    }

    stock_store_destroy(&g_store);
    return 0;
}

/*
 * Prints the required CLI shape and terminates the process.
 *
 * Startup treats a missing port as a fatal configuration error.
 */
static void usage(char **argv)
{
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
}
