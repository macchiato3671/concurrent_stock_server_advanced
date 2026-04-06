#ifndef STOCK_H
#define STOCK_H

typedef long long stock_gen_t;

typedef enum {
    STOCK_OK = 0,
    STOCK_ERR_NOT_FOUND,
    STOCK_ERR_INSUFFICIENT,
    STOCK_ERR_INVALID,
    STOCK_ERR_IO,
    STOCK_ERR_INTERNAL
} stock_status_t;

typedef enum {
    STOCK_LOG_BUY = 0,
    STOCK_LOG_SELL
} stock_log_op_t;

typedef struct {
    int id;
    int left_stock;
    int price;
} stock_record_t;

typedef struct {
    stock_log_op_t op;
    int id;
    int count;
} stock_log_record_t;

typedef struct {
    stock_gen_t gen;
} stock_manifest_t;

/*
 * Maps a status code to the short message used in logs and user-facing errors.
 *
 * This keeps error reporting consistent across recovery, persistence, and
 * request handling paths.
 */
const char *stock_status_name(stock_status_t status);

#endif
