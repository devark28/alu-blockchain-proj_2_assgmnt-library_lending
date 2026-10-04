# Library Lending Tracker: Token Rewards and Mining

A command-line library lending tracker in C where every borrow, return and
overdue event is a signed block on a proof-of-work blockchain, and
returning a book earns the member tokens.

This project extends the
[Blockchain-Based Library Book Lending Tracker](https://github.com/devark28/alu-blockchain-proj_1_assgmnt-library_lending),
which covers the base chain: book and member registries, SHA-256 linking,
ECDSA signatures with a passphrase-protected key, chain validation and
tamper detection. Everything from that version still works the same way;
this README focuses on what was added.

A detailed walkthrough of the design, both transaction models and the
mining simulation is in
[q1_library_lending/README.md](q1_library_lending/README.md).

## What changed

| Area | Before | Now |
|---|---|---|
| Recording | a block was appended immediately | a signed record waits in a pending pool until it is mined |
| Block | lending data, hashes, signature | adds `token_reward`, `transaction_id`, `difficulty`, `nonce` |
| Rewards | none | on-time return earns 10 coins, late return 5, no return 0, minus a 1 coin fee |
| Balances | none | UTXO model or account model, chosen per session |
| Transfers | none | members can send tokens; double spends and bad nonces are rejected |
| Mining | none | proof of work with difficulty 1 to 4, solo, pool and cloud mining |
| Persistence | `chain.txt` | adds `pending.txt` and `transactions.txt` |

## Requirements

- Linux (tested on Ubuntu 24.04)
- `gcc` (tested with 13.3)
- OpenSSL 3.0 or newer with development headers (`libssl-dev`)

```
sudo apt install build-essential libssl-dev
```

No other libraries are needed. The UTXO set, accounts, linked lists and
mining simulation are all plain C.

## Build

From inside `q1_library_lending/`:

```
gcc -Wall -Wextra -o library_tracker *.c -lcrypto
```

`*.c` compiles every source file in the folder, and `-lcrypto` links
OpenSSL for SHA-256, ECDSA and the PEM key files.

## Run

```
./library_tracker [--model utxo|account] [--difficulty 1-4]
```

Run it from inside `q1_library_lending/`, because the data files are
opened with relative paths. On the first run it creates the librarian's
key pair (you choose a passphrase) and mines the genesis block.

### Switching between transaction models

The model applies to every token transaction for the whole session.

| How | Example |
|---|---|
| command-line argument | `./library_tracker --model utxo` or `./library_tracker --model account` |
| startup prompt | run without `--model` and choose 1 (UTXO) or 2 (account) |

Confirmed transactions are stored in a model-independent log, so the same
history can be opened with either model. Balances come out identical;
only the way they are stored and validated changes.

### Setting the mining difficulty

Difficulty is the number of leading zero hex characters a block hash must
have. The default is 2.

| How | Example |
|---|---|
| command-line argument | `./library_tracker --difficulty 3` |
| menu, during a session | option 15, then a value from 1 to 4 |

Each extra zero makes a valid hash about 16 times harder to find: on
average 16, 256, 4,096 and 65,536 attempts for difficulty 1 to 4.

### Demonstrating late returns and overdue books

The loan period is 14 days. To see late returns and overdue records in a
short demo, shorten it at compile time:

```
gcc -Wall -Wextra -DLOAN_PERIOD_SECONDS=30 -o library_tracker *.c -lcrypto
```

### Menu

```
===== Library Lending Tracker =====
Model UTXO | Difficulty 2 | Chain 4 | Pending 0 | Librarian logged in
  1. View registries          10. Validate chain and ledger
  2. Borrow a book            11. View balances / UTXO set
  3. Return a book            12. Transfer tokens
  4. Mark a book overdue      13. Transaction history
  5. View pending pool        14. Double-spend test (replay last transfer)
  6. Mine: solo               15. Set mining difficulty
  7. Mine: pool               16. Tamper detection demo
  8. Mine: cloud rental       17. Log out
  9. View confirmed records    0. Exit
```

Borrow, return, overdue and transfers need the librarian passphrase.
Viewing, validating and mining only need the public key.

## How to test the mining simulations

Each mining option confirms the records in the pending pool, so queue a
few first: borrow two books (option 2) and return one (option 3). Option
5 shows them waiting.

**Solo mining (option 6).** Enter a miner ID such as `MINER1`. Every
pending record is mined in order; for each block it prints the nonce, the
number of hash attempts and the time taken, then the UTXO set or balances
after confirmation. The miner receives 50 coins per block.

**Pool mining (option 7).** Four miners (`POOL-M1` to `POOL-M4`) get
random hash rates. They search each block together, and the program
prints which miner found it. At the end a table shows each miner's hash
rate, attempts, share of the work and reward, after a 2% pool fee paid to
`POOL-OPERATOR`.

**Cloud mining (option 8).** Enter the member renting the power and a
duration of 1 to 5 rounds. Each round costs 20 coins and tries 300
nonces. A found block pays 50 coins minus a 10% maintenance fee to the
member. The program prints cumulative rewards and fees each round, warns
when fees exceed rewards, and ends with gross earnings, total fees and
net profit.

**Seeing difficulty change the cost.** Run solo mining at difficulty 1,
then set difficulty 4 (option 15) and mine again: attempts go from tens to
tens of thousands. For cloud mining, difficulty 3 makes 300 attempts per
round rarely enough, so the rental turns unprofitable.

## Testing the edge cases

| Case | Steps | Expected |
|---|---|---|
| unknown book or member | borrow `BK999` or `ALU999` | `ERROR: Book or Member not found` |
| overdue, not returned | build with `-DLOAN_PERIOD_SECONDS=30`, borrow, wait, option 4, mine | OVERDUE block with reward 0 and no transaction |
| late return | after marking overdue, return the book and mine | reward 5, member credited 4 |
| insufficient balance | transfer more than the sender holds | `REJECTED: insufficient ...`, nothing changes |
| failed nonce | account model, transfer with a nonce other than the one shown in option 11 | `REJECTED: incorrect nonce` |
| reused nonce / double spend | make a transfer, then option 14 | account model: nonce already used; UTXO model: input already spent |
| unprofitable cloud rental | 5 rounds with only one pending record, or difficulty 3 | warning each round fees exceed rewards, negative net profit |
| tampering | option 16, or edit `chain.txt` / `transactions.txt` by hand and restart | tampered block named; edited transactions refused at startup |

## Files

```
q1_library_lending/
  main.c           arguments, menu, views, transfers, tamper demo
  library.c/h      LibrarySystem state, startup, login, pending pool, block confirmation
  lending.c/h      borrow, return (with reward), overdue
  mining.c/h       solo, pool and cloud mining
  blockchain.c/h   Block and BlockList, signing, proof of work, validation, block files
  transaction.c/h  Transaction, transaction ids, transactions.txt
  ledger.c/h       model selection, replay, balances, history, double-spend test
  utxo.c/h         UTXO set, input selection, change outputs, validation
  account.c/h      accounts, nonces, per-account history linked lists
  crypto.c/h       SHA-256, ECDSA keys, sign and verify
  registry.c/h     books.txt and members.txt
  input.c/h        line, number and passphrase prompts
  utils.c/h        string helpers
  books.txt        book registry
  members.txt      member registry
```

Created at runtime (not committed): `chain.txt` (confirmed blocks),
`pending.txt` (records waiting to be mined), `transactions.txt`
(confirmed token transactions) and `keys/` (librarian key pair).

To start over: `rm -rf chain.txt pending.txt transactions.txt keys/`.
A `chain.txt` from the first version cannot be loaded, because blocks now
have four more fields.

## Data formats

`chain.txt` and `pending.txt`, one block per line:

```
index|timestamp|book_id|book_title|member_id|member_name|action|token_reward|transaction_id|difficulty|nonce|previous_hash|signature|hash
```

`transactions.txt`, one confirmed transaction per line:

```
txid|type|sender|recipient|amount|fee|nonce|reference|timestamp
```

## Screenshots

| File | Shows |
|---|---|
| [images/01_startup.png](images/01_startup.png) | model choice, registries loaded, genesis block mined |
| [images/02_pending_pool.png](images/02_pending_pool.png) | borrow and return queued in the pending pool |
| [images/03_invalid_ids.png](images/03_invalid_ids.png) | unknown book and member rejected |
| [images/04_solo_mining.png](images/04_solo_mining.png) | solo mining: attempts per block, confirmations, miner reward |
| [images/05_utxo_set.png](images/05_utxo_set.png) | UTXO set after a reward and after a transfer with change |
| [images/06_insufficient_balance.png](images/06_insufficient_balance.png) | transfer rejected for insufficient funds |
| [images/07_double_spend.png](images/07_double_spend.png) | replayed transfer rejected in the UTXO model |
| [images/08_account_nonce.png](images/08_account_nonce.png) | wrong and reused nonce rejected, account history |
| [images/09_pool_mining.png](images/09_pool_mining.png) | pool reward sharing table |
| [images/10_cloud_mining.png](images/10_cloud_mining.png) | cloud rental rounds and summary |
| [images/11_cloud_unprofitable.png](images/11_cloud_unprofitable.png) | unprofitable rental warnings |
| [images/12_overdue_no_reward.png](images/12_overdue_no_reward.png) | overdue record with no transaction, then a late return |
| [images/13_records.png](images/13_records.png) | confirmed records with reward, nonce and signature status |
| [images/14_validate.png](images/14_validate.png) | chain and ledger validation |
| [images/15_tamper_demo.png](images/15_tamper_demo.png) | tamper demo: edit, then re-mine, still caught |
| [images/16_difficulty.png](images/16_difficulty.png) | attempts at difficulty 1 versus 4 |
