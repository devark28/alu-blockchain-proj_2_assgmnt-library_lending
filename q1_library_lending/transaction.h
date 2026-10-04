#ifndef TRANSACTION_H
#define TRANSACTION_H

#include "crypto.h"

#include <stdbool.h>
#include <time.h>

#define ACCOUNT_ID_SIZE 20
#define TX_TYPE_SIZE 10
#define REFERENCE_SIZE 32

#define TX_REWARD "REWARD"
#define TX_MINING "MINING"
#define TX_TRANSFER "TRANSFER"

#define LIBRARY_ACCOUNT "LIBRARY"
#define COINBASE_ACCOUNT "COINBASE"

#define TRANSACTION_FEE 1
#define NO_NONCE (-1)

/*
 * One token movement, independent of the ledger model. amount is what the
 * recipient receives; fee is paid on top by the sender and burned. Minted
 * transactions (REWARD from LIBRARY, MINING from COINBASE) have no nonce.
 */
typedef struct {
    char txid[HASH_HEX_SIZE];
    char type[TX_TYPE_SIZE];
    char sender[ACCOUNT_ID_SIZE];
    char recipient[ACCOUNT_ID_SIZE];
    int amount;
    int fee;
    int nonce;
    char reference[REFERENCE_SIZE];
    time_t timestamp;
} Transaction;

typedef struct {
    Transaction *items;
    int count;
    int capacity;
} TransactionList;

void make_transaction(Transaction *tx, const char *type, const char *sender, const char *recipient,
                      int amount, int fee, int nonce, const char *reference, time_t timestamp);
void make_reward_transaction(Transaction *tx, const char *member_id, int token_reward,
                             const char *book_id, time_t timestamp);
void make_mining_transaction(Transaction *tx, const char *miner_id, int amount,
                             const char *reference, time_t timestamp);
void compute_transaction_id(const Transaction *tx, char output[HASH_HEX_SIZE]);

bool is_minting_transaction(const Transaction *tx);
bool is_valid_account_id(const char *id);
void format_nonce(int nonce, char *buffer, size_t size);

bool transaction_list_append(TransactionList *list, const Transaction *tx);
void transaction_list_free(TransactionList *list);

bool append_transaction_to_file(const char *path, const Transaction *tx);
bool load_transactions(const char *path, TransactionList *list);

#endif
