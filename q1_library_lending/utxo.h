#ifndef UTXO_H
#define UTXO_H

#include "transaction.h"

#include <stdbool.h>

#define MAX_TX_INPUTS 32
#define MAX_TX_OUTPUTS 2

/* An unspent output: amount coins owned by owner, created by output_index of txid. */
typedef struct {
    char txid[HASH_HEX_SIZE];
    int output_index;
    char owner[ACCOUNT_ID_SIZE];
    int amount;
} Utxo;

typedef struct {
    Utxo *items;
    int count;
    int capacity;
} UtxoSet;

typedef struct {
    char txid[HASH_HEX_SIZE];
    int output_index;
} TxInput;

typedef struct {
    char owner[ACCOUNT_ID_SIZE];
    int amount;
} TxOutput;

/* A transaction in the UTXO model. Minted transactions have no inputs. */
typedef struct {
    char txid[HASH_HEX_SIZE];
    char sender[ACCOUNT_ID_SIZE];
    TxInput inputs[MAX_TX_INPUTS];
    int input_count;
    TxOutput outputs[MAX_TX_OUTPUTS];
    int output_count;
    int fee;
} UtxoTransaction;

bool utxo_build_transaction(const UtxoSet *set, const Transaction *tx, UtxoTransaction *utxo_tx);
bool utxo_validate(const UtxoSet *set, const UtxoTransaction *utxo_tx);
bool utxo_apply(UtxoSet *set, const UtxoTransaction *utxo_tx);
int utxo_balance(const UtxoSet *set, const char *owner);
void utxo_print_set(const UtxoSet *set);
void utxo_set_free(UtxoSet *set);

#endif
