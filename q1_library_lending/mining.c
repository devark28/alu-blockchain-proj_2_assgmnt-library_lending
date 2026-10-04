#include "mining.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char id[ACCOUNT_ID_SIZE];
    int hash_rate;
    unsigned long attempts;
    int blocks_found;
    int reward;
} PoolMiner;

typedef struct {
    unsigned long attempts;
    int block_index;
    int reward;
    int rental_fee;
    int maintenance_fee;
} CloudRound;

static bool ready_to_mine(const LibrarySystem *library)
{
    if (library->pending.count == 0) {
        printf("The pending pool is empty, there is nothing to mine. Borrow or return a book first.\n");
        return false;
    }
    if (!library_is_consistent(library, false)) {
        printf("ERROR: the chain failed validation, mining is blocked until it is restored.\n");
        return false;
    }
    return true;
}

/* Takes the first valid pending record and puts it on top of the chain, ready for proof of work. */
static bool take_next_block(LibrarySystem *library, Block *block)
{
    while (library->pending.count > 0) {
        if (next_pending_is_valid(library)) {
            *block = library->pending.blocks[0];
            prepare_for_mining(block, &library->chain, library->difficulty);
            return true;
        }
    }
    return false;
}

static double elapsed_ms(clock_t start)
{
    return (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
}

/*
 * One miner confirms every pending record, searching nonces until each hash
 * meets the difficulty. The miner then receives BLOCK_REWARD per block, on
 * top of the token rewards the members already received.
 */
void mine_solo(LibrarySystem *library, const char *miner_id)
{
    if (!ready_to_mine(library)) {
        return;
    }
    printf("\nSolo miner %s, %d pending records, difficulty %d (hash must start with %d zeros)\n\n",
           miner_id, library->pending.count, library->difficulty, library->difficulty);

    Block block;
    int first_index = library->chain.count;
    int blocks_mined = 0;
    unsigned long total_attempts = 0;

    while (take_next_block(library, &block)) {
        unsigned long attempts = 0;
        clock_t start = clock();
        search_nonce(&block, ULONG_MAX, &attempts);
        printf("Mined block %d: nonce %lu found after %lu hash attempts (%.1f ms)\n  hash %s\n",
               block.index, block.nonce, attempts, elapsed_ms(start), block.hash);

        if (!confirm_first_pending(library, &block)) {
            return;
        }
        blocks_mined++;
        total_attempts += attempts;
    }
    if (blocks_mined == 0) {
        printf("No valid pending records were left to mine.\n");
        return;
    }

    int reward = blocks_mined * BLOCK_REWARD;
    char reference[REFERENCE_SIZE];
    snprintf(reference, sizeof(reference), "blocks %d-%d", first_index, first_index + blocks_mined - 1);
    if (!pay_mining_reward(library, miner_id, reward, reference)) {
        return;
    }

    printf("Solo mining summary\n");
    printf("  Miner              %s\n", miner_id);
    printf("  Blocks confirmed   %d\n", blocks_mined);
    printf("  Total attempts     %lu\n", total_attempts);
    printf("  Mining reward      %d coins (%d per block)\n", reward, BLOCK_REWARD);
}

/*
 * Several miners search the same block at once. Each round is split into
 * time slices and in every slice each miner tries its share of its hash
 * rate, so faster miners do proportionally more work and nobody always goes
 * first. They share one nonce sequence, so no two miners repeat the same
 * work. Rewards are split by attempts, after the pool fee.
 */
void mine_pool(LibrarySystem *library)
{
    if (!ready_to_mine(library)) {
        return;
    }

    PoolMiner miners[POOL_MINER_COUNT];
    printf("\nMining pool, %d pending records, difficulty %d\n", library->pending.count, library->difficulty);
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        memset(&miners[i], 0, sizeof(miners[i]));
        snprintf(miners[i].id, sizeof(miners[i].id), "POOL-M%d", i + 1);
        int rate_steps = (POOL_MAX_HASH_RATE - POOL_MIN_HASH_RATE) / POOL_SLICES_PER_ROUND + 1;
        miners[i].hash_rate = POOL_MIN_HASH_RATE + (rand() % rate_steps) * POOL_SLICES_PER_ROUND;
        printf("  %s hash rate %d attempts per round\n", miners[i].id, miners[i].hash_rate);
    }
    printf("\n");

    Block block;
    int blocks_mined = 0;

    while (take_next_block(library, &block)) {
        int finder = -1;
        int round = 0;
        while (finder < 0) {
            round++;
            for (int slice = 0; slice < POOL_SLICES_PER_ROUND && finder < 0; slice++) {
                for (int i = 0; i < POOL_MINER_COUNT && finder < 0; i++) {
                    unsigned long slice_attempts = (unsigned long)(miners[i].hash_rate / POOL_SLICES_PER_ROUND);
                    if (search_nonce(&block, slice_attempts, &miners[i].attempts)) {
                        finder = i;
                    }
                }
            }
        }
        miners[finder].blocks_found++;
        printf("Block %d found by %s in round %d, nonce %lu\n  hash %s\n",
               block.index, miners[finder].id, round, block.nonce, block.hash);

        if (!confirm_first_pending(library, &block)) {
            return;
        }
        blocks_mined++;
    }
    if (blocks_mined == 0) {
        printf("No valid pending records were left to mine.\n");
        return;
    }

    unsigned long total_attempts = 0;
    int top_miner = 0;
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        total_attempts += miners[i].attempts;
        if (miners[i].attempts > miners[top_miner].attempts) {
            top_miner = i;
        }
    }

    int gross_reward = blocks_mined * BLOCK_REWARD;
    int pool_fee = gross_reward * POOL_FEE_PERCENT / 100;
    int shared_reward = gross_reward - pool_fee;
    int distributed = 0;
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        miners[i].reward = (int)((unsigned long long)shared_reward * miners[i].attempts / total_attempts);
        distributed += miners[i].reward;
    }
    miners[top_miner].reward += shared_reward - distributed;

    printf("Pool reward sharing: %d blocks x %d = %d coins, pool fee %d%% = %d, shared %d\n",
           blocks_mined, BLOCK_REWARD, gross_reward, POOL_FEE_PERCENT, pool_fee, shared_reward);
    printf("  %-10s %9s %10s %8s %7s %7s\n", "miner", "hash rate", "attempts", "share", "blocks", "reward");
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        double share = 100.0 * (double)miners[i].attempts / (double)total_attempts;
        printf("  %-10s %9d %10lu %7.2f%% %7d %7d\n", miners[i].id, miners[i].hash_rate,
               miners[i].attempts, share, miners[i].blocks_found, miners[i].reward);
    }
    printf("  %-10s %9s %10lu %7.2f%% %7d %7d\n", "total", "", total_attempts, 100.0, blocks_mined, shared_reward);
    printf("  Rounding remainder (%d) goes to the miner with the most attempts.\n", shared_reward - distributed);

    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        if (!pay_mining_reward(library, miners[i].id, miners[i].reward, "pool share")) {
            return;
        }
    }
    pay_mining_reward(library, POOL_OPERATOR, pool_fee, "pool fee");
}

static void print_cloud_summary(const CloudRound rounds[], int round_count)
{
    int gross = 0;
    int rental_total = 0;
    int maintenance_total = 0;

    printf("\nCloud mining summary\n");
    printf("  %-5s %8s %6s %7s %7s %11s %10s %9s\n",
           "round", "attempts", "block", "reward", "rental", "maintenance", "net round", "net total");
    for (int i = 0; i < round_count; i++) {
        const CloudRound *round = &rounds[i];
        int net_round = round->reward - round->rental_fee - round->maintenance_fee;
        gross += round->reward;
        rental_total += round->rental_fee;
        maintenance_total += round->maintenance_fee;
        char block_text[16];
        if (round->block_index < 0) {
            snprintf(block_text, sizeof(block_text), "-");
        } else {
            snprintf(block_text, sizeof(block_text), "%d", round->block_index);
        }
        printf("  %-5d %8lu %6s %7d %7d %11d %10d %9d\n", i + 1, round->attempts, block_text,
               round->reward, round->rental_fee, round->maintenance_fee, net_round,
               gross - rental_total - maintenance_total);
    }

    int fees = rental_total + maintenance_total;
    printf("  Gross earnings   %d coins\n", gross);
    printf("  Total fees       %d coins (rental %d + maintenance %d)\n", fees, rental_total, maintenance_total);
    printf("  Net profit       %d coins\n", gross - fees);
    if (gross - fees < 0) {
        printf("  WARNING: this rental was unprofitable overall.\n");
    }
}

/*
 * A member rents CLOUD_HASH_RATE attempts per round for 1 to 5 rounds. Each
 * round costs CLOUD_RENTAL_FEE. When the rented power finds a block, the
 * block reward minus the maintenance fee is credited to the member. The
 * rental fee is paid to the provider outside the token ledger, so it only
 * appears in the profit calculation.
 */
void mine_cloud(LibrarySystem *library, const char *renter_id, int rounds)
{
    if (!ready_to_mine(library)) {
        return;
    }

    printf("\nCloud rental for %s: %d rounds, %d attempts per round, rental fee %d per round, "
           "maintenance %d%% of rewards, difficulty %d\n\n",
           renter_id, rounds, CLOUD_HASH_RATE, CLOUD_RENTAL_FEE, CLOUD_MAINTENANCE_PERCENT, library->difficulty);

    CloudRound results[MAX_RENTAL_ROUNDS];
    Block block;
    bool have_block = false;
    int cumulative_rewards = 0;
    int cumulative_fees = 0;

    for (int round = 1; round <= rounds; round++) {
        CloudRound *result = &results[round - 1];
        memset(result, 0, sizeof(*result));
        result->block_index = -1;
        result->rental_fee = CLOUD_RENTAL_FEE;

        if (!have_block) {
            have_block = take_next_block(library, &block);
        }

        if (!have_block) {
            printf("Round %d: pending pool is empty, the rented power is idle\n", round);
        } else if (search_nonce(&block, CLOUD_HASH_RATE, &result->attempts)) {
            printf("Round %d: block %d found after %lu attempts, nonce %lu\n",
                   round, block.index, result->attempts, block.nonce);
            if (!confirm_first_pending(library, &block)) {
                return;
            }
            result->block_index = block.index;
            result->reward = BLOCK_REWARD;
            result->maintenance_fee = BLOCK_REWARD * CLOUD_MAINTENANCE_PERCENT / 100;

            char reference[REFERENCE_SIZE];
            snprintf(reference, sizeof(reference), "cloud block %d", block.index);
            if (!pay_mining_reward(library, renter_id, result->reward - result->maintenance_fee, reference)) {
                return;
            }
            have_block = false;
        } else {
            printf("Round %d: no valid hash in %lu attempts, block %d carries over to the next round\n",
                   round, result->attempts, block.index);
        }

        cumulative_rewards += result->reward;
        cumulative_fees += result->rental_fee + result->maintenance_fee;
        printf("  cumulative rewards %d, cumulative fees %d\n", cumulative_rewards, cumulative_fees);
        if (cumulative_fees > cumulative_rewards) {
            printf("  WARNING: after round %d the fees exceed the rewards, the rental is unprofitable so far\n", round);
        }
    }

    print_cloud_summary(results, rounds);
}
