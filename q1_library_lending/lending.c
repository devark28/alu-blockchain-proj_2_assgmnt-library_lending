#include "lending.h"
#include "input.h"
#include "transaction.h"

#include <stdio.h>
#include <string.h>

#define ID_INPUT_SIZE 64

static bool lookup_book_and_member(const LibrarySystem *library, const Book **book, const Member **member)
{
    char book_id[ID_INPUT_SIZE];
    char member_id[ID_INPUT_SIZE];
    prompt_text("Book ID: ", book_id, sizeof(book_id));
    prompt_text("Member ID: ", member_id, sizeof(member_id));

    *book = find_book(&library->registry, book_id);
    *member = find_member(&library->registry, member_id);
    if (*book == NULL || *member == NULL) {
        printf("ERROR: Book or Member not found\n");
        return false;
    }
    return true;
}

static void report_queued(const LibrarySystem *library, const Block *record)
{
    printf("%s record for %s (%s) by %s (%s) added to the pending pool, %d waiting to be mined.\n",
           record->action, record->book_id, record->book_title, record->member_id, record->member_name,
           library->pending.count);
}

void borrow_book(LibrarySystem *library)
{
    const Book *book;
    const Member *member;
    if (!require_login(library) || !lookup_book_and_member(library, &book, &member)) {
        return;
    }

    bool is_pending;
    const Block *open_loan = find_open_loan(library, book->book_id, &is_pending);
    if (open_loan != NULL) {
        printf("ERROR: %s is already on loan to %s (%s)%s\n", book->book_id, open_loan->member_id,
               open_loan->member_name, is_pending ? ", waiting to be mined" : "");
        return;
    }

    Block record;
    create_lending_record(&record, book, member, ACTION_BORROWED, 0, "");
    if (add_to_pending_pool(library, &record)) {
        report_queued(library, &record);
    }
}

/*
 * A return earns REWARD_ON_TIME if it happens within the loan period and
 * REWARD_LATE otherwise (including books already marked OVERDUE). The
 * reward transaction is created now and its id is stored in the record, but
 * the member is only credited once the record is mined.
 */
void return_book(LibrarySystem *library)
{
    const Book *book;
    const Member *member;
    if (!require_login(library) || !lookup_book_and_member(library, &book, &member)) {
        return;
    }

    bool is_pending;
    const Block *open_loan = find_open_loan(library, book->book_id, &is_pending);
    if (open_loan == NULL) {
        printf("ERROR: %s has no open BORROWED record (never borrowed or already returned)\n", book->book_id);
        return;
    }
    if (strcmp(open_loan->member_id, member->member_id) != 0) {
        printf("ERROR: %s is on loan to %s (%s), not %s\n",
               book->book_id, open_loan->member_id, open_loan->member_name, member->member_id);
        return;
    }

    bool was_overdue = strcmp(open_loan->action, ACTION_OVERDUE) == 0;
    bool is_late = was_overdue || time(NULL) > open_loan->timestamp + LOAN_PERIOD_SECONDS;
    int token_reward = is_late ? REWARD_LATE : REWARD_ON_TIME;

    Block record;
    Transaction reward;
    create_lending_record(&record, book, member, ACTION_RETURNED, token_reward, "");
    make_reward_transaction(&reward, member->member_id, token_reward, book->book_id, record.timestamp);
    strcpy(record.transaction_id, reward.txid);

    if (add_to_pending_pool(library, &record)) {
        report_queued(library, &record);
        printf("Returned %s: reward %d coins, %d after the %d coin fee. Reward transaction %.16s... is pending.\n",
               is_late ? "late" : "on time", token_reward, reward.amount, reward.fee, reward.txid);
    }
}

/*
 * Marks a loan overdue once its period has passed. Overdue records carry no
 * reward and no transaction; the book can still be returned late.
 */
void mark_overdue(LibrarySystem *library)
{
    if (!require_login(library)) {
        return;
    }

    char book_id[ID_INPUT_SIZE];
    prompt_text("Book ID: ", book_id, sizeof(book_id));
    const Book *book = find_book(&library->registry, book_id);
    if (book == NULL) {
        printf("ERROR: Book or Member not found\n");
        return;
    }

    bool is_pending;
    const Block *open_loan = find_open_loan(library, book->book_id, &is_pending);
    if (open_loan == NULL) {
        printf("ERROR: %s is not on loan\n", book->book_id);
        return;
    }
    if (strcmp(open_loan->action, ACTION_OVERDUE) == 0) {
        printf("ERROR: %s is already marked overdue\n", book->book_id);
        return;
    }

    time_t due_time = open_loan->timestamp + LOAN_PERIOD_SECONDS;
    if (time(NULL) <= due_time) {
        char due_text[32];
        format_timestamp(due_time, due_text, sizeof(due_text));
        printf("ERROR: %s is not overdue yet, it is due on %s\n", book->book_id, due_text);
        return;
    }

    const Member *member = find_member(&library->registry, open_loan->member_id);
    if (member == NULL) {
        printf("ERROR: Book or Member not found\n");
        return;
    }

    Block record;
    create_lending_record(&record, book, member, ACTION_OVERDUE, 0, "");
    if (add_to_pending_pool(library, &record)) {
        report_queued(library, &record);
        printf("No reward transaction is created for an overdue book that has not been returned.\n");
    }
}
