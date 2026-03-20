#ifndef PARSER_H
#define PARSER_H

int parser_parse_line(const char* line, char tokens[][64], const char* argv[], int max_args, int token_max);
int parser_parse_integer(const char* text, int fallback);

#endif
