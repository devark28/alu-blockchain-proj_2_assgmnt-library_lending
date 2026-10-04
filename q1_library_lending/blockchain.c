#include "blockchain.h"
#include "transaction.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GENESIS_PREVIOUS_HASH "0000000000000000000000000000000000000000000000000000000000000000"
#define PAYLOAD_SIZE 512
#define SIGNATURE_HEX_SIZE (MAX_SIGNATURE_SIZE * 2 + 1)
#define HASH_INPUT_SIZE 1024
#define CHAIN_LINE_SIZE 1024
#define BLOCK_FIELD_COUNT 14
#define INITIAL_CAPACITY 16

bool block_list_append(BlockList *list, const Block *block)
{
    if (list->count == list->capacity) {
        int new_capacity = list->capacity == 0 ? INITIAL_CAPACITY : list->capacity * 2;
        Block *grown = realloc(list->blocks, (size_t)new_capacity * sizeof(Block));
        if (grown == NULL) {
            fprintf(stderr, "ERROR: out of memory\n");
            return false;
        }
        list->blocks = grown;
        list->capacity = new_capacity;
    }
    list->blocks[list->count++] = *block;
    return true;
}

void block_list_remove_first(BlockList *list)
{
    if (list->count == 0) {
        return;
    }
    memmove(&list->blocks[0], &list->blocks[1], (size_t)(list->count - 1) * sizeof(Block));
    list->count--;
}

void block_list_free(BlockList *list)
{
    free(list->blocks);
    list->blocks = NULL;
    list->count = 0;
    list->capacity = 0;
}

void create_genesis_record(Block *record)
{
    memset(record, 0, sizeof(*record));
    record->timestamp = time(NULL);
    strcpy(record->action, ACTION_GENESIS);
}

void create_lending_record(Block *record, const Book *book, const Member *member,
                           const char *action, int token_reward, const char *transaction_id)
{
    memset(record, 0, sizeof(*record));
    record->timestamp = time(NULL);
    strcpy(record->book_id, book->book_id);
    strcpy(record->book_title, book->title);
    strcpy(record->member_id, member->member_id);
    strcpy(record->member_name, member->full_name);
    snprintf(record->action, sizeof(record->action), "%s", action);
    record->token_reward = token_reward;
    snprintf(record->transaction_id, sizeof(record->transaction_id), "%s", transaction_id);
}

/*
 * The lending data the librarian signs, joined with '|' so that the fields
 * "AB" + "C" and "A" + "BC" can never produce the same bytes.
 */
static void build_signing_payload(const Block *block, char *payload, size_t size)
{
    snprintf(payload, size, "%lld|%s|%s|%s|%s|%s|%d|%s",
             (long long)block->timestamp,
             block->book_id, block->book_title,
             block->member_id, block->member_name,
             block->action, block->token_reward, block->transaction_id);
}

bool sign_block(Block *block, EVP_PKEY *private_key)
{
    char payload[PAYLOAD_SIZE];
    build_signing_payload(block, payload, sizeof(payload));

    if (!sign_data(private_key, payload, block->signature, &block->signature_len)) {
        fprintf(stderr, "ERROR: could not sign the %s record\n", block->action);
        return false;
    }
    return true;
}

bool block_signature_is_valid(const Block *block, EVP_PKEY *public_key)
{
    char payload[PAYLOAD_SIZE];
    build_signing_payload(block, payload, sizeof(payload));
    return verify_signature(public_key, payload, block->signature, block->signature_len);
}

/* Places a pending record on top of the chain so mining can start. */
void prepare_for_mining(Block *block, const BlockList *chain, int difficulty)
{
    block->index = chain->count;
    block->difficulty = difficulty;
    block->nonce = 0;
    if (chain->count == 0) {
        strcpy(block->previous_hash, GENESIS_PREVIOUS_HASH);
    } else {
        strcpy(block->previous_hash, chain->blocks[chain->count - 1].hash);
    }
}

/* SHA-256 over the mining fields, the signed lending data and the signature. */
void compute_block_hash(const Block *block, char output[HASH_HEX_SIZE])
{
    char payload[PAYLOAD_SIZE];
    char signature_hex[SIGNATURE_HEX_SIZE];
    char hash_input[HASH_INPUT_SIZE];

    build_signing_payload(block, payload, sizeof(payload));
    bytes_to_hex(block->signature, block->signature_len, signature_hex);
    snprintf(hash_input, sizeof(hash_input), "%d|%d|%lu|%s|%s|%s",
             block->index, block->difficulty, block->nonce, block->previous_hash, payload, signature_hex);
    sha256_hex(hash_input, output);
}

bool hash_meets_difficulty(const char *hash, int difficulty)
{
    for (int i = 0; i < difficulty; i++) {
        if (hash[i] != '0') {
            return false;
        }
    }
    return true;
}

/*
 * Proof of work: hash, check for the leading zeros, otherwise increment the
 * nonce and try again. Stops after max_attempts so pool and cloud miners can
 * work in rounds. attempts is increased by the number of hashes computed.
 */
bool search_nonce(Block *block, unsigned long max_attempts, unsigned long *attempts)
{
    for (unsigned long tried = 0; tried < max_attempts; tried++) {
        compute_block_hash(block, block->hash);
        (*attempts)++;
        if (hash_meets_difficulty(block->hash, block->difficulty)) {
            return true;
        }
        block->nonce++;
    }
    return false;
}

/*
 * A block without a reward must not reference a transaction. A block with a
 * reward must reference exactly the reward transaction rebuilt from its own
 * fields, so the reward amount and its transaction cannot drift apart.
 */
bool reward_matches_transaction(const Block *block)
{
    if (block->token_reward == 0) {
        return block->transaction_id[0] == '\0';
    }
    if (block->token_reward <= TRANSACTION_FEE) {
        return false;
    }

    Transaction reward;
    make_reward_transaction(&reward, block->member_id, block->token_reward, block->book_id, block->timestamp);
    return strcmp(reward.txid, block->transaction_id) == 0;
}

static const char *check_result(bool passed)
{
    return passed ? "OK" : "FAIL";
}

bool validate_chain(const BlockList *chain, EVP_PKEY *public_key, bool print_report)
{
    int first_bad_block = -1;

    for (int i = 0; i < chain->count; i++) {
        const Block *block = &chain->blocks[i];
        const char *expected_previous = (i == 0) ? GENESIS_PREVIOUS_HASH : chain->blocks[i - 1].hash;
        char recomputed_hash[HASH_HEX_SIZE];
        compute_block_hash(block, recomputed_hash);

        bool index_ok = block->index == i;
        bool hash_ok = strcmp(block->hash, recomputed_hash) == 0;
        bool link_ok = strcmp(block->previous_hash, expected_previous) == 0;
        bool work_ok = block->difficulty >= MIN_DIFFICULTY && block->difficulty <= MAX_DIFFICULTY
                       && hash_meets_difficulty(recomputed_hash, block->difficulty);
        bool signature_ok = block_signature_is_valid(block, public_key);
        bool reward_ok = reward_matches_transaction(block);
        bool block_ok = index_ok && hash_ok && link_ok && work_ok && signature_ok && reward_ok;

        if (!block_ok && first_bad_block == -1) {
            first_bad_block = i;
        }

        if (print_report) {
            printf("Block %-3d index %-4s hash %-4s link %-4s pow %-4s signature %-4s reward %-4s%s\n",
                   i, check_result(index_ok), check_result(hash_ok), check_result(link_ok),
                   check_result(work_ok), check_result(signature_ok), check_result(reward_ok),
                   block_ok ? "" : " <-- TAMPERED");
            if (!hash_ok) {
                printf("          stored hash     %s\n", block->hash);
                printf("          recomputed hash %s\n", recomputed_hash);
            }
            if (!link_ok) {
                printf("          previous_hash   %s\n", block->previous_hash);
                printf("          expected        %s\n", expected_previous);
            }
        }
    }

    if (print_report) {
        if (first_bad_block == -1) {
            printf("\nRESULT: chain is VALID (%d blocks, all hashes, links, proof of work, signatures and rewards verified)\n",
                   chain->count);
        } else {
            printf("\nRESULT: chain is INVALID, first tampered block is %d\n", first_bad_block);
        }
    }
    return first_bad_block == -1;
}

static bool write_block_line(FILE *file, const Block *block)
{
    char signature_hex[SIGNATURE_HEX_SIZE];
    bytes_to_hex(block->signature, block->signature_len, signature_hex);

    return fprintf(file, "%d|%lld|%s|%s|%s|%s|%s|%d|%s|%d|%lu|%s|%s|%s\n",
                   block->index, (long long)block->timestamp,
                   block->book_id, block->book_title,
                   block->member_id, block->member_name,
                   block->action, block->token_reward, block->transaction_id,
                   block->difficulty, block->nonce, block->previous_hash,
                   signature_hex, block->hash) >= 0;
}

bool append_block_to_file(const char *path, const Block *block)
{
    FILE *file = fopen(path, "a");
    if (file == NULL) {
        fprintf(stderr, "ERROR: cannot open %s for writing (%s)\n", path, strerror(errno));
        return false;
    }
    bool written = write_block_line(file, block);
    bool closed = fclose(file) == 0;
    if (!written || !closed) {
        fprintf(stderr, "ERROR: could not save block to %s\n", path);
        return false;
    }
    return true;
}

/*
 * Rewrites the whole file. The new content goes to a temporary file that
 * replaces the old one with rename(), so a crash never leaves a half
 * written file behind.
 */
bool save_blocks(const char *path, const BlockList *list)
{
    char temporary_path[256];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);

    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        fprintf(stderr, "ERROR: cannot open %s for writing (%s)\n", temporary_path, strerror(errno));
        return false;
    }

    bool written = true;
    for (int i = 0; i < list->count && written; i++) {
        written = write_block_line(file, &list->blocks[i]);
    }
    bool closed = fclose(file) == 0;

    if (!written || !closed || rename(temporary_path, path) != 0) {
        fprintf(stderr, "ERROR: could not save %s\n", path);
        remove(temporary_path);
        return false;
    }
    return true;
}

static bool parse_block_line(char *line, Block *block)
{
    char *fields[BLOCK_FIELD_COUNT];
    if (split_line(line, '|', fields, BLOCK_FIELD_COUNT) != BLOCK_FIELD_COUNT) {
        return false;
    }

    memset(block, 0, sizeof(*block));
    long long timestamp;
    int signature_len = hex_to_bytes(fields[12], block->signature, MAX_SIGNATURE_SIZE);

    if (sscanf(fields[0], "%d", &block->index) != 1
        || sscanf(fields[1], "%lld", &timestamp) != 1
        || !copy_field(block->book_id, sizeof(block->book_id), fields[2])
        || !copy_field(block->book_title, sizeof(block->book_title), fields[3])
        || !copy_field(block->member_id, sizeof(block->member_id), fields[4])
        || !copy_field(block->member_name, sizeof(block->member_name), fields[5])
        || !copy_field(block->action, sizeof(block->action), fields[6])
        || sscanf(fields[7], "%d", &block->token_reward) != 1
        || !copy_field(block->transaction_id, sizeof(block->transaction_id), fields[8])
        || sscanf(fields[9], "%d", &block->difficulty) != 1
        || sscanf(fields[10], "%lu", &block->nonce) != 1
        || !copy_field(block->previous_hash, sizeof(block->previous_hash), fields[11])
        || signature_len < 0
        || !copy_field(block->hash, sizeof(block->hash), fields[13])) {
        return false;
    }

    block->timestamp = (time_t)timestamp;
    block->signature_len = (unsigned int)signature_len;
    return true;
}

/* A missing file is not an error: it means nothing has been saved yet. */
bool load_blocks(const char *path, BlockList *list)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return true;
        }
        fprintf(stderr, "ERROR: cannot open %s (%s)\n", path, strerror(errno));
        return false;
    }

    char line[CHAIN_LINE_SIZE];
    int line_number = 0;
    bool loaded = true;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        Block block;
        if (!parse_block_line(line, &block)) {
            fprintf(stderr, "ERROR: %s line %d is not a valid block record\n", path, line_number);
            loaded = false;
            break;
        }
        if (!block_list_append(list, &block)) {
            loaded = false;
            break;
        }
    }
    fclose(file);
    return loaded;
}

/* The newest record for book_id in list, or NULL if the book never appears. */
const Block *find_latest_record(const BlockList *list, const char *book_id)
{
    for (int i = list->count - 1; i >= 0; i--) {
        if (strcmp(list->blocks[i].book_id, book_id) == 0) {
            return &list->blocks[i];
        }
    }
    return NULL;
}

void format_timestamp(time_t timestamp, char *buffer, size_t size)
{
    struct tm *local = localtime(&timestamp);
    if (local == NULL || strftime(buffer, size, "%Y-%m-%d %H:%M:%S", local) == 0) {
        snprintf(buffer, size, "%lld", (long long)timestamp);
    }
}

void print_block(const Block *block, EVP_PKEY *public_key)
{
    char time_text[32];
    format_timestamp(block->timestamp, time_text, sizeof(time_text));
    bool signature_ok = block_signature_is_valid(block, public_key);

    printf("Block %d  [signature %s]\n", block->index, signature_ok ? "VALID" : "INVALID");
    printf("  Time        %s\n", time_text);
    printf("  Action      %s\n", block->action);
    if (strcmp(block->action, ACTION_GENESIS) == 0) {
        printf("  Record      genesis block, no lending data\n");
    } else {
        printf("  Book        %s  %s\n", block->book_id, block->book_title);
        printf("  Member      %s  %s\n", block->member_id, block->member_name);
    }
    if (block->token_reward > 0) {
        printf("  Reward      %d coins, transaction %s\n", block->token_reward, block->transaction_id);
    } else {
        printf("  Reward      0 coins, no transaction\n");
    }
    printf("  Mining      difficulty %d, nonce %lu\n", block->difficulty, block->nonce);
    printf("  Prev hash   %s\n", block->previous_hash);
    printf("  Hash        %s\n\n", block->hash);
}

void print_pending_record(const Block *block, EVP_PKEY *public_key)
{
    char time_text[32];
    format_timestamp(block->timestamp, time_text, sizeof(time_text));
    bool signature_ok = block_signature_is_valid(block, public_key);

    printf("  %s  %-9s %-6s %-22.22s %-7s %-14.14s reward %-2d signature %s\n",
           time_text, block->action, block->book_id, block->book_title,
           block->member_id, block->member_name, block->token_reward,
           signature_ok ? "VALID" : "INVALID");
}
