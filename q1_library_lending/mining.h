#ifndef MINING_H
#define MINING_H

#include "library.h"

#define BLOCK_REWARD 50

#define POOL_MINER_COUNT 4
#define POOL_FEE_PERCENT 2
#define POOL_MIN_HASH_RATE 50
#define POOL_MAX_HASH_RATE 300
#define POOL_SLICES_PER_ROUND 10
#define POOL_OPERATOR "POOL-OPERATOR"

#define CLOUD_HASH_RATE 300
#define CLOUD_RENTAL_FEE 20
#define CLOUD_MAINTENANCE_PERCENT 10
#define MIN_RENTAL_ROUNDS 1
#define MAX_RENTAL_ROUNDS 5

void mine_solo(LibrarySystem *library, const char *miner_id);
void mine_pool(LibrarySystem *library);
void mine_cloud(LibrarySystem *library, const char *renter_id, int rounds);

#endif
