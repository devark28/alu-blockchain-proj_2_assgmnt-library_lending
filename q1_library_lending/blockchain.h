#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include "crypto.h"
#include "registry.h"

#include <stdbool.h>
#include <time.h>

#define ACTION_SIZE 10

#define ACTION_GENESIS "GENESIS"
#define ACTION_BORROWED "BORROWED"
#define ACTION_RETURNED "RETURNED"
#define ACTION_OVERDUE "OVERDUE"

#define DEFAULT_DIFFICULTY 2
#define MIN_DIFFICULTY 1
#define MAX_DIFFICULTY 4

/*
 * token_reward and transaction_id are set when the record is created and
 * covered by the librarian's signature. index, difficulty, nonce and
 * previous_hash are only set when a miner confirms the block.
 */
typedef struct {
    int index;
    time_t timestamp;
    char book_id[20];
    char book_title[80];
    char member_id[20];
    char member_name[50];
    char action[ACTION_SIZE];
    int token_reward;
    char transaction_id[HASH_HEX_SIZE];
    int difficulty;
    unsigned long nonce;
    char previous_hash[HASH_HEX_SIZE];
    unsigned char signature[MAX_SIGNATURE_SIZE];
    unsigned int signature_len;
    char hash[HASH_HEX_SIZE];
} Block;

/* A growable array of blocks, used for both the chain and the pending pool. */
typedef struct {
    Block *blocks;
    int count;
    int capacity;
} BlockList;

bool block_list_append(BlockList *list, const Block *block);
void block_list_remove_first(BlockList *list);
void block_list_free(BlockList *list);

void create_genesis_record(Block *record);
void create_lending_record(Block *record, const Book *book, const Member *member,
                           const char *action, int token_reward, const char *transaction_id);
bool sign_block(Block *block, EVP_PKEY *private_key);
bool block_signature_is_valid(const Block *block, EVP_PKEY *public_key);

void prepare_for_mining(Block *block, const BlockList *chain, int difficulty);
void compute_block_hash(const Block *block, char output[HASH_HEX_SIZE]);
bool hash_meets_difficulty(const char *hash, int difficulty);
bool search_nonce(Block *block, unsigned long max_attempts, unsigned long *attempts);

bool reward_matches_transaction(const Block *block);
bool validate_chain(const BlockList *chain, EVP_PKEY *public_key, bool print_report);

bool append_block_to_file(const char *path, const Block *block);
bool save_blocks(const char *path, const BlockList *list);
bool load_blocks(const char *path, BlockList *list);

const Block *find_latest_record(const BlockList *list, const char *book_id);
void format_timestamp(time_t timestamp, char *buffer, size_t size);
void print_block(const Block *block, EVP_PKEY *public_key);
void print_pending_record(const Block *block, EVP_PKEY *public_key);

#endif
