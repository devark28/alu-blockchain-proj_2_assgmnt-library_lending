#include "account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

int account_index(const AccountTable *table, const char *id)
{
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

bool account_create(AccountTable *table, const char *id)
{
    if (account_index(table, id) >= 0) {
        return true;
    }
    if (table->count == table->capacity) {
        int new_capacity = table->capacity == 0 ? INITIAL_CAPACITY : table->capacity * 2;
        Account *grown = realloc(table->items, (size_t)new_capacity * sizeof(Account));
        if (grown == NULL) {
            fprintf(stderr, "ERROR: out of memory\n");
            return false;
        }
        table->items = grown;
        table->capacity = new_capacity;
    }

    Account *account = &table->items[table->count++];
    memset(account, 0, sizeof(*account));
    snprintf(account->id, sizeof(account->id), "%s", id);
    return true;
}

/*
 * Minted transactions create coins, so only their amount is checked. A
 * transfer must come from an existing account, carry exactly the nonce the
 * account expects next, and be covered by the balance.
 */
bool account_validate(const AccountTable *table, const Transaction *tx)
{
    if (tx->amount <= 0) {
        printf("REJECTED: amount must be positive\n");
        return false;
    }
    if (is_minting_transaction(tx)) {
        return true;
    }

    int sender_index = account_index(table, tx->sender);
    if (sender_index < 0) {
        printf("REJECTED: sender %s has no account\n", tx->sender);
        return false;
    }
    const Account *sender = &table->items[sender_index];

    if (tx->nonce < sender->nonce) {
        printf("REJECTED: nonce %d was already used by %s, the next nonce is %d\n", tx->nonce, sender->id, sender->nonce);
        return false;
    }
    if (tx->nonce > sender->nonce) {
        printf("REJECTED: incorrect nonce %d for %s, the next nonce is %d\n", tx->nonce, sender->id, sender->nonce);
        return false;
    }
    if (sender->balance < tx->amount + tx->fee) {
        printf("REJECTED: insufficient balance, %s has %d coins but needs %d (%d + %d fee)\n",
               sender->id, sender->balance, tx->amount + tx->fee, tx->amount, tx->fee);
        return false;
    }
    return true;
}

static bool add_history(Account *account, const Transaction *tx)
{
    HistoryNode *node = malloc(sizeof(HistoryNode));
    if (node == NULL) {
        fprintf(stderr, "ERROR: out of memory\n");
        return false;
    }
    node->transaction = *tx;
    node->next = NULL;

    if (account->history_tail == NULL) {
        account->history_head = node;
    } else {
        account->history_tail->next = node;
    }
    account->history_tail = node;
    return true;
}

/* Debits the sender (amount + fee, nonce + 1) and credits the recipient. Call account_validate first. */
bool account_apply(AccountTable *table, const Transaction *tx)
{
    if (!account_create(table, tx->recipient)) {
        return false;
    }

    if (!is_minting_transaction(tx)) {
        Account *sender = &table->items[account_index(table, tx->sender)];
        sender->balance -= tx->amount + tx->fee;
        sender->nonce++;
        if (!add_history(sender, tx)) {
            return false;
        }
    }

    Account *recipient = &table->items[account_index(table, tx->recipient)];
    recipient->balance += tx->amount;
    return add_history(recipient, tx);
}

void account_print_table(const AccountTable *table)
{
    printf("Accounts (%d)\n", table->count);
    printf("  %-16s %8s %6s\n", "account", "balance", "nonce");
    for (int i = 0; i < table->count; i++) {
        const Account *account = &table->items[i];
        printf("  %-16s %8d %6d\n", account->id, account->balance, account->nonce);
    }
}

void account_print_history(const AccountTable *table, const char *id)
{
    int index = account_index(table, id);
    if (index < 0) {
        printf("ERROR: no account %s\n", id);
        return;
    }
    const Account *account = &table->items[index];

    printf("Transaction history for %s (balance %d, next nonce %d)\n", account->id, account->balance, account->nonce);
    if (account->history_head == NULL) {
        printf("  (no transactions)\n");
        return;
    }
    printf("  %-9s %-10s %-10s %6s %4s %5s  %s\n", "type", "sender", "recipient", "amount", "fee", "nonce", "txid");
    for (const HistoryNode *node = account->history_head; node != NULL; node = node->next) {
        const Transaction *tx = &node->transaction;
        char nonce_text[16];
        format_nonce(tx->nonce, nonce_text, sizeof(nonce_text));
        printf("  %-9s %-10s %-10s %6d %4d %5s  %.16s...\n",
               tx->type, tx->sender, tx->recipient, tx->amount, tx->fee, nonce_text, tx->txid);
    }
}

void account_table_free(AccountTable *table)
{
    for (int i = 0; i < table->count; i++) {
        HistoryNode *node = table->items[i].history_head;
        while (node != NULL) {
            HistoryNode *next = node->next;
            free(node);
            node = next;
        }
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}
