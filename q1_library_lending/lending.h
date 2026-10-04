#ifndef LENDING_H
#define LENDING_H

#include "library.h"

#define REWARD_ON_TIME 10
#define REWARD_LATE 5

#ifndef LOAN_PERIOD_SECONDS
#define LOAN_PERIOD_SECONDS (14 * 24 * 60 * 60)
#endif

void borrow_book(LibrarySystem *library);
void return_book(LibrarySystem *library);
void mark_overdue(LibrarySystem *library);

#endif
