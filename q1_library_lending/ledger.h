#ifndef LEDGER_H
#define LEDGER_H

#include "account.h"
#include "registry.h"
#include "transaction.h"
#include "utxo.h"

#include <stdbool.h>

typedef enum {
    MODEL_UTXO,
    MODEL_ACCOUNT
} LedgerModel;

/*
 * Confirmed transactions are kept in order and applied to whichever model
 * was chosen for the session. The other model's structure stays empty.
 */
typedef struct {
    LedgerModel model;
    TransactionList confirmed;
    UtxoSet utxos;
    AccountTable accounts;
    bool has_last_transfer;
    Transaction last_transfer;
    UtxoTransaction last_utxo_transfer;
} Ledger;

const char *model_name(LedgerModel model);
bool ledger_init(Ledger *ledger, LedgerModel model, const Registry *registry);
bool ledger_load(Ledger *ledger, const char *path);
bool ledger_submit(Ledger *ledger, const Transaction *tx, const char *path);

int ledger_balance(const Ledger *ledger, const char *id);
int ledger_next_nonce(const Ledger *ledger, const char *id);
bool ledger_has_account(const Ledger *ledger, const char *id);
const Transaction *ledger_find_transaction(const Ledger *ledger, const char *txid);

void ledger_print_state(const Ledger *ledger, const Registry *registry);
void ledger_print_history(const Ledger *ledger, const char *id);
void ledger_replay_last_transfer(const Ledger *ledger);
void ledger_free(Ledger *ledger);

#endif
