# Token Rewards and Mining: how it works

## Goal

The base tracker recorded every borrow and return as a signed, hash-linked
block. This version turns those records into an economy:

- returning a book earns the member tokens (10 on time, 5 late, 0 if the
  book is never returned)
- no record reaches the chain until a miner confirms it with proof of work
- member balances only change once that confirmation happens
- balances can be kept in either of the two ledger designs used by real
  blockchains: Bitcoin's UTXO model or Ethereum's account model
- confirmation can be done by a solo miner, a mining pool or rented cloud
  mining power

The registries, ECDSA signatures, passphrase-protected key and tamper
detection from the base version are unchanged and are described in its
[README](https://github.com/devark28/alu-blockchain-proj_1_assgmnt-library_lending/blob/main/q1_library_lending/README.md).
This document covers what was added and why.

## Module overview

| File | Responsibility |
|---|---|
| `blockchain.c/h` | `Block` and the growable `BlockList`; signing, proof of work, validation, reading and writing block files |
| `transaction.c/h` | the model-independent `Transaction`, transaction ids, `transactions.txt` |
| `utxo.c/h` | UTXO set, input selection, change outputs, UTXO validation |
| `account.c/h` | accounts with balance and nonce, history linked lists, account validation |
| `ledger.c/h` | runs whichever model was chosen, replays the log, balances, history, double-spend test |
| `library.c/h` | `LibrarySystem` state, startup, login, pending pool, confirming mined blocks, ledger cross-check |
| `lending.c/h` | borrow, return with reward, mark overdue |
| `mining.c/h` | solo, pool and cloud mining |
| `main.c` | arguments, menu, views, transfers, tamper demo |
| `registry.c/h`, `crypto.c/h`, `utils.c/h`, `input.c/h` | registries, OpenSSL wrappers, string and input helpers |

The dependencies run one way. `utxo.c` and `account.c` know only about
`Transaction`; `ledger.c` chooses between them; `library.c` ties the
chain, the pool and the ledger together; `mining.c` and `lending.c` use
`library.c`; `main.c` uses everything. Neither ledger model knows that
blocks exist.

## From lending to tokens

### The flow

```
librarian: borrow / return / overdue
        │  (registry lookup, loan state check, reward decided, record signed)
        ▼
pending.txt  ── records wait here, nothing is on the chain yet
        │
miner: solo / pool / cloud
        │  (signature and reward checked, proof of work found)
        ▼
chain.txt  ── block confirmed
        │
reward transaction confirmed ──► transactions.txt ──► UTXO set or account balance
        │
mining reward paid to the miner(s)
```

### Rewards

When a book is returned, `return_book()` looks at the open loan:

| Situation | `token_reward` | Transaction | Member receives |
|---|---|---|---|
| returned within the loan period | 10 | yes | 9 |
| returned after the loan period, or after being marked OVERDUE | 5 | yes | 4 |
| marked OVERDUE and not returned | 0 | none | nothing |
| borrowed | 0 | none | nothing |

The 1 coin fee is taken out of the reward before the member is credited.
The fee is burned, not paid to anyone (see design choices).

### The reward transaction is derived from the block

The reward transaction is built entirely from fields the block already
has:

```
type REWARD, sender LIBRARY, recipient = member_id,
amount = token_reward - 1, fee = 1, reference = book_id, timestamp = block timestamp
```

Its id is the SHA-256 of those fields, and that id is stored in the
block's `transaction_id`. Because nothing extra is needed, anyone holding
the chain can rebuild the transaction and check it. The validator does
exactly that for every block (the `reward` column), so the reward amount,
the member and the transaction can never drift apart. Since
`token_reward` and `transaction_id` are also covered by the librarian's
signature, changing a reward means breaking the signature.

### The pending pool

Records are not appended to the chain when they are created. They are
signed and written to `pending.txt`, and wait there until mined. This
changes a few things compared to the base version:

- **What the signature covers.** The librarian signs the lending data:
  timestamp, book, member, action, `token_reward`, `transaction_id`. The
  block's position (`index`, `previous_hash`) and the mining fields
  (`difficulty`, `nonce`) are not known yet, so they are not signed. The
  block hash covers all of it, including the signature.
- **Book state includes pending records.** `find_open_loan()` checks the
  pool before the chain, so a book that is borrowed but not yet mined
  cannot be borrowed again, and a pending borrow can already be returned.
- **Miners validate before mining.** `next_pending_is_valid()` checks the
  signature and the reward of the first pending record. Anything invalid
  (for example an edited `pending.txt`) is dropped instead of being mined
  into the chain.

### Confirmation

`confirm_first_pending()` is the one place where a record becomes real:

1. append the mined block to `chain.txt` and the in-memory chain
2. remove it from the pool and rewrite `pending.txt`
3. if it has a reward, submit the reward transaction to the ledger, which
   validates it, writes it to `transactions.txt` and updates balances
4. print the UTXO set or the account balances

Only step 3 changes a balance, so a member is never credited for a record
that is not on the chain.

## Transaction models

All confirmed transactions share one model-independent format:

```c
typedef struct {
    char txid[HASH_HEX_SIZE];
    char type[TX_TYPE_SIZE];          /* REWARD, MINING or TRANSFER */
    char sender[ACCOUNT_ID_SIZE];     /* LIBRARY, COINBASE or an account */
    char recipient[ACCOUNT_ID_SIZE];
    int amount;                       /* what the recipient receives */
    int fee;                          /* paid on top by the sender */
    int nonce;                        /* -1 for minted transactions */
    char reference[REFERENCE_SIZE];
    time_t timestamp;
} Transaction;
```

`LIBRARY` (lending rewards) and `COINBASE` (mining rewards) are minting
senders: they create new coins, so they have no balance to check. Account
IDs are member IDs from `members.txt`, plus miner IDs that appear when a
miner is paid. Every member starts at 0 and only gains tokens from
confirmed returns or transfers.

The ledger applies each transaction to the model chosen for the session.
The log is the same for both, so the history can be opened as UTXOs in one
session and as accounts in the next, with identical balances. This was
tested by building a history in the UTXO model and reopening it in the
account model.

### UTXO model

The ledger is a set of unspent outputs. Each `Utxo` records which
transaction created it, its output index, its owner and its amount.
Nobody has a stored balance: a balance is the sum of the outputs a person
owns.

**Minted transactions** have no inputs and one output.

**A transfer** is built by `utxo_build_transaction()`:

1. collect the sender's outputs, oldest first, until they cover
   `amount + fee`
2. if they never do, reject: insufficient funds
3. output 0 pays the recipient `amount`
4. if the inputs were more than `amount + fee`, output 1 returns the
   excess to the sender as change

The fee is the difference between inputs and outputs, so it simply
disappears from the set.

**Validation** (`utxo_validate()`) checks that every input is currently in
the set, owned by the sender, and not used twice in the same transaction,
and that inputs cover outputs plus the fee.

**Double spending** is prevented because applying a transaction removes
its inputs from the set. Any later transaction that names one of them
fails the first check. Option 14 demonstrates this by re-submitting the
last transfer with its original inputs:

```
Re-submitting transfer 86ceaf26de954de2... (ALU001 -> ALU003, amount 5, nonce 0) under the UTXO model
REJECTED: input f280c4a968f3...:0 is not in the UTXO set (already spent or never existed)
Double spend prevented, nothing was changed.
```

A transfer with change, from a real run (ALU001 had one 9 coin output
and sent 5):

```
UTXO set, unspent outputs: 3
  txid                 output owner            amount
  ec3723d3ce85cc36...  0      MINER1           150
  86ceaf26de954de2...  0      ALU003           5
  86ceaf26de954de2...  1      ALU001           3
```

9 in, 5 to ALU003, 3 change back to ALU001, 1 fee burned. The full set is
printed after every confirmed block and every transfer.

### Account model

Each `Account` holds an id, a balance, a nonce, and the head and tail of
a linked list of `HistoryNode`s. The table of accounts is a growable
array; members get an account at balance 0 when the session starts, and
miners get one the first time they are paid.

**Validation** (`account_validate()`) for a transfer:

- the sender must exist
- the transaction's nonce must equal the sender's next nonce; a lower one
  is reported as reused, a higher one as incorrect
- the balance must cover `amount + fee`

**Applying** debits the sender `amount + fee`, increments the sender's
nonce, credits the recipient, and appends the transaction to both
accounts' history lists. Appending uses the tail pointer, so it is O(1)
and the list stays in order.

**The nonce** is what stops a replay in this model. There are no inputs
to spend, so the same signed-off transfer could otherwise be applied
twice. From a real run:

```
REJECTED: incorrect nonce 5 for ALU003, the next nonce is 1
REJECTED: nonce 1 was already used by ALU003, the next nonce is 2
```

**History** (option 13) walks the account's list:

```
Transaction history for ALU003 (balance 0, next nonce 2)
  type      sender     recipient  amount  fee nonce  txid
  TRANSFER  ALU001     ALU003          5    1     0  86ceaf26de954de2...
  TRANSFER  ALU003     ALU005          2    1     0  2cde652ee7e4cdf2...
  TRANSFER  ALU003     ALU005          1    1     1  9807d074657294b8...
```

In the UTXO model the same option filters the confirmed transaction list,
since that model has no accounts to hang a list on.

### Comparing the two models

| | UTXO | Account |
|---|---|---|
| State | set of unspent outputs | one balance and nonce per account |
| Balance | computed by summing outputs | read directly |
| Double-spend protection | inputs are removed when spent | nonce must match exactly |
| Change | explicit change output | not needed, balance is debited |
| Fee | inputs minus outputs | debited with the amount |
| History | derived from transactions | linked list per account |
| Parallel validation | transactions with different inputs are independent | transactions from one sender must be ordered by nonce |
| Privacy | owner can use a new output each time | one visible account |
| Cost of a balance lookup | O(outputs) | O(accounts) to find it, then O(1) |

The UTXO model needs more bookkeeping (input selection, change) but its
rules are local: whether a transaction is valid depends only on the
outputs it spends. The account model is simpler to read and cheaper for
balances, but every transfer from one account depends on all earlier
ones through the nonce.

## Mining

### Proof of work

`search_nonce()` is the core loop:

```
compute the block hash
if it starts with `difficulty` zero hex characters: done
otherwise increment nonce and try again
```

It takes a maximum number of attempts, so the same function serves solo
mining (no limit) and pool or cloud mining (a fixed number per round).

`difficulty` (1 to 4, default 2) is stored in each block, so validation
always checks a block against the difficulty it was mined at, even if the
setting changes later. Each extra zero multiplies the expected work by
16:

| Difficulty | Expected attempts |
|---|---|
| 1 | 16 |
| 2 | 256 |
| 3 | 4,096 |
| 4 | 65,536 |

The hash covers `index | difficulty | nonce | previous_hash | signed data
| signature`. Changing anything, including the nonce, gives a completely
different hash, which is why there is no shortcut to searching.

### Solo mining

One miner takes every pending record in order, verifies it, places it on
top of the chain and searches nonces until the hash meets the
difficulty. It prints the nonce, the attempts and the time for each
block. The miner is paid `50 x blocks` through a `MINING` transaction
from `COINBASE`, separate from the members' reward transactions.

### Pool mining

Four miners get random hash rates between 50 and 300 attempts per round.
Each round is split into 10 time slices, and in each slice every miner
tries a tenth of its hash rate. This approximates miners working in
parallel: a faster miner does proportionally more work, and no miner
always gets the first try. All miners continue one shared nonce sequence,
the way a pool hands out non-overlapping work.

When the pool has confirmed every pending block, rewards are shared:

```
gross reward   = 50 x blocks
pool fee       = 2% of gross            -> POOL-OPERATOR
miner reward_i = (attempts_i / total_attempts) x (gross - pool fee)
```

Coins are whole numbers, so each share is rounded down and the remainder
(at most a few coins) goes to the miner with the most attempts. From a
real run:

```
Pool reward sharing: 2 blocks x 50 = 100 coins, pool fee 2% = 2, shared 98
  miner      hash rate   attempts    share  blocks  reward
  POOL-M1          100         57   16.52%       1      16
  POOL-M2           50         25    7.25%       0       7
  POOL-M3          250        125   36.23%       0      35
  POOL-M4          300        138   40.00%       1      40
  total                       345  100.00%       2      98
```

POOL-M3 found no block but earned 35 coins, because rewards follow work
done, not luck. That is the point of a pool: members get a steady income
proportional to their hash rate instead of rare large payouts.

### Cloud mining

A member rents 300 attempts per round for 1 to 5 rounds:

- each round costs a rental fee of 20 coins
- when the rented power finds a block, the 50 coin reward minus a 10%
  maintenance fee is credited to the member through the ledger
- an unfinished block carries over to the next round
- once the pool is empty, the remaining rounds are idle but still paid

After every round it prints cumulative rewards and fees, and warns as soon
as fees exceed rewards. The summary shows gross earnings, total fees and
net profit. Two real runs show what drives profit:

```
difficulty 2, 5 rounds, 2 pending records
  Gross earnings   100 coins
  Total fees       110 coins (rental 100 + maintenance 10)
  Net profit       -10 coins

difficulty 3, 3 rounds, 1 pending record
  Gross earnings   0 coins
  Total fees       60 coins (rental 60 + maintenance 0)
  Net profit       -60 coins
```

At difficulty 2 a block needs about 256 attempts, so 300 per round
usually finds one, and the rental pays while there is work; renting more
rounds than there are pending blocks loses money. At difficulty 3 a block
needs about 4,096 attempts, so 300 per round almost never succeeds. This
is the real economics of cloud mining: the contract price is fixed, the
reward depends on difficulty.

## Validation and integrity

`validate_chain()` now checks six things per block:

| Check | Catches |
|---|---|
| index matches position | removed, inserted or reordered blocks |
| hash matches recomputed hash | any edit to any field |
| `previous_hash` matches the previous block | broken links |
| proof of work: recomputed hash meets the block's difficulty | blocks that were never mined, or edited after mining |
| signature verifies | edits to the lending data, forged records |
| reward matches transaction | a reward or transaction id that does not belong to the block |

On top of that, `library_is_consistent()` checks that every reward on the
chain is confirmed in `transactions.txt` with the same member and amount,
and `load_transactions()` recomputes every transaction id, so an edited
amount in the log is refused at startup. Writing records and mining are
blocked while anything fails.

### Tamper demo

Option 16 attacks a copy of the chain. Step 1 raises a returned book's
reward from 10 to 100: hash, proof of work, signature and reward checks
all fail on that block. Step 2 does everything an attacker can do without
the key: rebuild the reward transaction id, then redo the proof of work
for that block and every block after it.

```
Re-mined 8 blocks in 1719 hash attempts.
Block 3   index OK   hash OK   link OK   pow OK   signature FAIL reward OK   <-- TAMPERED
```

Proof of work makes rewriting history cost work proportional to the
difficulty and to the number of later blocks; on a real network that cost
is enormous. Here it is cheap, which is why the signature is still the
check that holds.

## Design choices and assumptions

- **Fees are burned.** The 1 coin fee leaves circulation instead of going
  to a miner, as with Ethereum's base fee since EIP-1559. Miners are paid
  only by block rewards, which keeps the three mining methods comparable.
- **Transfers are confirmed immediately.** A lending block carries one
  lending record, so manual transfers are validated and applied at once
  rather than waiting in the pool. Lending records always go through
  mining.
- **Transfers are authorised by the librarian.** Members have no keys of
  their own, so a transfer requires librarian login, the same as lending
  actions.
- **Minted transactions have no nonce.** Like Bitcoin's coinbase, a
  reward or mining payout is not sent by an account, so nonce rules apply
  only to transfers.
- **Whole coins.** All amounts are integers to avoid floating-point
  rounding in balances. Pool shares round down and the remainder goes to
  the biggest contributor.
- **Cloud rental fees are paid outside the ledger.** The rented reward is
  credited on-chain minus maintenance; the per-round rental fee is a
  payment to the provider, counted only in the profit calculation.
- **Mining reward is 50 coins per block** for all three methods, so their
  results can be compared directly.
- **Miners do not need the librarian key.** They only verify signatures
  with the public key, so confirming records is separate from creating
  them.
- **One difficulty per block.** The difficulty used is stored in the block
  so changing the setting never invalidates earlier blocks.

## Memory management

Everything that grows is allocated dynamically and freed at exit:

| Structure | Storage |
|---|---|
| chain and pending pool | `BlockList`, array grown with `realloc` (doubling) |
| confirmed transactions | `TransactionList`, same pattern |
| UTXO set | `UtxoSet`, same pattern, spent outputs removed with `memmove` |
| accounts | `AccountTable`, same pattern |
| account history | singly linked list per account, `malloc` per node, freed node by node |

A full session (borrow, return, overdue, all three mining methods,
transfers, replay test, validation, tamper demo) was run under
AddressSanitizer and UndefinedBehaviorSanitizer with no errors and no
leaks reported.

## Edge case results

| Case | Result |
|---|---|
| unknown member `ALU999` | `ERROR: Book or Member not found`, nothing queued |
| book marked overdue and not returned | OVERDUE block mined with reward 0 and no transaction id |
| late return | reward 5, member credited 4 |
| transfer of 10 with a balance of 3 | `REJECTED: insufficient funds, ALU001 has 3 coins in unspent outputs but needs 11 (10 + 1 fee)` |
| account transfer with nonce 5, expected 1 | `REJECTED: incorrect nonce 5 for ALU003, the next nonce is 1` |
| replaying the last transfer | UTXO: input already spent; account: nonce already used |
| 5 round rental with 2 pending blocks | warning at round 5, net profit -10 |
| 3 round rental at difficulty 3 | warning every round, net profit -60 |
| edited amount in `transactions.txt` | refused at startup, txid does not match contents |
| edited name in `chain.txt` | startup warning, block named as tampered, writes and mining blocked |
| edited record in `pending.txt` | shown with INVALID signature, dropped by the miner |
