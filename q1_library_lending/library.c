#include "library.h"
#include "input.h"

#include <errno.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PASSPHRASE_SIZE 128
#define MIN_PASSPHRASE_LENGTH 8
#define MAX_LOGIN_ATTEMPTS 3

static bool choose_new_passphrase(char *passphrase, size_t size)
{
    char confirmation[PASSPHRASE_SIZE];

    for (;;) {
        prompt_passphrase("New librarian passphrase: ", passphrase, size);
        if (strlen(passphrase) < MIN_PASSPHRASE_LENGTH) {
            printf("The passphrase must be at least %d characters.\n", MIN_PASSPHRASE_LENGTH);
            if (feof(stdin)) {
                return false;
            }
            continue;
        }
        prompt_passphrase("Confirm passphrase: ", confirmation, sizeof(confirmation));
        bool matches = strcmp(passphrase, confirmation) == 0;
        OPENSSL_cleanse(confirmation, sizeof(confirmation));
        if (matches) {
            return true;
        }
        printf("The passphrases do not match, try again.\n");
        if (feof(stdin)) {
            return false;
        }
    }
}

/*
 * First run: create the librarian's key pair. The new private key stays
 * loaded, so the librarian is logged in and can sign the genesis block.
 */
static bool create_librarian_keys(LibrarySystem *library)
{
    printf("No librarian keys found. Creating a new %s key pair.\n", SIGNATURE_CURVE);

    if (mkdir(KEYS_DIR, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: cannot create %s/ (%s)\n", KEYS_DIR, strerror(errno));
        return false;
    }

    char passphrase[PASSPHRASE_SIZE];
    if (!choose_new_passphrase(passphrase, sizeof(passphrase))) {
        fprintf(stderr, "ERROR: no passphrase was set\n");
        return false;
    }

    EVP_PKEY *key = generate_key_pair();
    bool saved = key != NULL && save_key_pair(key, PRIVATE_KEY_FILE, PUBLIC_KEY_FILE, passphrase);
    OPENSSL_cleanse(passphrase, sizeof(passphrase));
    if (!saved) {
        EVP_PKEY_free(key);
        return false;
    }

    library->private_key = key;
    library->public_key = load_public_key(PUBLIC_KEY_FILE);
    if (library->public_key == NULL) {
        return false;
    }
    printf("Keys saved to %s/. The private key is encrypted with your passphrase.\n", KEYS_DIR);
    return true;
}

static bool setup_keys(LibrarySystem *library)
{
    if (access(PUBLIC_KEY_FILE, F_OK) != 0) {
        return create_librarian_keys(library);
    }
    library->public_key = load_public_key(PUBLIC_KEY_FILE);
    return library->public_key != NULL;
}

/*
 * Creating lending records needs the private key, and the only way to get it
 * is to decrypt it with the librarian's passphrase. Reading, validating and
 * mining only need the public key.
 */
bool require_login(LibrarySystem *library)
{
    if (library->private_key != NULL) {
        return true;
    }
    printf("This action creates a signed record and requires librarian login.\n");

    for (int attempt = 1; attempt <= MAX_LOGIN_ATTEMPTS; attempt++) {
        char passphrase[PASSPHRASE_SIZE];
        prompt_passphrase("Librarian passphrase: ", passphrase, sizeof(passphrase));
        EVP_PKEY *key = load_private_key(PRIVATE_KEY_FILE, passphrase);
        OPENSSL_cleanse(passphrase, sizeof(passphrase));

        if (key != NULL && EVP_PKEY_eq(key, library->public_key) == 1) {
            library->private_key = key;
            printf("Login successful.\n");
            return true;
        }
        if (key != NULL) {
            EVP_PKEY_free(key);
            printf("ERROR: %s does not match %s\n", PRIVATE_KEY_FILE, PUBLIC_KEY_FILE);
            return false;
        }
        printf("Wrong passphrase (attempt %d of %d).\n", attempt, MAX_LOGIN_ATTEMPTS);
        if (feof(stdin)) {
            break;
        }
    }
    printf("ERROR: login failed, action cancelled.\n");
    return false;
}

void logout(LibrarySystem *library)
{
    if (library->private_key == NULL) {
        printf("You are not logged in.\n");
        return;
    }
    EVP_PKEY_free(library->private_key);
    library->private_key = NULL;
    printf("Logged out. The private key has been removed from memory.\n");
}

/* The genesis block is signed and mined like any other block. */
static bool create_genesis_if_missing(LibrarySystem *library)
{
    if (library->chain.count > 0) {
        return true;
    }
    printf("No chain found in %s, creating the genesis block.\n", CHAIN_FILE);
    if (!require_login(library)) {
        return false;
    }

    Block genesis;
    unsigned long attempts = 0;
    create_genesis_record(&genesis);
    if (!sign_block(&genesis, library->private_key)) {
        return false;
    }
    prepare_for_mining(&genesis, &library->chain, library->difficulty);
    search_nonce(&genesis, ULONG_MAX, &attempts);
    printf("Genesis block mined at difficulty %d after %lu attempts.\n", library->difficulty, attempts);

    return append_block_to_file(CHAIN_FILE, &genesis) && block_list_append(&library->chain, &genesis);
}

bool library_open(LibrarySystem *library, LedgerModel model, int difficulty)
{
    library->difficulty = difficulty;

    if (!load_registry(&library->registry, BOOKS_FILE, MEMBERS_FILE)) {
        return false;
    }
    printf("Loaded %d books from %s and %d members from %s.\n",
           library->registry.book_count, BOOKS_FILE, library->registry.member_count, MEMBERS_FILE);

    if (!ledger_init(&library->ledger, model, &library->registry)
        || !setup_keys(library)
        || !load_blocks(CHAIN_FILE, &library->chain)
        || !create_genesis_if_missing(library)
        || !load_blocks(PENDING_FILE, &library->pending)
        || !ledger_load(&library->ledger, TRANSACTIONS_FILE)) {
        return false;
    }

    printf("Chain blocks: %d. Pending records: %d. Confirmed transactions: %d. Model: %s.\n",
           library->chain.count, library->pending.count, library->ledger.confirmed.count, model_name(model));
    if (!library_is_consistent(library, false)) {
        printf("WARNING: the chain or the ledger failed validation. Use the validate option for details. "
               "New records and mining are blocked.\n");
    }
    return true;
}

void library_close(LibrarySystem *library)
{
    block_list_free(&library->chain);
    block_list_free(&library->pending);
    ledger_free(&library->ledger);
    EVP_PKEY_free(library->private_key);
    EVP_PKEY_free(library->public_key);
}

/*
 * Every confirmed reward block must have its reward transaction in the
 * ledger, with the same recipient and amount. This ties transactions.txt to
 * the chain: deleting or editing a reward there is detected here.
 */
static bool rewards_match_ledger(const LibrarySystem *library, bool print_report)
{
    int checked = 0;
    bool all_match = true;

    for (int i = 0; i < library->chain.count; i++) {
        const Block *block = &library->chain.blocks[i];
        if (block->token_reward == 0) {
            continue;
        }
        checked++;
        const Transaction *tx = ledger_find_transaction(&library->ledger, block->transaction_id);
        bool matches = tx != NULL
                       && strcmp(tx->type, TX_REWARD) == 0
                       && strcmp(tx->recipient, block->member_id) == 0
                       && tx->amount + tx->fee == block->token_reward;
        if (!matches) {
            all_match = false;
            if (print_report) {
                printf("Ledger: block %d reward transaction %.16s... is missing or different in %s\n",
                       i, block->transaction_id, TRANSACTIONS_FILE);
            }
        }
    }

    if (print_report && all_match) {
        printf("Ledger: all %d reward transactions on the chain are confirmed in %s\n", checked, TRANSACTIONS_FILE);
    }
    return all_match;
}

bool library_is_consistent(const LibrarySystem *library, bool print_report)
{
    bool chain_ok = validate_chain(&library->chain, library->public_key, print_report);
    bool ledger_ok = rewards_match_ledger(library, print_report);
    return chain_ok && ledger_ok;
}

/*
 * A book's state is derived from its newest record. Pending records are
 * newer than anything on the chain, so the pool is searched first. The book
 * is on loan when that record is BORROWED or OVERDUE.
 */
const Block *find_open_loan(const LibrarySystem *library, const char *book_id, bool *is_pending)
{
    const Block *latest = find_latest_record(&library->pending, book_id);
    *is_pending = latest != NULL;
    if (latest == NULL) {
        latest = find_latest_record(&library->chain, book_id);
    }

    if (latest != NULL
        && (strcmp(latest->action, ACTION_BORROWED) == 0 || strcmp(latest->action, ACTION_OVERDUE) == 0)) {
        return latest;
    }
    return NULL;
}

/* Signs the record and queues it for mining. It is not on the chain yet. */
bool add_to_pending_pool(LibrarySystem *library, Block *record)
{
    if (!library_is_consistent(library, false)) {
        printf("ERROR: the chain failed validation, new records are blocked until it is restored.\n");
        return false;
    }
    if (!sign_block(record, library->private_key)
        || !append_block_to_file(PENDING_FILE, record)
        || !block_list_append(&library->pending, record)) {
        printf("ERROR: the record was not saved.\n");
        return false;
    }
    return true;
}

/*
 * Before mining, a miner checks the first pending record: its signature
 * must verify and its reward must match its transaction. Invalid records
 * are dropped from the pool instead of being mined into the chain.
 */
bool next_pending_is_valid(LibrarySystem *library)
{
    const Block *record = &library->pending.blocks[0];
    if (block_signature_is_valid(record, library->public_key) && reward_matches_transaction(record)) {
        return true;
    }

    printf("Dropping pending %s record for %s: invalid signature or reward.\n", record->action, record->book_id);
    block_list_remove_first(&library->pending);
    save_blocks(PENDING_FILE, &library->pending);
    return false;
}

/*
 * Moves a mined block from the pending pool onto the chain, then confirms
 * its reward transaction. Only at this point does the member's balance
 * change.
 */
bool confirm_first_pending(LibrarySystem *library, const Block *mined)
{
    if (!append_block_to_file(CHAIN_FILE, mined) || !block_list_append(&library->chain, mined)) {
        return false;
    }
    block_list_remove_first(&library->pending);
    if (!save_blocks(PENDING_FILE, &library->pending)) {
        return false;
    }

    printf("Block %d confirmed: %s %s by %s, reward %d\n",
           mined->index, mined->action, mined->book_id, mined->member_id, mined->token_reward);

    if (mined->token_reward > 0) {
        Transaction reward;
        make_reward_transaction(&reward, mined->member_id, mined->token_reward, mined->book_id, mined->timestamp);
        if (!ledger_submit(&library->ledger, &reward, TRANSACTIONS_FILE)) {
            return false;
        }
        printf("Reward transaction %.16s... confirmed: %s receives %d (%d - %d fee)\n",
               reward.txid, reward.recipient, reward.amount, mined->token_reward, reward.fee);
    }

    ledger_print_state(&library->ledger, &library->registry);
    printf("\n");
    return true;
}

bool pay_mining_reward(LibrarySystem *library, const char *miner_id, int amount, const char *reference)
{
    if (amount <= 0) {
        return true;
    }
    Transaction payout;
    make_mining_transaction(&payout, miner_id, amount, reference, time(NULL));
    return ledger_submit(&library->ledger, &payout, TRANSACTIONS_FILE);
}
