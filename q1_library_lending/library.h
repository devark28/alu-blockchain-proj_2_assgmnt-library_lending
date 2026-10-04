#ifndef LIBRARY_H
#define LIBRARY_H

#include "blockchain.h"
#include "ledger.h"
#include "registry.h"

#include <stdbool.h>

#define BOOKS_FILE "books.txt"
#define MEMBERS_FILE "members.txt"
#define CHAIN_FILE "chain.txt"
#define PENDING_FILE "pending.txt"
#define TRANSACTIONS_FILE "transactions.txt"
#define KEYS_DIR "keys"
#define PRIVATE_KEY_FILE KEYS_DIR "/librarian_private.pem"
#define PUBLIC_KEY_FILE KEYS_DIR "/librarian_public.pem"

typedef struct {
    Registry registry;
    BlockList chain;
    BlockList pending;
    Ledger ledger;
    EVP_PKEY *public_key;
    EVP_PKEY *private_key;
    int difficulty;
} LibrarySystem;

bool library_open(LibrarySystem *library, LedgerModel model, int difficulty);
void library_close(LibrarySystem *library);

bool require_login(LibrarySystem *library);
void logout(LibrarySystem *library);

bool library_is_consistent(const LibrarySystem *library, bool print_report);
const Block *find_open_loan(const LibrarySystem *library, const char *book_id, bool *is_pending);
bool add_to_pending_pool(LibrarySystem *library, Block *record);

bool next_pending_is_valid(LibrarySystem *library);
bool confirm_first_pending(LibrarySystem *library, const Block *mined);
bool pay_mining_reward(LibrarySystem *library, const char *miner_id, int amount, const char *reference);

#endif
