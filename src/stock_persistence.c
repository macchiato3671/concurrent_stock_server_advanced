/*
 * stock_persistence.c - recovery and persistence
 */
#include "stock_persistence.h"

#define STOCK_DATA_DIR "./data"
#define STOCK_INIT_PATH "./data/stock_init.txt"
#define STOCK_SNAPSHOT_PREFIX "./data/stock.snapshot"
#define STOCK_SNAPSHOT_NAME "stock.snapshot"
#define STOCK_AOF_PREFIX "./data/stock.aof"
#define STOCK_AOF_NAME "stock.aof"
#define STOCK_BOOTSTRAP_MARKER_PATH "./data/stock.bootstraping"
#define STOCK_BOOTSTRAP_MARKER_NAME "stock.bootstraping"
#define STOCK_MANIFEST_PATH "./data/stock.manifest"
#define STOCK_MANIFEST_NAME "stock.manifest"
#define STOCK_MANIFEST_TMP_PATH "./data/stock.manifest.tmp"
#define STOCK_MANIFEST_TMP_NAME "stock.manifest.tmp"
#define STOCK_LOG_OP_SIZE 16
#define STOCK_PATH_SIZE 256

typedef enum {
    STOCK_PARSE_OK = 0,
    STOCK_PARSE_SKIP,
    STOCK_PARSE_ERR
} stock_parse_status_t;

static int file_exists(const char *path);
static int is_blank_line(const char *line);
static int parse_generation_suffix(const char *name, const char *base_name,
                                   stock_gen_t *gen);
static stock_status_t build_generation_path(char *path, size_t path_size,
                                            const char *prefix,
                                            stock_gen_t gen);
static stock_status_t build_data_path(char *path, size_t path_size,
                                      const char *name);
static stock_status_t fsync_parent_dir(const char *path);
static stock_parse_status_t parse_stock_record_line(const char *line,
                                                    stock_record_t *record);
static stock_parse_status_t parse_aof_record_line(const char *line,
                                                  stock_log_record_t *record);
static stock_parse_status_t parse_manifest_line(const char *line,
                                                stock_manifest_t *manifest);
static stock_status_t apply_stock_record(stock_store_t *store,
                                         const stock_record_t *record);
static stock_status_t apply_aof_record(stock_store_t *store,
                                       const stock_log_record_t *record);
static stock_status_t load_stock_file_into_store(stock_store_t *store,
                                                 const char *path);
static stock_status_t replay_aof_into_store(stock_store_t *store,
                                            const char *path);
static stock_status_t read_manifest_generation(stock_gen_t *gen);
static stock_status_t create_bootstrap_marker(void);
static stock_status_t remove_bootstrap_marker(void);
static stock_status_t ensure_bootstrap_recovery_state(void);
static stock_status_t write_snapshot_node(stock_item_t *node, void *arg);
static stock_status_t save_snapshot_file(stock_store_t *store,
                                         const char *path);
static stock_status_t create_empty_aof_file(const char *path);
static stock_status_t write_manifest_file(stock_gen_t gen);
static stock_status_t cleanup_old_generation_files(stock_gen_t current_gen);

/*
 * Checks whether a path is present without distinguishing file types.
 *
 * Recovery uses this for coarse existence tests around manifest bootstrap.
 */
static int file_exists(const char *path)
{
    struct stat statbuf;

    return stat(path, &statbuf) == 0;
}

/*
 * Treats lines containing only whitespace as ignorable input.
 *
 * Parsers use this so blank lines do not become hard recovery failures.
 */
static int is_blank_line(const char *line)
{
    while (*line != '\0') {
        if (!isspace((unsigned char)*line)) {
            return 0;
        }
        line++;
    }

    return 1;
}

/*
 * Parses the trailing generation number from names like base.<gen>.
 *
 * Returns 1 only for positive generations so cleanup and recovery ignore
 * unrelated files in the data directory.
 */
static int parse_generation_suffix(const char *name, const char *base_name,
                                   stock_gen_t *gen)
{
    const char *gen_str;
    char *endptr;
    size_t base_len;
    long long value;

    base_len = strlen(base_name);
    if (strncmp(name, base_name, base_len) != 0) {
        return 0;
    }
    if (name[base_len] != '.') {
        return 0;
    }

    gen_str = name + base_len + 1;
    if (*gen_str == '\0') {
        return 0;
    }

    errno = 0;
    value = strtoll(gen_str, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || value <= 0) {
        return 0;
    }

    *gen = value;
    return 1;
}

/*
 * Formats one generation-qualified artifact path.
 *
 * The helper centralizes path construction so snapshot and AOF naming stay
 * consistent across recovery, publish, and cleanup code.
 */
static stock_status_t build_generation_path(char *path, size_t path_size,
                                            const char *prefix,
                                            stock_gen_t gen)
{
    int written;

    written = snprintf(path, path_size, "%s.%lld", prefix, gen);
    if (written < 0 || (size_t)written >= path_size) {
        return STOCK_ERR_INTERNAL;
    }

    return STOCK_OK;
}

/*
 * Builds a path under the data directory for directory scans and cleanup.
 *
 * This avoids open-coded buffer math when unlinking discovered artifacts.
 */
static stock_status_t build_data_path(char *path, size_t path_size,
                                      const char *name)
{
    int written;

    written = snprintf(path, path_size, "%s/%s", STOCK_DATA_DIR, name);
    if (written < 0 || (size_t)written >= path_size) {
        return STOCK_ERR_INTERNAL;
    }

    return STOCK_OK;
}

/*
 * fsyncs the parent directory that contains a published or removed artifact.
 *
 * This extends durability from file contents to the directory entry change
 * caused by create, rename, or unlink.
 */
static stock_status_t fsync_parent_dir(const char *path)
{
    char dir_path[STOCK_PATH_SIZE];
    char *slash;
    int fd;

    if (strlen(path) >= sizeof(dir_path)) {
        return STOCK_ERR_INTERNAL;
    }

    strcpy(dir_path, path);
    slash = strrchr(dir_path, '/');
    if (slash == NULL) {
        strcpy(dir_path, ".");
    }
    else if (slash == dir_path) {
        slash[1] = '\0';
    }
    else {
        *slash = '\0';
    }

    fd = open(dir_path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        return STOCK_ERR_IO;
    }

    if (fsync(fd) < 0) {
        Close(fd);
        return STOCK_ERR_IO;
    }

    Close(fd);
    return STOCK_OK;
}

/*
 * Parses one snapshot/init line into a stock record.
 *
 * Blank lines are skipped, and any extra trailing token makes the line
 * invalid so recovery never accepts ambiguous base state.
 */
static stock_parse_status_t parse_stock_record_line(const char *line,
                                                    stock_record_t *record)
{
    char trailing;
    int matched;

    if (is_blank_line(line)) {
        return STOCK_PARSE_SKIP;
    }

    matched = sscanf(line, " %d %d %d %c",
                     &record->id, &record->left_stock, &record->price,
                     &trailing);
    if (matched != 3) {
        return STOCK_PARSE_ERR;
    }

    return STOCK_PARSE_OK;
}

/*
 * Parses one append-only log line into a typed BUY or SELL operation.
 *
 * The parser rejects unknown verbs or trailing garbage before replay mutates
 * the in-memory store.
 */
static stock_parse_status_t parse_aof_record_line(const char *line,
                                                  stock_log_record_t *record)
{
    char op[STOCK_LOG_OP_SIZE];
    char trailing;
    int matched;

    if (is_blank_line(line)) {
        return STOCK_PARSE_SKIP;
    }

    matched = sscanf(line, " %15s %d %d %c",
                     op, &record->id, &record->count, &trailing);
    if (matched != 3) {
        return STOCK_PARSE_ERR;
    }

    if (!strcmp(op, "BUY")) {
        record->op = STOCK_LOG_BUY;
        return STOCK_PARSE_OK;
    }

    if (!strcmp(op, "SELL")) {
        record->op = STOCK_LOG_SELL;
        return STOCK_PARSE_OK;
    }

    return STOCK_PARSE_ERR;
}

/*
 * Parses the single authoritative manifest record.
 *
 * Only "gen <positive>" is accepted so recovery never guesses which
 * generation should be treated as authoritative.
 */
static stock_parse_status_t parse_manifest_line(const char *line,
                                                stock_manifest_t *manifest)
{
    char key[STOCK_LOG_OP_SIZE];
    char trailing;
    int matched;

    if (is_blank_line(line)) {
        return STOCK_PARSE_SKIP;
    }

    matched = sscanf(line, " %15s %lld %c", key, &manifest->gen, &trailing);
    if (matched != 2 || strcmp(key, "gen") != 0 || manifest->gen <= 0) {
        return STOCK_PARSE_ERR;
    }

    return STOCK_PARSE_OK;
}

/*
 * Applies one base stock record during snapshot or init loading.
 *
 * Duplicate ids and negative values are rejected because the base image must
 * describe a complete, internally consistent store.
 */
static stock_status_t apply_stock_record(stock_store_t *store,
                                         const stock_record_t *record)
{
    stock_item_t *item;

    if (record->id <= 0 || record->left_stock < 0 || record->price < 0) {
        return STOCK_ERR_INVALID;
    }

    if (stock_store_find_nolock(store, record->id, &item) == STOCK_OK) {
        return STOCK_ERR_INVALID;
    }

    return stock_store_upsert_nolock(store, record->id,
                                     record->left_stock, record->price);
}

/*
 * Replays one append-only log record onto the live generation image.
 *
 * Validation happens before mutation so malformed or impossible operations
 * stop recovery at the offending record.
 */
static stock_status_t apply_aof_record(stock_store_t *store,
                                       const stock_log_record_t *record)
{
    int delta;

    if (record->id <= 0 || record->count <= 0) {
        return STOCK_ERR_INVALID;
    }

    if (record->op == STOCK_LOG_BUY) {
        delta = -record->count;
    }
    else {
        delta = record->count;
    }

    return stock_store_apply_delta_nolock(store, record->id, delta);
}

/*
 * Loads a full stock image from disk into the provided store.
 *
 * Missing files map to not found, while malformed content aborts immediately
 * so recovery never continues from a partially trusted snapshot.
 */
static stock_status_t load_stock_file_into_store(stock_store_t *store,
                                                 const char *path)
{
    FILE *fp;
    char line[MAXLINE];
    int line_no;
    stock_record_t record;
    stock_parse_status_t parse_status;
    stock_status_t status;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "failed to open stock file %s: %s\n",
                path, strerror(errno));
        if (errno == ENOENT) {
            return STOCK_ERR_NOT_FOUND;
        }
        return STOCK_ERR_IO;
    }

    line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;

        parse_status = parse_stock_record_line(line, &record);
        if (parse_status == STOCK_PARSE_SKIP) {
            continue;
        }
        if (parse_status == STOCK_PARSE_ERR) {
            fprintf(stderr,
                    "malformed stock record in %s at line %d: %s",
                    path, line_no, line);
            fclose(fp);
            return STOCK_ERR_INVALID;
        }

        status = apply_stock_record(store, &record);
        if (status != STOCK_OK) {
            fprintf(stderr,
                    "invalid stock record in %s at line %d: %s",
                    path, line_no, line);
            fclose(fp);
            return status;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "failed while reading stock file %s\n", path);
        fclose(fp);
        return STOCK_ERR_IO;
    }

    fclose(fp);
    return STOCK_OK;
}

/*
 * Replays a generation's append-only log in file order.
 *
 * This is the recovery step that reconstructs committed state changes after
 * the authoritative snapshot for that generation.
 */
static stock_status_t replay_aof_into_store(stock_store_t *store,
                                            const char *path)
{
    FILE *fp;
    char line[MAXLINE];
    int line_no;
    stock_log_record_t record;
    stock_parse_status_t parse_status;
    stock_status_t status;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "failed to open append-only log %s: %s\n",
                path, strerror(errno));
        if (errno == ENOENT) {
            return STOCK_ERR_NOT_FOUND;
        }
        return STOCK_ERR_IO;
    }

    line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;

        parse_status = parse_aof_record_line(line, &record);
        if (parse_status == STOCK_PARSE_SKIP) {
            continue;
        }
        if (parse_status == STOCK_PARSE_ERR) {
            fprintf(stderr,
                    "malformed append-only log record in %s at line %d: %s",
                    path, line_no, line);
            fclose(fp);
            return STOCK_ERR_INVALID;
        }

        status = apply_aof_record(store, &record);
        if (status != STOCK_OK) {
            fprintf(stderr,
                    "append-only log replay failed in %s at line %d: %s",
                    path, line_no, line);
            fclose(fp);
            return status;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "failed while reading append-only log %s\n", path);
        fclose(fp);
        return STOCK_ERR_IO;
    }

    fclose(fp);
    return STOCK_OK;
}

/*
 * Reads the one authoritative generation number from the manifest.
 *
 * Missing manifest means bootstrap mode; malformed or duplicated records are
 * treated as invalid because recovery cannot safely choose a generation.
 */
static stock_status_t read_manifest_generation(stock_gen_t *gen)
{
    FILE *fp;
    char line[MAXLINE];
    int line_no;
    int seen_record;
    stock_manifest_t manifest;
    stock_parse_status_t parse_status;

    fp = fopen(STOCK_MANIFEST_PATH, "r");
    if (fp == NULL) {
        if (errno == ENOENT) {
            return STOCK_ERR_NOT_FOUND;
        }
        return STOCK_ERR_IO;
    }

    line_no = 0;
    seen_record = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;

        parse_status = parse_manifest_line(line, &manifest);
        if (parse_status == STOCK_PARSE_SKIP) {
            continue;
        }
        if (parse_status == STOCK_PARSE_ERR || seen_record) {
            fprintf(stderr, "malformed manifest at line %d: %s",
                    line_no, line);
            fclose(fp);
            return STOCK_ERR_INVALID;
        }

        seen_record = 1;
        *gen = manifest.gen;
    }

    if (ferror(fp)) {
        fclose(fp);
        return STOCK_ERR_IO;
    }

    fclose(fp);

    if (!seen_record) {
        return STOCK_ERR_INVALID;
    }

    return STOCK_OK;
}

/*
 * Publishes a bootstrap marker before first-generation materialization starts.
 *
 * The marker lets the next startup distinguish an interrupted bootstrap from a
 * clean "manifest not created yet" state.
 */
static stock_status_t create_bootstrap_marker(void)
{
    FILE *fp;

    fp = fopen(STOCK_BOOTSTRAP_MARKER_PATH, "w");
    if (fp == NULL) {
        return STOCK_ERR_IO;
    }

    if (fclose(fp) != 0) {
        unlink(STOCK_BOOTSTRAP_MARKER_PATH);
        return STOCK_ERR_IO;
    }

    return fsync_parent_dir(STOCK_BOOTSTRAP_MARKER_PATH);
}

/*
 * Removes the bootstrap marker after bootstrap cleanup or success.
 *
 * ENOENT is treated as success because the desired durable state is simply
 * "marker absent".
 */
static stock_status_t remove_bootstrap_marker(void)
{
    if (unlink(STOCK_BOOTSTRAP_MARKER_PATH) != 0) {
        if (errno == ENOENT) {
            return STOCK_OK;
        }
        return STOCK_ERR_IO;
    }

    return fsync_parent_dir(STOCK_BOOTSTRAP_MARKER_PATH);
}

/*
 * Verifies that bootstrap can legally start when no manifest exists.
 *
 * If generation artifacts remain without a manifest, recovery stops instead of
 * guessing which partially published files are authoritative.
 */
static stock_status_t ensure_bootstrap_recovery_state(void)
{
    DIR *dirp;
    struct dirent *entry;
    stock_gen_t gen;

    dirp = opendir(STOCK_DATA_DIR);
    if (dirp == NULL) {
        if (errno == ENOENT) {
            return STOCK_ERR_IO;
        }
        return STOCK_ERR_IO;
    }

    while ((entry = readdir(dirp)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        if (!strcmp(entry->d_name, STOCK_MANIFEST_TMP_NAME) ||
            parse_generation_suffix(entry->d_name, STOCK_SNAPSHOT_NAME, &gen) ||
            parse_generation_suffix(entry->d_name, STOCK_AOF_NAME, &gen)) {
            closedir(dirp);
            fprintf(stderr,
                    "manifest is missing but persistence artifacts still exist\n");
            return STOCK_ERR_INVALID;
        }
    }

    closedir(dirp);
    return STOCK_OK;
}

/*
 * Rebuilds the live store from the authoritative on-disk generation.
 *
 * Called only during startup, before client traffic begins. When the manifest
 * is missing, this performs bootstrap from the init file, materializes the
 * first generation, and uses the bootstrap marker to detect incomplete setup.
 */
stock_status_t recover_stock_store(stock_store_t *live_store)
{
    char snapshot_path[STOCK_PATH_SIZE];
    char aof_path[STOCK_PATH_SIZE];
    stock_gen_t active_gen;
    stock_status_t status;

    status = read_manifest_generation(&active_gen);
    if (status == STOCK_ERR_NOT_FOUND) {
        if (file_exists(STOCK_BOOTSTRAP_MARKER_PATH)) {
            status = cleanup_old_generation_files(0);
            if (status != STOCK_OK) {
                fprintf(stderr,
                        "failed to cleanup incomplete bootstrap artifacts: %s\n",
                        stock_status_name(status));
                return status;
            }

            status = remove_bootstrap_marker();
            if (status != STOCK_OK) {
                fprintf(stderr,
                        "failed to remove bootstrap marker: %s\n",
                        stock_status_name(status));
                return status;
            }
        }
        else {
            status = ensure_bootstrap_recovery_state();
            if (status != STOCK_OK) {
                return status;
            }
        }

        if (!file_exists(STOCK_INIT_PATH)) {
            fprintf(stderr, "neither %s nor %s exists\n",
                    STOCK_MANIFEST_PATH, STOCK_INIT_PATH);
            return STOCK_ERR_IO;
        }

        status = create_bootstrap_marker();
        if (status != STOCK_OK) {
            return status;
        }

        status = load_stock_file_into_store(live_store, STOCK_INIT_PATH);
        if (status != STOCK_OK) {
            return status;
        }

        status = materialize_store_generation(live_store);
        if (status != STOCK_OK) {
            return status;
        }

        return remove_bootstrap_marker();
    }

    if (status != STOCK_OK) {
        return status;
    }

    if (file_exists(STOCK_BOOTSTRAP_MARKER_PATH)) {
        status = remove_bootstrap_marker();
        if (status != STOCK_OK) {
            fprintf(stderr,
                    "warning: failed to remove stale bootstrap marker: %s\n",
                    stock_status_name(status));
        }
    }

    status = build_generation_path(snapshot_path, sizeof(snapshot_path),
                                   STOCK_SNAPSHOT_PREFIX, active_gen);
    if (status != STOCK_OK) {
        return status;
    }

    status = build_generation_path(aof_path, sizeof(aof_path),
                                   STOCK_AOF_PREFIX, active_gen);
    if (status != STOCK_OK) {
        return status;
    }

    status = load_stock_file_into_store(live_store, snapshot_path);
    if (status != STOCK_OK) {
        return status;
    }

    return replay_aof_into_store(live_store, aof_path);
}

/*
 * Serializes one BST node into the snapshot file format.
 *
 * The inorder traversal around this callback keeps snapshot records sorted by
 * stock id for deterministic recovery input.
 */
static stock_status_t write_snapshot_node(stock_item_t *node, void *arg)
{
    FILE *fp;

    fp = (FILE *)arg;
    if (fprintf(fp, "%d %d %d\n",
                node->id, node->left_stock, node->price) < 0) {
        return STOCK_ERR_IO;
    }

    return STOCK_OK;
}

/*
 * Writes a complete snapshot file for one generation and makes it durable.
 *
 * Caller must ensure the store is stable for the full traversal. On failure
 * this removes the partially written file so it is never reused as a snapshot.
 */
static stock_status_t save_snapshot_file(stock_store_t *store,
                                         const char *path)
{
    FILE *fp;
    int fd;
    stock_status_t status;

    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open snapshot file %s: %s\n",
                path, strerror(errno));
        return STOCK_ERR_IO;
    }

    status = stock_store_inorder_walk(store->root, write_snapshot_node, fp);
    if (status == STOCK_OK && fflush(fp) != 0) {
        status = STOCK_ERR_IO;
    }

    if (status != STOCK_OK) {
        fclose(fp);
        unlink(path);
        return status;
    }

    fd = fileno(fp);
    if (fd < 0 || fsync(fd) < 0) {
        fclose(fp);
        unlink(path);
        return STOCK_ERR_IO;
    }

    if (fclose(fp) != 0) {
        unlink(path);
        return STOCK_ERR_IO;
    }

    return fsync_parent_dir(path);
}

/*
 * Creates a durable empty append-only log for a new generation.
 *
 * Materialization needs this companion file before the manifest can point at
 * the generation as authoritative.
 */
static stock_status_t create_empty_aof_file(const char *path)
{
    FILE *fp;
    int fd;

    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open aof file %s: %s\n",
                path, strerror(errno));
        return STOCK_ERR_IO;
    }

    if (fflush(fp) != 0) {
        fclose(fp);
        unlink(path);
        return STOCK_ERR_IO;
    }

    fd = fileno(fp);
    if (fd < 0 || fsync(fd) < 0) {
        fclose(fp);
        unlink(path);
        return STOCK_ERR_IO;
    }

    if (fclose(fp) != 0) {
        unlink(path);
        return STOCK_ERR_IO;
    }

    return fsync_parent_dir(path);
}

/*
 * Publishes the next authoritative generation through a temp file and rename.
 *
 * The temp file is fsynced before rename, then the parent directory is fsynced
 * so recovery never observes a partially written manifest record.
 */
static stock_status_t write_manifest_file(stock_gen_t gen)
{
    FILE *fp;
    int fd;
    stock_status_t status;

    fp = fopen(STOCK_MANIFEST_TMP_PATH, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open manifest temp file %s: %s\n",
                STOCK_MANIFEST_TMP_PATH, strerror(errno));
        return STOCK_ERR_IO;
    }

    if (fprintf(fp, "gen %lld\n", gen) < 0 || fflush(fp) != 0) {
        fclose(fp);
        unlink(STOCK_MANIFEST_TMP_PATH);
        return STOCK_ERR_IO;
    }

    fd = fileno(fp);
    if (fd < 0 || fsync(fd) < 0) {
        fclose(fp);
        unlink(STOCK_MANIFEST_TMP_PATH);
        return STOCK_ERR_IO;
    }

    if (fclose(fp) != 0) {
        unlink(STOCK_MANIFEST_TMP_PATH);
        return STOCK_ERR_IO;
    }

    if (rename(STOCK_MANIFEST_TMP_PATH, STOCK_MANIFEST_PATH) != 0) {
        unlink(STOCK_MANIFEST_TMP_PATH);
        return STOCK_ERR_IO;
    }

    status = fsync_parent_dir(STOCK_MANIFEST_PATH);
    if (status != STOCK_OK) {
        return status;
    }

    return STOCK_OK;
}

/*
 * Removes stale snapshot/AOF generations after a newer one is authoritative.
 *
 * Cleanup is best-effort from the caller's perspective; files for current_gen
 * are preserved and manifest selection remains the source of truth.
 */
static stock_status_t cleanup_old_generation_files(stock_gen_t current_gen)
{
    DIR *dirp;
    struct dirent *entry;
    char path[STOCK_PATH_SIZE];
    stock_gen_t gen;
    stock_status_t status;

    dirp = opendir(STOCK_DATA_DIR);
    if (dirp == NULL) {
        return STOCK_ERR_IO;
    }

    while ((entry = readdir(dirp)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        status = build_data_path(path, sizeof(path), entry->d_name);
        if (status != STOCK_OK) {
            closedir(dirp);
            return status;
        }

        if (!strcmp(entry->d_name, STOCK_MANIFEST_TMP_NAME)) {
            if (unlink(path) != 0 && errno != ENOENT) {
                closedir(dirp);
                return STOCK_ERR_IO;
            }
            continue;
        }

        if (parse_generation_suffix(entry->d_name, STOCK_SNAPSHOT_NAME, &gen) ||
            parse_generation_suffix(entry->d_name, STOCK_AOF_NAME, &gen)) {
            if (gen == current_gen) {
                continue;
            }

            if (unlink(path) != 0 && errno != ENOENT) {
                closedir(dirp);
                return STOCK_ERR_IO;
            }
        }
    }

    closedir(dirp);
    return STOCK_OK;
}

/*
 * Publishes a fresh snapshot/AOF generation for the current in-memory store.
 *
 * Caller must ensure the store remains stable for the whole operation. The
 * manifest is updated last, so an unfinished publish never becomes authoritative.
 */
stock_status_t materialize_store_generation(stock_store_t *store)
{
    char snapshot_path[STOCK_PATH_SIZE];
    char aof_path[STOCK_PATH_SIZE];
    stock_gen_t current_gen;
    stock_gen_t next_gen;
    stock_status_t status;

    status = read_manifest_generation(&current_gen);
    if (status == STOCK_ERR_NOT_FOUND) {
        current_gen = 0;
    }
    else if (status != STOCK_OK) {
        return status;
    }

    next_gen = current_gen + 1;

    status = build_generation_path(snapshot_path, sizeof(snapshot_path),
                                   STOCK_SNAPSHOT_PREFIX, next_gen);
    if (status != STOCK_OK) {
        return status;
    }

    status = build_generation_path(aof_path, sizeof(aof_path),
                                   STOCK_AOF_PREFIX, next_gen);
    if (status != STOCK_OK) {
        return status;
    }

    status = save_snapshot_file(store, snapshot_path);
    if (status != STOCK_OK) {
        return status;
    }

    status = create_empty_aof_file(aof_path);
    if (status != STOCK_OK) {
        return status;
    }

    status = write_manifest_file(next_gen);
    if (status != STOCK_OK) {
        return status;
    }

    status = cleanup_old_generation_files(next_gen);
    if (status != STOCK_OK) {
        fprintf(stderr, "warning: failed to cleanup old generations: %s\n",
                stock_status_name(status));
    }

    return STOCK_OK;
}
