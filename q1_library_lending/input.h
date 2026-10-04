#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stddef.h>

bool read_line(char *buffer, size_t size);
void prompt_text(const char *label, char *buffer, size_t size);
bool prompt_number(const char *label, int min, int max, int *value);
void prompt_passphrase(const char *label, char *buffer, size_t size);

#endif
