#ifndef ACCOUNT_H
#define ACCOUNT_H

#include "transaction.h"

#include <stdbool.h>

typedef struct HistoryNode {
    Transaction transaction;
    struct HistoryNode *next;
} HistoryNode;

/* nonce is the value the account's next outgoing transaction must carry. */
typedef struct {
    char id[ACCOUNT_ID_SIZE];
    int balance;
    int nonce;
    HistoryNode *history_head;
    HistoryNode *history_tail;
} Account;

typedef struct {
    Account *items;
    int count;
    int capacity;
} AccountTable;

int account_index(const AccountTable *table, const char *id);
bool account_create(AccountTable *table, const char *id);
bool account_validate(const AccountTable *table, const Transaction *tx);
bool account_apply(AccountTable *table, const Transaction *tx);
void account_print_table(const AccountTable *table);
void account_print_history(const AccountTable *table, const char *id);
void account_table_free(AccountTable *table);

#endif
