#include "blockchain.h"
#include "input.h"
#include "ledger.h"
#include "lending.h"
#include "library.h"
#include "mining.h"
#include "transaction.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ID_INPUT_SIZE 64
#define MAX_TRANSFER_AMOUNT 1000000
#define TAMPERED_REWARD 100

static void print_usage(const char *program)
{
    printf("Usage: %s [--model utxo|account] [--difficulty %d-%d]\n", program, MIN_DIFFICULTY, MAX_DIFFICULTY);
    printf("Without --model the program asks which model to use.\n");
}

/* Returns false on an unknown or malformed argument. */
static bool parse_arguments(int argc, char *argv[], bool *model_given, LedgerModel *model, int *difficulty)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "utxo") == 0) {
                *model = MODEL_UTXO;
            } else if (strcmp(argv[i], "account") == 0) {
                *model = MODEL_ACCOUNT;
            } else {
                return false;
            }
            *model_given = true;
        } else if (strcmp(argv[i], "--difficulty") == 0 && i + 1 < argc) {
            i++;
            char *end;
            long value = strtol(argv[i], &end, 10);
            if (*end != '\0' || value < MIN_DIFFICULTY || value > MAX_DIFFICULTY) {
                return false;
            }
            *difficulty = (int)value;
        } else {
            return false;
        }
    }
    return true;
}

static bool choose_model(LedgerModel *model)
{
    printf("Choose the transaction model for this session:\n");
    printf("  1. UTXO (unspent transaction outputs)\n");
    printf("  2. Account-based (balances and nonces)\n");

    int choice;
    while (!prompt_number("Model: ", 1, 2, &choice)) {
        if (feof(stdin)) {
            return false;
        }
    }
    *model = choice == 1 ? MODEL_UTXO : MODEL_ACCOUNT;
    return true;
}

static void view_registries(const LibrarySystem *library)
{
    const Registry *registry = &library->registry;

    printf("\nBooks (%d)\n", registry->book_count);
    for (int i = 0; i < registry->book_count; i++) {
        const Book *book = &registry->books[i];
        bool is_pending;
        const Block *open_loan = find_open_loan(library, book->book_id, &is_pending);
        printf("  %-8s %-28s %-26s ", book->book_id, book->title, book->author);
        if (open_loan == NULL) {
            printf("available\n");
        } else {
            printf("%s by %s%s\n", open_loan->action, open_loan->member_id, is_pending ? " (pending)" : "");
        }
    }

    printf("\nMembers (%d)\n", registry->member_count);
    for (int i = 0; i < registry->member_count; i++) {
        const Member *member = &registry->members[i];
        printf("  %-8s %-28s %s\n", member->member_id, member->full_name, member->course_code);
    }
}

static void view_pending_pool(const LibrarySystem *library)
{
    printf("\nPending pool, unconfirmed records: %d (signed, not yet on the chain)\n", library->pending.count);
    for (int i = 0; i < library->pending.count; i++) {
        print_pending_record(&library->pending.blocks[i], library->public_key);
    }
    if (library->pending.count > 0) {
        printf("Mine them with option 6, 7 or 8 to confirm them and credit any rewards.\n");
    }
}

static void view_records(const LibrarySystem *library)
{
    printf("\n");
    for (int i = 0; i < library->chain.count; i++) {
        print_block(&library->chain.blocks[i], library->public_key);
    }
    printf("%d confirmed blocks in the chain.\n", library->chain.count);
}

static void view_balances(const LibrarySystem *library)
{
    printf("\n%s model\n", model_name(library->ledger.model));
    ledger_print_state(&library->ledger, &library->registry);
}

static bool is_known_account(const LibrarySystem *library, const char *id)
{
    return find_member(&library->registry, id) != NULL || ledger_has_account(&library->ledger, id);
}

/*
 * A manual transfer between two accounts, confirmed immediately. The UTXO
 * model picks the inputs and the nonce itself; the account model asks for
 * the nonce so that a wrong or reused one can be shown being rejected.
 */
static void transfer_tokens(LibrarySystem *library)
{
    if (!require_login(library)) {
        return;
    }

    char sender[ID_INPUT_SIZE];
    char recipient[ID_INPUT_SIZE];
    prompt_text("Sender ID: ", sender, sizeof(sender));
    prompt_text("Recipient ID: ", recipient, sizeof(recipient));
    if (!is_known_account(library, sender) || !is_known_account(library, recipient)) {
        printf("ERROR: unknown account, use a member ID or a miner that has received coins\n");
        return;
    }
    if (strcmp(sender, recipient) == 0) {
        printf("ERROR: sender and recipient must be different\n");
        return;
    }

    int amount;
    if (!prompt_number("Amount: ", 1, MAX_TRANSFER_AMOUNT, &amount)) {
        return;
    }

    int nonce;
    if (library->ledger.model == MODEL_ACCOUNT) {
        if (!prompt_number("Nonce: ", 0, INT_MAX, &nonce)) {
            return;
        }
    } else {
        nonce = ledger_next_nonce(&library->ledger, sender);
    }

    Transaction tx;
    make_transaction(&tx, TX_TRANSFER, sender, recipient, amount, TRANSACTION_FEE, nonce, "", time(NULL));
    if (!ledger_submit(&library->ledger, &tx, TRANSACTIONS_FILE)) {
        return;
    }

    printf("Transfer %.16s... confirmed: %s -> %s, amount %d, fee %d, nonce %d\n",
           tx.txid, sender, recipient, amount, tx.fee, nonce);
    if (library->ledger.model == MODEL_UTXO) {
        const UtxoTransaction *spent = &library->ledger.last_utxo_transfer;
        printf("Inputs spent: %d, outputs created: %d%s\n", spent->input_count, spent->output_count,
               spent->output_count > 1 ? " (including change back to the sender)" : "");
    }
    view_balances(library);
}

static void show_history(const LibrarySystem *library)
{
    char id[ID_INPUT_SIZE];
    prompt_text("Member or miner ID: ", id, sizeof(id));
    printf("\n");
    ledger_print_history(&library->ledger, id);
}

static void set_difficulty(LibrarySystem *library)
{
    int difficulty;
    printf("Current difficulty: %d. Each extra zero makes a valid hash about 16 times harder to find.\n",
           library->difficulty);
    if (prompt_number("New difficulty (1-4): ", MIN_DIFFICULTY, MAX_DIFFICULTY, &difficulty)) {
        library->difficulty = difficulty;
        printf("Difficulty set to %d: about %d hash attempts per block on average.\n",
               difficulty, 1 << (4 * difficulty));
    }
}

static void solo_mining(LibrarySystem *library)
{
    char miner_id[ID_INPUT_SIZE];
    prompt_text("Miner ID (e.g. MINER1): ", miner_id, sizeof(miner_id));
    if (!is_valid_account_id(miner_id)) {
        printf("ERROR: a miner ID is 1 to %d letters, digits, '-' or '_'\n", ACCOUNT_ID_SIZE - 1);
        return;
    }
    mine_solo(library, miner_id);
}

static void cloud_mining(LibrarySystem *library)
{
    char renter_id[ID_INPUT_SIZE];
    prompt_text("Member renting the mining power: ", renter_id, sizeof(renter_id));
    if (find_member(&library->registry, renter_id) == NULL) {
        printf("ERROR: Book or Member not found\n");
        return;
    }
    int rounds;
    if (prompt_number("Rental duration in rounds (1-5): ", MIN_RENTAL_ROUNDS, MAX_RENTAL_ROUNDS, &rounds)) {
        mine_cloud(library, renter_id, rounds);
    }
}

static void tamper_with_block(Block *block)
{
    if (block->token_reward > 0) {
        printf("change block %d reward from %d to %d coins", block->index, block->token_reward, TAMPERED_REWARD);
        block->token_reward = TAMPERED_REWARD;
    } else {
        const char *forged_action = strcmp(block->action, ACTION_BORROWED) == 0 ? ACTION_RETURNED : ACTION_BORROWED;
        printf("change block %d action from %s to %s", block->index, block->action, forged_action);
        strcpy(block->action, forged_action);
    }
}

/*
 * Works on a copy of the chain, so chain.txt is never touched. Step 1 edits
 * a block. Step 2 does what a determined attacker would: recompute the
 * reward transaction id and redo the proof of work for the edited block and
 * every block after it. Only the signature check still catches step 2.
 */
static void run_tamper_demo(const LibrarySystem *library)
{
    const BlockList *chain = &library->chain;
    if (chain->count < 2) {
        printf("Mine at least one lending record first, the genesis block has no lending data to tamper with.\n");
        return;
    }

    int target;
    printf("This demo edits a copy of the chain in memory, %s is not modified.\n", CHAIN_FILE);
    if (!prompt_number("Block to tamper with: ", 1, chain->count - 1, &target)) {
        return;
    }

    BlockList copy = {0};
    for (int i = 0; i < chain->count; i++) {
        if (!block_list_append(&copy, &chain->blocks[i])) {
            block_list_free(&copy);
            return;
        }
    }
    Block *forged = &copy.blocks[target];

    printf("\nStep 1: ");
    tamper_with_block(forged);
    printf(", leaving its hash, proof of work and signature as they were.\n\n");
    validate_chain(&copy, library->public_key, true);

    printf("\nStep 2: recompute the transaction id, then redo the proof of work for block %d and every block after it.\n",
           target);
    if (forged->token_reward > 0) {
        Transaction forged_reward;
        make_reward_transaction(&forged_reward, forged->member_id, forged->token_reward, forged->book_id, forged->timestamp);
        strcpy(forged->transaction_id, forged_reward.txid);
    }
    unsigned long attempts = 0;
    for (int i = target; i < copy.count; i++) {
        strcpy(copy.blocks[i].previous_hash, copy.blocks[i - 1].hash);
        copy.blocks[i].nonce = 0;
        search_nonce(&copy.blocks[i], ULONG_MAX, &attempts);
    }
    printf("Re-mined %d blocks in %lu hash attempts.\n\n", copy.count - target, attempts);
    validate_chain(&copy, library->public_key, true);

    printf("\nHashes, links, proof of work and the reward transaction id are all consistent again, but block %d\n"
           "still fails its signature check. Forging it would need the librarian's private key.\n", target);
    printf("Re-mining cost %lu attempts here; at a higher difficulty or with more blocks after the edit it grows quickly.\n",
           attempts);
    block_list_free(&copy);
}

static void print_menu(const LibrarySystem *library)
{
    printf("\n===== Library Lending Tracker =====\n");
    printf("Model %s | Difficulty %d | Chain %d | Pending %d | Librarian %s\n",
           model_name(library->ledger.model), library->difficulty, library->chain.count,
           library->pending.count, library->private_key != NULL ? "logged in" : "logged out");
    printf("  1. View registries          10. Validate chain and ledger\n");
    printf("  2. Borrow a book            11. View balances / UTXO set\n");
    printf("  3. Return a book            12. Transfer tokens\n");
    printf("  4. Mark a book overdue      13. Transaction history\n");
    printf("  5. View pending pool        14. Double-spend test (replay last transfer)\n");
    printf("  6. Mine: solo               15. Set mining difficulty\n");
    printf("  7. Mine: pool               16. Tamper detection demo\n");
    printf("  8. Mine: cloud rental       17. Log out\n");
    printf("  9. View confirmed records    0. Exit\n");
}

static void run_menu(LibrarySystem *library)
{
    for (;;) {
        print_menu(library);
        int choice;
        if (!prompt_number("Choice: ", 0, 17, &choice)) {
            if (feof(stdin)) {
                printf("\n");
                return;
            }
            continue;
        }

        switch (choice) {
        case 1: view_registries(library); break;
        case 2: borrow_book(library); break;
        case 3: return_book(library); break;
        case 4: mark_overdue(library); break;
        case 5: view_pending_pool(library); break;
        case 6: solo_mining(library); break;
        case 7: mine_pool(library); break;
        case 8: cloud_mining(library); break;
        case 9: view_records(library); break;
        case 10: library_is_consistent(library, true); break;
        case 11: view_balances(library); break;
        case 12: transfer_tokens(library); break;
        case 13: show_history(library); break;
        case 14: ledger_replay_last_transfer(&library->ledger); break;
        case 15: set_difficulty(library); break;
        case 16: run_tamper_demo(library); break;
        case 17: logout(library); break;
        case 0: return;
        }
    }
}

int main(int argc, char *argv[])
{
    bool model_given = false;
    LedgerModel model = MODEL_UTXO;
    int difficulty = DEFAULT_DIFFICULTY;

    if (!parse_arguments(argc, argv, &model_given, &model, &difficulty)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (!model_given && !choose_model(&model)) {
        return EXIT_FAILURE;
    }
    srand((unsigned int)time(NULL));

    LibrarySystem library = {0};
    bool opened = library_open(&library, model, difficulty);
    if (opened) {
        run_menu(&library);
    }
    library_close(&library);
    return opened ? EXIT_SUCCESS : EXIT_FAILURE;
}
