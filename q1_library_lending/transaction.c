#include "transaction.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TX_PAYLOAD_SIZE 256
#define TX_LINE_SIZE 512
#define TX_FIELD_COUNT 9
#define INITIAL_CAPACITY 16

void make_transaction(Transaction *tx, const char *type, const char *sender, const char *recipient,
                      int amount, int fee, int nonce, const char *reference, time_t timestamp)
{
    memset(tx, 0, sizeof(*tx));
    snprintf(tx->type, sizeof(tx->type), "%s", type);
    snprintf(tx->sender, sizeof(tx->sender), "%s", sender);
    snprintf(tx->recipient, sizeof(tx->recipient), "%s", recipient);
    tx->amount = amount;
    tx->fee = fee;
    tx->nonce = nonce;
    snprintf(tx->reference, sizeof(tx->reference), "%s", reference);
    tx->timestamp = timestamp;
    compute_transaction_id(tx, tx->txid);
}

/*
 * The reward transaction is built only from data stored in the lending
 * block, so anyone holding the chain can rebuild it and check the block's
 * transaction_id. The fee is taken out of the reward before crediting.
 */
void make_reward_transaction(Transaction *tx, const char *member_id, int token_reward,
                             const char *book_id, time_t timestamp)
{
    make_transaction(tx, TX_REWARD, LIBRARY_ACCOUNT, member_id,
                     token_reward - TRANSACTION_FEE, TRANSACTION_FEE, NO_NONCE, book_id, timestamp);
}

void make_mining_transaction(Transaction *tx, const char *miner_id, int amount,
                             const char *reference, time_t timestamp)
{
    make_transaction(tx, TX_MINING, COINBASE_ACCOUNT, miner_id, amount, 0, NO_NONCE, reference, timestamp);
}

void compute_transaction_id(const Transaction *tx, char output[HASH_HEX_SIZE])
{
    char payload[TX_PAYLOAD_SIZE];
    snprintf(payload, sizeof(payload), "%s|%s|%s|%d|%d|%d|%s|%lld",
             tx->type, tx->sender, tx->recipient, tx->amount, tx->fee,
             tx->nonce, tx->reference, (long long)tx->timestamp);
    sha256_hex(payload, output);
}

bool is_minting_transaction(const Transaction *tx)
{
    return strcmp(tx->sender, LIBRARY_ACCOUNT) == 0 || strcmp(tx->sender, COINBASE_ACCOUNT) == 0;
}

/* Letters, digits, '-' and '_' only, and never one of the system accounts. */
bool is_valid_account_id(const char *id)
{
    size_t length = strlen(id);
    if (length == 0 || length >= ACCOUNT_ID_SIZE) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!isalnum((unsigned char)id[i]) && id[i] != '-' && id[i] != '_') {
            return false;
        }
    }
    return strcmp(id, LIBRARY_ACCOUNT) != 0 && strcmp(id, COINBASE_ACCOUNT) != 0;
}

void format_nonce(int nonce, char *buffer, size_t size)
{
    if (nonce == NO_NONCE) {
        snprintf(buffer, size, "-");
    } else {
        snprintf(buffer, size, "%d", nonce);
    }
}

bool transaction_list_append(TransactionList *list, const Transaction *tx)
{
    if (list->count == list->capacity) {
        int new_capacity = list->capacity == 0 ? INITIAL_CAPACITY : list->capacity * 2;
        Transaction *grown = realloc(list->items, (size_t)new_capacity * sizeof(Transaction));
        if (grown == NULL) {
            fprintf(stderr, "ERROR: out of memory\n");
            return false;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = *tx;
    return true;
}

void transaction_list_free(TransactionList *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

bool append_transaction_to_file(const char *path, const Transaction *tx)
{
    FILE *file = fopen(path, "a");
    if (file == NULL) {
        fprintf(stderr, "ERROR: cannot open %s for writing (%s)\n", path, strerror(errno));
        return false;
    }

    int written = fprintf(file, "%s|%s|%s|%s|%d|%d|%d|%s|%lld\n",
                          tx->txid, tx->type, tx->sender, tx->recipient,
                          tx->amount, tx->fee, tx->nonce, tx->reference, (long long)tx->timestamp);
    bool closed = fclose(file) == 0;
    if (written < 0 || !closed) {
        fprintf(stderr, "ERROR: could not save transaction to %s\n", path);
        return false;
    }
    return true;
}

static bool parse_transaction_line(char *line, Transaction *tx)
{
    char *fields[TX_FIELD_COUNT];
    if (split_line(line, '|', fields, TX_FIELD_COUNT) != TX_FIELD_COUNT) {
        return false;
    }

    memset(tx, 0, sizeof(*tx));
    long long timestamp;
    if (!copy_field(tx->txid, sizeof(tx->txid), fields[0])
        || !copy_field(tx->type, sizeof(tx->type), fields[1])
        || !copy_field(tx->sender, sizeof(tx->sender), fields[2])
        || !copy_field(tx->recipient, sizeof(tx->recipient), fields[3])
        || sscanf(fields[4], "%d", &tx->amount) != 1
        || sscanf(fields[5], "%d", &tx->fee) != 1
        || sscanf(fields[6], "%d", &tx->nonce) != 1
        || !copy_field(tx->reference, sizeof(tx->reference), fields[7])
        || sscanf(fields[8], "%lld", &timestamp) != 1) {
        return false;
    }
    tx->timestamp = (time_t)timestamp;
    return true;
}

/*
 * A missing file means no transaction has been confirmed yet. Every line's
 * txid is recomputed, so an edited amount or recipient is refused here.
 */
bool load_transactions(const char *path, TransactionList *list)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return true;
        }
        fprintf(stderr, "ERROR: cannot open %s (%s)\n", path, strerror(errno));
        return false;
    }

    char line[TX_LINE_SIZE];
    int line_number = 0;
    bool loaded = true;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        Transaction tx;
        char expected_txid[HASH_HEX_SIZE];
        if (!parse_transaction_line(line, &tx)) {
            fprintf(stderr, "ERROR: %s line %d is not a valid transaction\n", path, line_number);
            loaded = false;
            break;
        }
        compute_transaction_id(&tx, expected_txid);
        if (strcmp(tx.txid, expected_txid) != 0) {
            fprintf(stderr, "ERROR: %s line %d was modified, its txid does not match its contents\n", path, line_number);
            loaded = false;
            break;
        }
        if (!transaction_list_append(list, &tx)) {
            loaded = false;
            break;
        }
    }
    fclose(file);
    return loaded;
}
