#include "input.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define NUMBER_INPUT_SIZE 32

/* Reads one line from stdin, discarding whatever does not fit in buffer. */
bool read_line(char *buffer, size_t size)
{
    if (fgets(buffer, (int)size, stdin) == NULL) {
        buffer[0] = '\0';
        return false;
    }

    size_t length = strcspn(buffer, "\n");
    if (buffer[length] != '\n') {
        int extra;
        while ((extra = getchar()) != '\n' && extra != EOF) {
        }
    }
    buffer[length] = '\0';
    trim_whitespace(buffer);
    return true;
}

void prompt_text(const char *label, char *buffer, size_t size)
{
    printf("%s", label);
    fflush(stdout);
    read_line(buffer, size);
}

bool prompt_number(const char *label, int min, int max, int *value)
{
    char input[NUMBER_INPUT_SIZE];
    printf("%s", label);
    fflush(stdout);
    if (!read_line(input, sizeof(input))) {
        return false;
    }

    char *end;
    errno = 0;
    long number = strtol(input, &end, 10);
    if (input[0] == '\0' || *end != '\0' || errno != 0 || number < min || number > max) {
        printf("ERROR: enter a whole number from %d to %d\n", min, max);
        return false;
    }
    *value = (int)number;
    return true;
}

/* Turns terminal echo off while the passphrase is typed. */
void prompt_passphrase(const char *label, char *buffer, size_t size)
{
    struct termios original_settings;
    bool is_terminal = tcgetattr(STDIN_FILENO, &original_settings) == 0;

    if (is_terminal) {
        struct termios hidden_settings = original_settings;
        hidden_settings.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &hidden_settings);
    }

    prompt_text(label, buffer, size);

    if (is_terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_settings);
        printf("\n");
    }
}
