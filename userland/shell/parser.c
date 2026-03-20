#include "parser.h"

static const char* skip_spaces(const char* text) {
    int i = 0;

    while (text[i] == ' ' || text[i] == '\t') {
        i++;
    }

    return text + i;
}

int parser_parse_line(const char* line, char tokens[][64], const char* argv[], int max_args, int token_max) {
    int argc = 0;
    int index = 0;

    if (line == 0) {
        return 0;
    }

    line = skip_spaces(line);

    while (line[index] != '\0') {
        while (line[index] == ' ' || line[index] == '\t') {
            index++;
        }

        if (line[index] == '\0') {
            break;
        }

        if (argc >= max_args) {
            break;
        }

        int token_length = 0;
        char quote = 0;

        if (line[index] == '"' || line[index] == '\'') {
            quote = line[index];
            index++;
        }

        while (line[index] != '\0') {
            char current = line[index];

            if (current == '\\' && line[index + 1] != '\0') {
                index++;
                current = line[index];
            } else if (quote != 0 && current == quote) {
                index++;
                break;
            } else if (quote == 0 && (current == ' ' || current == '\t')) {
                break;
            }

            if (token_length < token_max - 1) {
                tokens[argc][token_length] = current;
                token_length++;
            }

            index++;
        }

        tokens[argc][token_length] = '\0';
        argv[argc] = tokens[argc];
        argc++;

        while (line[index] == ' ' || line[index] == '\t') {
            index++;
        }
    }

    return argc;
}

int parser_parse_integer(const char* text, int fallback) {
    int value = 0;
    int index = 0;
    int has_digit = 0;

    if (text == 0) {
        return fallback;
    }

    while (text[index] == ' ' || text[index] == '\t') {
        index++;
    }

    while (text[index] >= '0' && text[index] <= '9') {
        has_digit = 1;
        value = (value * 10) + (text[index] - '0');
        index++;
    }

    if (!has_digit || value <= 0) {
        return fallback;
    }

    return value;
}
