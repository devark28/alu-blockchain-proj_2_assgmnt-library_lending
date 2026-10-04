#include "ledger.h"

#include <stdio.h>
#include <string.h>

const char *model_name(LedgerModel model)
{
    return model == MODEL_UTXO ? "UTXO" : "account";
}

/* Every member starts with an account at balance 0 in the account model. */
bool ledger_init(Ledger *ledger, LedgerModel model, const Registry *registry)
{
    memset(ledger, 0, sizeof(*ledger));
    ledger->model = model;

    if (model == MODEL_ACCOUNT) {
        for (int i = 0; i < registry->member_count; i++) {
            if (!account_create(&ledger->accounts, registry->members[i].member_id)) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Validates tx against the current state, writes it to the transaction log
 * (when path is given), then applies it. Writing before applying means a
 * failed write never leaves a balance change that was not saved.
 */
bool ledger_submit(Ledger *ledger, const Transaction *tx, const char *path)
{
    UtxoTransaction utxo_tx;
    if (ledger->model == MODEL_UTXO) {
        if (!utxo_build_transaction(&ledger->utxos, tx, &utxo_tx) || !utxo_validate(&ledger->utxos, &utxo_tx)) {
            return false;
        }
    } else if (!account_validate(&ledger->accounts, tx)) {
        return false;
    }
    if (ledger_find_transaction(ledger, tx->txid) != NULL) {
        printf("REJECTED: transaction %.16s... is already confirmed\n", tx->txid);
        return false;
    }

    if (path != NULL && !append_transaction_to_file(path, tx)) {
        return false;
    }

    bool applied;
    if (ledger->model == MODEL_UTXO) {
        applied = utxo_apply(&ledger->utxos, &utxo_tx);
    } else {
        applied = account_apply(&ledger->accounts, tx);
    }
    if (!applied || !transaction_list_append(&ledger->confirmed, tx)) {
        return false;
    }

    if (strcmp(tx->type, TX_TRANSFER) == 0) {
        ledger->has_last_transfer = true;
        ledger->last_transfer = *tx;
        if (ledger->model == MODEL_UTXO) {
            ledger->last_utxo_transfer = utxo_tx;
        }
    }
    return true;
}

/*
 * Rebuilds the chosen model by replaying every confirmed transaction in
 * order. The log is model independent, so the same history can be loaded
 * as UTXOs in one session and as accounts in the next.
 */
bool ledger_load(Ledger *ledger, const char *path)
{
    TransactionList history = {0};
    bool loaded = load_transactions(path, &history);

    for (int i = 0; loaded && i < history.count; i++) {
        if (!ledger_submit(ledger, &history.items[i], NULL)) {
            fprintf(stderr, "ERROR: transaction %d in %s could not be replayed\n", i + 1, path);
            loaded = false;
        }
    }

    transaction_list_free(&history);
    ledger->has_last_transfer = false;
    return loaded;
}

int ledger_balance(const Ledger *ledger, const char *id)
{
    if (ledger->model == MODEL_UTXO) {
        return utxo_balance(&ledger->utxos, id);
    }
    int index = account_index(&ledger->accounts, id);
    return index < 0 ? 0 : ledger->accounts.items[index].balance;
}

/* The next nonce is the number of transfers the account has already sent. */
int ledger_next_nonce(const Ledger *ledger, const char *id)
{
    int sent = 0;
    for (int i = 0; i < ledger->confirmed.count; i++) {
        const Transaction *tx = &ledger->confirmed.items[i];
        if (strcmp(tx->type, TX_TRANSFER) == 0 && strcmp(tx->sender, id) == 0) {
            sent++;
        }
    }
    return sent;
}

bool ledger_has_account(const Ledger *ledger, const char *id)
{
    if (ledger->model == MODEL_ACCOUNT) {
        return account_index(&ledger->accounts, id) >= 0;
    }
    for (int i = 0; i < ledger->confirmed.count; i++) {
        if (strcmp(ledger->confirmed.items[i].recipient, id) == 0) {
            return true;
        }
    }
    return false;
}

const Transaction *ledger_find_transaction(const Ledger *ledger, const char *txid)
{
    for (int i = 0; i < ledger->confirmed.count; i++) {
        if (strcmp(ledger->confirmed.items[i].txid, txid) == 0) {
            return &ledger->confirmed.items[i];
        }
    }
    return NULL;
}

static bool owner_listed_before(const UtxoSet *set, int position)
{
    for (int i = 0; i < position; i++) {
        if (strcmp(set->items[i].owner, set->items[position].owner) == 0) {
            return true;
        }
    }
    return false;
}

void ledger_print_state(const Ledger *ledger, const Registry *registry)
{
    if (ledger->model == MODEL_ACCOUNT) {
        account_print_table(&ledger->accounts);
        return;
    }

    utxo_print_set(&ledger->utxos);
    printf("Balances (sum of each owner's unspent outputs)\n");
    for (int i = 0; i < registry->member_count; i++) {
        const char *member_id = registry->members[i].member_id;
        printf("  %-16s %8d\n", member_id, utxo_balance(&ledger->utxos, member_id));
    }
    for (int i = 0; i < ledger->utxos.count; i++) {
        const char *owner = ledger->utxos.items[i].owner;
        if (find_member(registry, owner) == NULL && !owner_listed_before(&ledger->utxos, i)) {
            printf("  %-16s %8d\n", owner, utxo_balance(&ledger->utxos, owner));
        }
    }
}

/*
 * The account model keeps a linked list per account. The UTXO model has no
 * accounts, so the same view is filtered from the confirmed transactions.
 */
void ledger_print_history(const Ledger *ledger, const char *id)
{
    if (ledger->model == MODEL_ACCOUNT) {
        account_print_history(&ledger->accounts, id);
        return;
    }

    printf("Transactions involving %s (UTXO model, filtered from confirmed transactions)\n", id);
    printf("  %-9s %-10s %-10s %6s %4s %5s  %s\n", "type", "sender", "recipient", "amount", "fee", "nonce", "txid");
    int shown = 0;
    for (int i = 0; i < ledger->confirmed.count; i++) {
        const Transaction *tx = &ledger->confirmed.items[i];
        if (strcmp(tx->sender, id) != 0 && strcmp(tx->recipient, id) != 0) {
            continue;
        }
        char nonce_text[16];
        format_nonce(tx->nonce, nonce_text, sizeof(nonce_text));
        printf("  %-9s %-10s %-10s %6d %4d %5s  %.16s...\n",
               tx->type, tx->sender, tx->recipient, tx->amount, tx->fee, nonce_text, tx->txid);
        shown++;
    }
    if (shown == 0) {
        printf("  (no transactions)\n");
    }
}

/*
 * Sends the most recent transfer through validation a second time, the way
 * an attacker would rebroadcast it. The UTXO model refuses it because its
 * inputs are already spent; the account model refuses the reused nonce.
 */
void ledger_replay_last_transfer(const Ledger *ledger)
{
    if (!ledger->has_last_transfer) {
        printf("No transfer has been made in this session yet. Make one with the transfer option first.\n");
        return;
    }

    const Transaction *tx = &ledger->last_transfer;
    printf("Re-submitting transfer %.16s... (%s -> %s, amount %d, nonce %d) under the %s model\n",
           tx->txid, tx->sender, tx->recipient, tx->amount, tx->nonce, model_name(ledger->model));

    bool accepted;
    if (ledger->model == MODEL_UTXO) {
        accepted = utxo_validate(&ledger->utxos, &ledger->last_utxo_transfer);
    } else {
        accepted = account_validate(&ledger->accounts, tx);
    }
    if (accepted) {
        printf("WARNING: the replayed transfer passed validation\n");
    } else {
        printf("Double spend prevented, nothing was changed.\n");
    }
}

void ledger_free(Ledger *ledger)
{
    transaction_list_free(&ledger->confirmed);
    utxo_set_free(&ledger->utxos);
    account_table_free(&ledger->accounts);
}
