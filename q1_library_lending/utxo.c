#include "utxo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

static int find_utxo(const UtxoSet *set, const char *txid, int output_index)
{
    for (int i = 0; i < set->count; i++) {
        if (set->items[i].output_index == output_index && strcmp(set->items[i].txid, txid) == 0) {
            return i;
        }
    }
    return -1;
}

static void add_output(UtxoTransaction *utxo_tx, const char *owner, int amount)
{
    TxOutput *output = &utxo_tx->outputs[utxo_tx->output_count++];
    snprintf(output->owner, sizeof(output->owner), "%s", owner);
    output->amount = amount;
}

/*
 * Turns a model-independent transaction into inputs and outputs. For a
 * transfer, the sender's oldest outputs are spent first until they cover
 * amount + fee; anything above that comes back to the sender as change.
 */
bool utxo_build_transaction(const UtxoSet *set, const Transaction *tx, UtxoTransaction *utxo_tx)
{
    memset(utxo_tx, 0, sizeof(*utxo_tx));
    strcpy(utxo_tx->txid, tx->txid);
    utxo_tx->fee = tx->fee;

    if (is_minting_transaction(tx)) {
        add_output(utxo_tx, tx->recipient, tx->amount);
        return true;
    }

    strcpy(utxo_tx->sender, tx->sender);
    int needed = tx->amount + tx->fee;
    int collected = 0;

    for (int i = 0; i < set->count && collected < needed; i++) {
        const Utxo *utxo = &set->items[i];
        if (strcmp(utxo->owner, tx->sender) != 0) {
            continue;
        }
        if (utxo_tx->input_count == MAX_TX_INPUTS) {
            printf("REJECTED: %s would need more than %d inputs\n", tx->sender, MAX_TX_INPUTS);
            return false;
        }
        TxInput *input = &utxo_tx->inputs[utxo_tx->input_count++];
        strcpy(input->txid, utxo->txid);
        input->output_index = utxo->output_index;
        collected += utxo->amount;
    }

    if (collected < needed) {
        printf("REJECTED: insufficient funds, %s has %d coins in unspent outputs but needs %d (%d + %d fee)\n",
               tx->sender, utxo_balance(set, tx->sender), needed, tx->amount, tx->fee);
        return false;
    }

    add_output(utxo_tx, tx->recipient, tx->amount);
    if (collected > needed) {
        add_output(utxo_tx, tx->sender, collected - needed);
    }
    return true;
}

/*
 * Every input must be an unspent output owned by the sender and used only
 * once, and the inputs must cover the outputs plus the fee. An input that
 * was already spent is no longer in the set, which is what stops a double
 * spend.
 */
bool utxo_validate(const UtxoSet *set, const UtxoTransaction *utxo_tx)
{
    int output_total = 0;
    for (int i = 0; i < utxo_tx->output_count; i++) {
        if (utxo_tx->outputs[i].amount <= 0) {
            printf("REJECTED: output %d has a non-positive amount\n", i);
            return false;
        }
        output_total += utxo_tx->outputs[i].amount;
    }

    if (utxo_tx->input_count == 0) {
        return true;
    }

    int input_total = 0;
    for (int i = 0; i < utxo_tx->input_count; i++) {
        const TxInput *input = &utxo_tx->inputs[i];
        int position = find_utxo(set, input->txid, input->output_index);
        if (position < 0) {
            printf("REJECTED: input %.12s...:%d is not in the UTXO set (already spent or never existed)\n",
                   input->txid, input->output_index);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (utxo_tx->inputs[j].output_index == input->output_index
                && strcmp(utxo_tx->inputs[j].txid, input->txid) == 0) {
                printf("REJECTED: input %.12s...:%d is used twice in the same transaction\n",
                       input->txid, input->output_index);
                return false;
            }
        }
        if (strcmp(set->items[position].owner, utxo_tx->sender) != 0) {
            printf("REJECTED: input %.12s...:%d belongs to %s, not %s\n",
                   input->txid, input->output_index, set->items[position].owner, utxo_tx->sender);
            return false;
        }
        input_total += set->items[position].amount;
    }

    if (input_total < output_total + utxo_tx->fee) {
        printf("REJECTED: inputs total %d, outputs plus fee total %d\n", input_total, output_total + utxo_tx->fee);
        return false;
    }
    return true;
}

static bool append_utxo(UtxoSet *set, const Utxo *utxo)
{
    if (set->count == set->capacity) {
        int new_capacity = set->capacity == 0 ? INITIAL_CAPACITY : set->capacity * 2;
        Utxo *grown = realloc(set->items, (size_t)new_capacity * sizeof(Utxo));
        if (grown == NULL) {
            fprintf(stderr, "ERROR: out of memory\n");
            return false;
        }
        set->items = grown;
        set->capacity = new_capacity;
    }
    set->items[set->count++] = *utxo;
    return true;
}

/* Removes the spent inputs and adds the new outputs, keeping the set in creation order. */
bool utxo_apply(UtxoSet *set, const UtxoTransaction *utxo_tx)
{
    for (int i = 0; i < utxo_tx->input_count; i++) {
        int position = find_utxo(set, utxo_tx->inputs[i].txid, utxo_tx->inputs[i].output_index);
        if (position < 0) {
            return false;
        }
        memmove(&set->items[position], &set->items[position + 1],
                (size_t)(set->count - position - 1) * sizeof(Utxo));
        set->count--;
    }

    for (int i = 0; i < utxo_tx->output_count; i++) {
        Utxo utxo;
        strcpy(utxo.txid, utxo_tx->txid);
        utxo.output_index = i;
        strcpy(utxo.owner, utxo_tx->outputs[i].owner);
        utxo.amount = utxo_tx->outputs[i].amount;
        if (!append_utxo(set, &utxo)) {
            return false;
        }
    }
    return true;
}

int utxo_balance(const UtxoSet *set, const char *owner)
{
    int balance = 0;
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].owner, owner) == 0) {
            balance += set->items[i].amount;
        }
    }
    return balance;
}

void utxo_print_set(const UtxoSet *set)
{
    printf("UTXO set, unspent outputs: %d\n", set->count);
    if (set->count == 0) {
        printf("  (empty)\n");
        return;
    }
    printf("  %-20s %-6s %-16s %s\n", "txid", "output", "owner", "amount");
    for (int i = 0; i < set->count; i++) {
        const Utxo *utxo = &set->items[i];
        printf("  %.16s...  %-6d %-16s %d\n", utxo->txid, utxo->output_index, utxo->owner, utxo->amount);
    }
}

void utxo_set_free(UtxoSet *set)
{
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}
