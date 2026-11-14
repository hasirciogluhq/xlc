// A smoke test of the xlang lexer through the C boundary: tokenize a snippet, print the kinds and
// texts, and confirm the error path. Built by hand while the xlang lexer settles in:
//   xl build src/lang/lexer/lexer.xl --emit=obj -o /tmp/lexer_xl.o
//   cc test/lex_smoke.c /tmp/lexer_xl.o -Iinclude -o /tmp/lex_smoke && /tmp/lex_smoke
#include "xlang/lang/lexer_c.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    const char* src = "fn f(a: u8) -> *i32 { return b'x' }\nlocal n = 0x1F\n";
    XlcLexResult r = xlcLex(src, (uint32_t)strlen(src));
    printf("tokens: %u errors: %u\n", r.token_count, r.error_count);
    for (uint32_t i = 0; i < r.token_count; ++i) {
        const XlcLexToken* t = &r.tokens[i];
        printf("  kind=%3d line=%u col=%u nl=%d text=\"%.*s\" int=%llu\n", t->kind, t->line, t->column,
               t->newline_before, (int)t->text_len, t->text ? t->text : "", (unsigned long long)t->int_value);
    }
    xlcLexFree(r);
    return 0;
}
