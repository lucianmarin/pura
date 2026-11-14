/*
 * Pura — Pythonic C/C++ Bidirectional Transpiler (single-file C11 implementation).
 *
 * Pura is to C/C++ what LMON is to JSON:
 *   - Strict, lossless 1-to-1 structural equivalence with C and C++
 *   - Pythonic indentation (`:` blocks instead of `{` `}`, no trailing `;` noise)
 *   - Pythonic declarations (`def fn(a: i32) -> i32:`, `x: i32 = 10`)
 *   - Pythonic keywords & operators (`elif`, `and`, `or`, `not`, `nil`, `pass`)
 *   - Bracket-free `import`: extensionless `import module` for `.pu` modules, `import stdio.h` for `.h`/`.c` files
 *   - Concise fixed-width & C/C++ type aliases (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `usize`, `isize`, `str`, `cstr`, `byte`)
 *   - Auto-detected indentation (any consistent spaces or tabs) & universal newlines (\n, \r\n, \r)
 *   - Embeddable as a library (#define PURA_NO_MAIN before including/compiling)
 *
 * Build & CLI usage:
 *   cc -O3 -std=c11 -Wall -Wextra pura.c -o pura
 *   ./pura examples/hello.pu
 *   ./pura --pura [--indent <N|tab>] examples/hello.pu
 *   ./pura --from-c examples/hello.c
 *   ./pura --compile examples/hello.pu -o hello
 *   ./pura --run examples/hello.pu
 *   ./pura --check examples/hello.pu
 *   ./pura --test
 *   ./pura --bench [file.pu]
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Public Types & Dynamic Buffer
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *message;
    size_t line;
    size_t col;
} PuraError;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} PuraBuf;

static bool pura_buf_init(PuraBuf *b, size_t initial_cap) {
    if (initial_cap < 64) initial_cap = 64;
    b->data = (char *)malloc(initial_cap);
    if (!b->data) {
        b->len = 0;
        b->cap = 0;
        return false;
    }
    b->data[0] = '\0';
    b->len = 0;
    b->cap = initial_cap;
    return true;
}

static void pura_buf_free(PuraBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static bool pura_buf_reserve(PuraBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char *nd = (char *)realloc(b->data, ncap);
    if (!nd) return false;
    b->data = nd;
    b->cap = ncap;
    return true;
}

static void pura_buf_append(PuraBuf *b, const char *s, size_t n) {
    if (n == 0) return;
    if (!pura_buf_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void pura_buf_puts(PuraBuf *b, const char *s) {
    pura_buf_append(b, s, strlen(s));
}

static void pura_buf_putc(PuraBuf *b, char c) {
    if (!pura_buf_reserve(b, 1)) return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void pura_buf_indent(PuraBuf *b, const char *unit, size_t unit_len, size_t level) {
    for (size_t i = 0; i < level; i++) {
        pura_buf_append(b, unit, unit_len);
    }
}

/* -------------------------------------------------------------------------
 * Character & Token Helpers
 * ------------------------------------------------------------------------- */

static inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool is_ident_cont(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

typedef struct {
    const char *pura_type;
    const char *c_type;
} TypeMapping;

static const TypeMapping TYPE_MAP[] = {
    {"i8",    "int8_t"},
    {"i16",   "int16_t"},
    {"i32",   "int32_t"},
    {"i64",   "int64_t"},
    {"u8",    "uint8_t"},
    {"u16",   "uint16_t"},
    {"u32",   "uint32_t"},
    {"u64",   "uint64_t"},
    {"usize", "size_t"},
    {"isize", "ptrdiff_t"},
    {"f32",   "float"},
    {"f64",   "double"},
    {"byte",  "uint8_t"},
    {"cstr",  "const char*"},
    {"str",   "char*"},
    {"any",   "void*"}
};

static const size_t TYPE_MAP_COUNT = sizeof(TYPE_MAP) / sizeof(TYPE_MAP[0]);

/* -------------------------------------------------------------------------
 * Scoped Type & Pointer Symbol Tracking (for `addr[T]`, unified `.`, auto struct/enum)
 * ------------------------------------------------------------------------- */

typedef struct {
    char name[64];
    char kind[8]; /* "struct", "union", or "enum" */
} TagEntry;

typedef struct {
    char name[64];
    size_t scope_level;
} PtrVarEntry;

typedef struct {
    TagEntry tags[128];
    size_t num_tags;
    PtrVarEntry ptrs[256];
    size_t num_ptrs;
} PuraSymTab;

static PuraSymTab g_sym;

static void symtab_reset(void) {
    g_sym.num_tags = 0;
    g_sym.num_ptrs = 0;
}

static void symtab_add_tag(const char *kind, const char *name, size_t nlen) {
    if (nlen == 0 || nlen >= sizeof(g_sym.tags[0].name)) return;
    for (size_t i = 0; i < g_sym.num_tags; i++) {
        if (strlen(g_sym.tags[i].name) == nlen && memcmp(g_sym.tags[i].name, name, nlen) == 0) {
            return;
        }
    }
    if (g_sym.num_tags < 128) {
        memcpy(g_sym.tags[g_sym.num_tags].name, name, nlen);
        g_sym.tags[g_sym.num_tags].name[nlen] = '\0';
        snprintf(g_sym.tags[g_sym.num_tags].kind, sizeof(g_sym.tags[0].kind), "%s", kind);
        g_sym.num_tags++;
    }
}

static const char *symtab_find_tag_kind(const char *name, size_t nlen) {
    for (size_t i = 0; i < g_sym.num_tags; i++) {
        if (strlen(g_sym.tags[i].name) == nlen && memcmp(g_sym.tags[i].name, name, nlen) == 0) {
            return g_sym.tags[i].kind;
        }
    }
    return NULL;
}

static void symtab_add_ptr_var(const char *name, size_t nlen, size_t level) {
    while (nlen > 0 && (name[0] == ' ' || name[0] == '*')) { name++; nlen--; }
    while (nlen > 0 && name[nlen - 1] == ' ') nlen--;
    if (nlen == 0 || nlen >= sizeof(g_sym.ptrs[0].name)) return;
    for (size_t i = 0; i < g_sym.num_ptrs; i++) {
        if (g_sym.ptrs[i].scope_level == level &&
            strlen(g_sym.ptrs[i].name) == nlen &&
            memcmp(g_sym.ptrs[i].name, name, nlen) == 0) {
            return;
        }
    }
    if (g_sym.num_ptrs < 256) {
        memcpy(g_sym.ptrs[g_sym.num_ptrs].name, name, nlen);
        g_sym.ptrs[g_sym.num_ptrs].name[nlen] = '\0';
        g_sym.ptrs[g_sym.num_ptrs].scope_level = level;
        g_sym.num_ptrs++;
    }
}

static void symtab_pop_scope(size_t min_keep_level) {
    while (g_sym.num_ptrs > 0 && g_sym.ptrs[g_sym.num_ptrs - 1].scope_level > min_keep_level) {
        g_sym.num_ptrs--;
    }
}

static bool symtab_is_ptr_var(const char *name, size_t nlen) {
    if (nlen == 4 && memcmp(name, "self", 4) == 0) return true;
    for (size_t i = g_sym.num_ptrs; i > 0; i--) {
        if (strlen(g_sym.ptrs[i - 1].name) == nlen &&
            memcmp(g_sym.ptrs[i - 1].name, name, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_pura_ptr_type(const char *type_s, size_t type_len) {
    while (type_len > 0 && type_s[0] == ' ') { type_s++; type_len--; }
    while (type_len > 0 && type_s[type_len - 1] == ' ') type_len--;
    if (type_len >= 6 && memcmp(type_s, "addr[", 5) == 0 && type_s[type_len - 1] == ']') return true;
    if (type_len > 0 && type_s[type_len - 1] == '*') return true;
    return false;
}

static const char *map_pura_word_to_c(const char *w, size_t len) {
    for (size_t i = 0; i < TYPE_MAP_COUNT; i++) {
        if (strlen(TYPE_MAP[i].pura_type) == len &&
            memcmp(w, TYPE_MAP[i].pura_type, len) == 0) {
            return TYPE_MAP[i].c_type;
        }
    }
    if (len == 3 && memcmp(w, "and", 3) == 0) return "&&";
    if (len == 2 && memcmp(w, "or", 2) == 0) return "||";
    if (len == 3 && memcmp(w, "not", 3) == 0) return "!";
    if (len == 3 && memcmp(w, "nil", 3) == 0) return "NULL";
    if (len == 4 && memcmp(w, "None", 4) == 0) return "NULL";
    if (len == 4 && memcmp(w, "True", 4) == 0) return "true";
    if (len == 5 && memcmp(w, "False", 5) == 0) return "false";
    if (len == 4 && memcmp(w, "self", 4) == 0) return "this";
    return NULL;
}

static const char *map_c_word_to_pura(const char *w, size_t len) {
    for (size_t i = 0; i < TYPE_MAP_COUNT; i++) {
        /* Skip duplicate uint8_t -> byte, prefer u8 */
        if (strcmp(TYPE_MAP[i].pura_type, "byte") == 0) continue;
        if (strlen(TYPE_MAP[i].c_type) == len &&
            memcmp(w, TYPE_MAP[i].c_type, len) == 0) {
            return TYPE_MAP[i].pura_type;
        }
    }
    if (len == 4 && memcmp(w, "NULL", 4) == 0) return "nil";
    if (len == 7 && memcmp(w, "nullptr", 7) == 0) return "nil";
    if (len == 4 && memcmp(w, "this", 4) == 0) return "self";
    return NULL;
}

static void translate_tokens_pura_to_c(PuraBuf *out, const char *s, size_t len);

/* Translate a Pura type specification (supporting `addr[T]` and automatic `struct`/`enum`/`union` prefix) to C */
static void translate_type_pura_to_c(PuraBuf *out, const char *s, size_t len) {
    while (len > 0 && s[0] == ' ') { s++; len--; }
    while (len > 0 && s[len - 1] == ' ') len--;
    if (len == 0) return;

    /* `addr[T]` -> `<C type of T>*` */
    if (len >= 6 && memcmp(s, "addr[", 5) == 0 && s[len - 1] == ']') {
        translate_type_pura_to_c(out, s + 5, len - 6);
        pura_buf_putc(out, '*');
        return;
    }

    /* Check leading `const ` */
    if (len > 6 && memcmp(s, "const ", 6) == 0) {
        pura_buf_puts(out, "const ");
        translate_type_pura_to_c(out, s + 6, len - 6);
        return;
    }

    /* Ifalready prefixed with `struct `, `enum `, `union `, translate tokens directly */
    if ((len > 7 && memcmp(s, "struct ", 7) == 0) ||
        (len > 5 && memcmp(s, "enum ", 5) == 0) ||
        (len > 6 && memcmp(s, "union ", 6) == 0)) {
        translate_tokens_pura_to_c(out, s, len);
        return;
    }

    /* Check if base identifier is a registered struct/enum/union tag */
    size_t id_len = 0;
    while (id_len < len && is_ident_cont(s[id_len])) id_len++;
    if (id_len > 0) {
        const char *tag_kind = symtab_find_tag_kind(s, id_len);
        if (tag_kind) {
            pura_buf_puts(out, tag_kind);
            pura_buf_putc(out, ' ');
        }
    }
    translate_tokens_pura_to_c(out, s, len);
}

/* Find matching closing `)` for `(` at `open_idx` */
static size_t find_matching_paren(const char *s, size_t len, size_t open_idx) {
    int depth = 0;
    char quote = 0;
    for (size_t i = open_idx; i < len; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') depth++;
        else if (c == ')') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return len;
}

static bool is_pura_type_word(const char *w, size_t len) {
    for (size_t i = 0; i < TYPE_MAP_COUNT; i++) {
        if (strlen(TYPE_MAP[i].pura_type) == len &&
            memcmp(w, TYPE_MAP[i].pura_type, len) == 0) {
            return true;
        }
    }
    if (len == 4 && memcmp(w, "char", 4) == 0) return true;
    if (len == 3 && memcmp(w, "int", 3) == 0) return true;
    if (len == 4 && memcmp(w, "long", 4) == 0) return true;
    if (len == 5 && memcmp(w, "short", 5) == 0) return true;
    if (len == 5 && memcmp(w, "float", 5) == 0) return true;
    if (len == 6 && memcmp(w, "double", 6) == 0) return true;
    if (len == 4 && memcmp(w, "void", 4) == 0) return true;
    if (symtab_find_tag_kind(w, len) != NULL) return true;
    return false;
}

static bool is_c_cast_type_slice(const char *s, size_t len) {
    while (len > 0 && s[0] == ' ') { s++; len--; }
    while (len > 0 && s[len - 1] == ' ') len--;
    if (len == 0) return false;
    while (len > 0 && s[len - 1] == '*') {
        len--;
        while (len > 0 && s[len - 1] == ' ') len--;
    }
    if (len > 6 && memcmp(s, "const ", 6) == 0) { s += 6; len -= 6; }
    if (len > 7 && memcmp(s, "struct ", 7) == 0) { s += 7; len -= 7; }
    else if (len > 5 && memcmp(s, "enum ", 5) == 0) { s += 5; len -= 5; }
    else if (len > 6 && memcmp(s, "union ", 6) == 0) { s += 6; len -= 6; }
    while (len > 0 && s[0] == ' ') { s++; len--; }
    if (len == 0 || !is_ident_start(s[0])) return false;
    for (size_t i = 0; i < len; i++) {
        if (!is_ident_cont(s[i])) return false;
    }
    if (map_c_word_to_pura(s, len) != NULL) return true;
    if (symtab_find_tag_kind(s, len) != NULL) return true;
    if (len == 4 && (memcmp(s, "char", 4) == 0 || memcmp(s, "void", 4) == 0 ||
                     memcmp(s, "long", 4) == 0 || memcmp(s, "bool", 4) == 0)) return true;
    if (len == 3 && memcmp(s, "int", 3) == 0) return true;
    if (len == 5 && (memcmp(s, "float", 5) == 0 || memcmp(s, "short", 5) == 0)) return true;
    if (len == 6 && (memcmp(s, "double", 6) == 0 || memcmp(s, "size_t", 6) == 0)) return true;
    return false;
}

static void emit_cast_pura_to_c(PuraBuf *out, const char *type_s, size_t type_len,
                                  const char *inner, size_t inner_len) {
    pura_buf_putc(out, '(');
    translate_type_pura_to_c(out, type_s, type_len);
    pura_buf_putc(out, ')');
    bool simple_postfix = true;
    int pdepth = 0;
    for (size_t k = 0; k < inner_len; k++) {
        char ic = inner[k];
        if (ic == '[' || ic == '(') pdepth++;
        else if (ic == ']' || ic == ')') pdepth--;
        else if (pdepth == 0 && (ic == ' ' || ic == '+' || ic == '-' || ic == '*' || ic == '/' ||
                                 ic == '%' || ic == '&' || ic == '|' || ic == '^' || ic == '<' || ic == '>')) {
            if ((ic == '+' || ic == '-') && k + 2 == inner_len && inner[k + 1] == ic) continue;
            simple_postfix = false;
            break;
        }
    }
    if (simple_postfix) {
        translate_tokens_pura_to_c(out, inner, inner_len);
    } else {
        pura_buf_putc(out, '(');
        translate_tokens_pura_to_c(out, inner, inner_len);
        pura_buf_putc(out, ')');
    }
}

/* Translate expression/type tokens from Pura to C, respecting string/char literals */
static void translate_tokens_pura_to_c(PuraBuf *out, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char quote = c;
            pura_buf_putc(out, c);
            i++;
            while (i < len) {
                char sc = s[i++];
                pura_buf_putc(out, sc);
                if (sc == '\\' && i < len) {
                    pura_buf_putc(out, s[i++]);
                } else if (sc == quote) {
                    break;
                }
            }
            continue;
        }
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < len && is_ident_cont(s[i])) i++;
            size_t wlen = i - start;
            /* Do not replace member names after '.' or '->' */
            bool is_member = false;
            if (start > 0 && s[start - 1] == '.') is_member = true;
            if (start > 1 && s[start - 2] == '-' && s[start - 1] == '>') is_member = true;

            /* `addr[T]` inside expressions (e.g. `sizeof(addr[cal_month])`) */
            if (!is_member && wlen == 4 && memcmp(s + start, "addr", 4) == 0 &&
                i < len && s[i] == '[') {
                int bdepth = 0;
                size_t close_b = i;
                while (close_b < len) {
                    if (s[close_b] == '[') bdepth++;
                    else if (s[close_b] == ']') {
                        bdepth--;
                        if (bdepth == 0) break;
                    }
                    close_b++;
                }
                if (close_b < len) {
                    translate_type_pura_to_c(out, s + start, close_b - start + 1);
                    i = close_b + 1;
                    continue;
                }
            }

            /* Pythonic `cast(Type, expr)` (from `typing.cast`): pointer reinterpretation & value casts */
            if (!is_member && wlen == 4 && memcmp(s + start, "cast", 4) == 0 &&
                i < len && s[i] == '(') {
                size_t close_p = find_matching_paren(s, len, i);
                if (close_p < len) {
                    const char *args = s + i + 1;
                    size_t args_len = close_p - (i + 1);
                    /* Find top-level comma separating `Type` and `expr` */
                    size_t comma = args_len;
                    int depth = 0;
                    for (size_t k = 0; k < args_len; k++) {
                        if (args[k] == '[' || args[k] == '(') depth++;
                        else if (args[k] == ']' || args[k] == ')') depth--;
                        else if (args[k] == ',' && depth == 0) {
                            comma = k;
                            break;
                        }
                    }
                    if (comma < args_len) {
                        const char *type_s = args;
                        size_t type_len = comma;
                        while (type_len > 0 && type_s[0] == ' ') { type_s++; type_len--; }
                        while (type_len > 0 && type_s[type_len - 1] == ' ') type_len--;

                        const char *val_s = args + comma + 1;
                        size_t val_len = args_len - (comma + 1);
                        while (val_len > 0 && val_s[0] == ' ') { val_s++; val_len--; }
                        while (val_len > 0 && val_s[val_len - 1] == ' ') val_len--;

                        emit_cast_pura_to_c(out, type_s, type_len, val_s, val_len);
                        i = close_p + 1;
                        continue;
                    }
                }
            }

            /* Pythonic scalar value conversion: `u8(x)`, `i32(x)`, `usize(x)`, `f64(x)` -> `(uint8_t)x` */
            if (!is_member && i < len && s[i] == '(' && is_pura_type_word(s + start, wlen)) {
                size_t close_p = find_matching_paren(s, len, i);
                if (close_p < len) {
                    emit_cast_pura_to_c(out, s + start, wlen, s + i + 1, close_p - (i + 1));
                    i = close_p + 1;
                    continue;
                }
            }

            /* `val(p)` -> `*p` and `val(level, p)` -> `**...*p` */
            if (!is_member && wlen == 3 && memcmp(s + start, "val", 3) == 0 &&
                i < len && s[i] == '(') {
                size_t close_p = find_matching_paren(s, len, i);
                if (close_p < len) {
                    const char *inner = s + i + 1;
                    size_t inner_len = close_p - (i + 1);
                    size_t comma = inner_len;
                    int cdepth = 0;
                    char cquote = 0;
                    for (size_t k = 0; k < inner_len; k++) {
                        char ic = inner[k];
                        if (cquote) {
                            if (ic == '\\' && k + 1 < inner_len) k++;
                            else if (ic == cquote) cquote = 0;
                        } else {
                            if (ic == '"' || ic == '\'') cquote = ic;
                            else if (ic == '(' || ic == '[' || ic == '<') cdepth++;
                            else if (ic == ')' || ic == ']' || ic == '>') cdepth--;
                            else if (ic == ',' && cdepth == 0) {
                                comma = k;
                                break;
                            }
                        }
                    }

                    const char *expr_s = inner;
                    size_t expr_len = inner_len;
                    size_t deref_level = 1;
                    if (comma < inner_len) {
                        const char *lvl_s = inner;
                        size_t lvl_len = comma;
                        while (lvl_len > 0 && lvl_s[0] == ' ') { lvl_s++; lvl_len--; }
                        while (lvl_len > 0 && lvl_s[lvl_len - 1] == ' ') lvl_len--;

                        expr_s = inner + comma + 1;
                        expr_len = inner_len - (comma + 1);
                        while (expr_len > 0 && expr_s[0] == ' ') { expr_s++; expr_len--; }
                        while (expr_len > 0 && expr_s[expr_len - 1] == ' ') expr_len--;

                        if (lvl_len > 0) {
                            size_t parsed = 0;
                            bool all_digits = true;
                            for (size_t k = 0; k < lvl_len; k++) {
                                if (lvl_s[k] >= '0' && lvl_s[k] <= '9') {
                                    parsed = parsed * 10 + (size_t)(lvl_s[k] - '0');
                                } else {
                                    all_digits = false;
                                    break;
                                }
                            }
                            if (all_digits && parsed > 0 && parsed <= 16) {
                                deref_level = parsed;
                            }
                        }
                    }

                    for (size_t d = 0; d < deref_level; d++) {
                        pura_buf_putc(out, '*');
                    }

                    bool simple_postfix = true;
                    int pdepth = 0;
                    for (size_t k = 0; k < expr_len; k++) {
                        char ic = expr_s[k];
                        if (ic == '[' || ic == '(') pdepth++;
                        else if (ic == ']' || ic == ')') pdepth--;
                        else if (pdepth == 0 && (ic == ' ' || ic == '+' || ic == '-' || ic == '*' || ic == '/')) {
                            if ((ic == '+' || ic == '-') && k + 2 == expr_len && expr_s[k + 1] == ic) {
                                continue;
                            }
                            simple_postfix = false;
                            break;
                        }
                    }
                    if (simple_postfix) {
                        translate_tokens_pura_to_c(out, expr_s, expr_len);
                    } else {
                        pura_buf_putc(out, '(');
                        translate_tokens_pura_to_c(out, expr_s, expr_len);
                        pura_buf_putc(out, ')');
                    }
                    i = close_p + 1;
                    continue;
                }
            }

            /* Check if primary/postfix chain starting at `start` ends with `.addr` (`var.addr` -> `&var`, `ms[i + 1].addr` -> `&ms[i + 1]`) */
            if (!is_member) {
                size_t scan = i;
                while (scan < len) {
                    if (s[scan] == '[') {
                        int bd = 0;
                        while (scan < len) {
                            if (s[scan] == '[') bd++;
                            else if (s[scan] == ']') { bd--; if (bd == 0) { scan++; break; } }
                            scan++;
                        }
                    } else if (scan + 5 <= len && memcmp(s + scan, ".addr", 5) == 0 &&
                               (scan + 5 == len || !is_ident_cont(s[scan + 5]))) {
                        pura_buf_putc(out, '&');
                        translate_tokens_pura_to_c(out, s + start, scan - start);
                        i = scan + 5;
                        goto next_pura_tok;
                    } else if (s[scan] == '.' && scan + 1 < len && is_ident_start(s[scan + 1])) {
                        scan++;
                        while (scan < len && is_ident_cont(s[scan])) scan++;
                    } else {
                        break;
                    }
                }
            }

            const char *rep = is_member ? NULL : map_pura_word_to_c(s + start, wlen);
            if (rep) {
                if (strcmp(rep, "!") == 0 && i < len && s[i] == ' ') {
                    pura_buf_putc(out, '!');
                    i++; /* consume space after `not ` so `not x` -> `!x` */
                } else {
                    pura_buf_puts(out, rep);
                }
            } else {
                /* If identifier is a registered struct/enum/union tag inside `sizeof(Name)` or `(Name)` cast */
                const char *tag_kind = is_member ? NULL : symtab_find_tag_kind(s + start, wlen);
                if (tag_kind && start > 0 && s[start - 1] == '(' &&
                    i < len && (s[i] == ')' || s[i] == '*')) {
                    pura_buf_puts(out, tag_kind);
                    pura_buf_putc(out, ' ');
                }
                pura_buf_append(out, s + start, wlen);
            }

            /* Unified `.` member access: if this identifier is a known pointer variable (`ctl`, `month`, `self`, etc.)
               and is immediately followed by `.` + identifier, emit `->` instead of `.`! */
            if (!is_member && i + 1 < len && s[i] == '.' && is_ident_start(s[i + 1]) &&
                symtab_is_ptr_var(s + start, wlen)) {
                pura_buf_puts(out, "->");
                i++; /* skip '.' */
            }
next_pura_tok:
            continue;
        }
        pura_buf_putc(out, c);
        i++;
    }
}

static void translate_tokens_c_to_pura(PuraBuf *out, const char *s, size_t len);

/* Translate a C type specification into Pura (`T*` -> `addr[T]`, dropping redundant `struct`/`enum`/`union` for known tags) */
static void translate_type_c_to_pura(PuraBuf *out, const char *s, size_t len) {
    while (len > 0 && s[0] == ' ') { s++; len--; }
    while (len > 0 && s[len - 1] == ' ') len--;
    if (len == 0) return;

    /* Special primitive pointer aliases first (`const char*` -> `cstr`, `char*` -> `str`, `void*` -> `any`) */
    if ((len == 11 && memcmp(s, "const char*", 11) == 0) ||
        (len == 12 && memcmp(s, "const char *", 12) == 0)) {
        pura_buf_puts(out, "cstr");
        return;
    }
    if ((len == 5 && memcmp(s, "char*", 5) == 0) ||
        (len == 6 && memcmp(s, "char *", 6) == 0)) {
        pura_buf_puts(out, "str");
        return;
    }
    if ((len == 5 && memcmp(s, "void*", 5) == 0) ||
        (len == 6 && memcmp(s, "void *", 6) == 0)) {
        pura_buf_puts(out, "any");
        return;
    }

    /* Trailing `*` -> `addr[...]` */
    if (len > 1 && s[len - 1] == '*') {
        pura_buf_puts(out, "addr[");
        translate_type_c_to_pura(out, s, len - 1);
        pura_buf_putc(out, ']');
        return;
    }

    /* Leading `const ` */
    if (len > 6 && memcmp(s, "const ", 6) == 0) {
        pura_buf_puts(out, "const ");
        translate_type_c_to_pura(out, s + 6, len - 6);
        return;
    }

    /* Strip leading `struct `, `enum `, `union ` if the tag was declared in this module */
    if (len > 7 && memcmp(s, "struct ", 7) == 0) {
        const char *tag = s + 7;
        size_t tlen = len - 7;
        if (symtab_find_tag_kind(tag, tlen)) {
            s = tag;
            len = tlen;
        }
    } else if (len > 5 && memcmp(s, "enum ", 5) == 0) {
        const char *tag = s + 5;
        size_t tlen = len - 5;
        if (symtab_find_tag_kind(tag, tlen)) {
            s = tag;
            len = tlen;
        }
    } else if (len > 6 && memcmp(s, "union ", 6) == 0) {
        const char *tag = s + 6;
        size_t tlen = len - 6;
        if (symtab_find_tag_kind(tag, tlen)) {
            s = tag;
            len = tlen;
        }
    }

    translate_tokens_c_to_pura(out, s, len);
}

/* Helper: check if C character before unary `&` or `*` indicates a unary prefix position */
static bool is_c_unary_prefix_pos(const char *s, size_t idx) {
    while (idx > 0 && s[idx - 1] == ' ') idx--;
    if (idx == 0) return true;
    char prev = s[idx - 1];
    return prev == '(' || prev == '[' || prev == ',' || prev == '=' ||
           prev == ':' || prev == ';' || prev == '!' || prev == '?' ||
           prev == '+' || prev == '-' || prev == '*' || prev == '/' ||
           prev == '%' || prev == '&' || prev == '|' || prev == '^' ||
           prev == '<' || prev == '>';
}

/* Scan unary operand extent starting at `start` (e.g. `x`, `a.b`, `ms[i + 1]`, `*argv++`) */
static size_t scan_c_unary_operand(const char *s, size_t len, size_t start) {
    size_t i = start;
    if (i < len && (s[i] == '*' || s[i] == '&')) {
        return scan_c_unary_operand(s, len, i + 1);
    }
    if (i < len && s[i] == '(') {
        size_t close_p = find_matching_paren(s, len, i);
        if (close_p < len) return close_p + 1;
    }
    while (i < len) {
        if (is_ident_cont(s[i]) || s[i] == '.') {
            i++;
        } else if (i + 1 < len && s[i] == '-' && s[i + 1] == '>') {
            i += 2;
        } else if (i + 1 < len && ((s[i] == '+' && s[i + 1] == '+') || (s[i] == '-' && s[i + 1] == '-'))) {
            i += 2;
        } else if (s[i] == '(') {
            size_t close_p = find_matching_paren(s, len, i);
            if (close_p < len) i = close_p + 1;
            else break;
        } else if (s[i] == '[') {
            int d = 0;
            while (i < len) {
                if (s[i] == '[') d++;
                else if (s[i] == ']') { d--; if (d == 0) { i++; break; } }
                i++;
            }
        } else {
            break;
        }
    }
    return i;
}

/* Translate expression/type tokens from C to Pura, respecting string/char literals */
static void translate_tokens_c_to_pura(PuraBuf *out, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char quote = c;
            pura_buf_putc(out, c);
            i++;
            while (i < len) {
                char sc = s[i++];
                pura_buf_putc(out, sc);
                if (sc == '\\' && i < len) {
                    pura_buf_putc(out, s[i++]);
                } else if (sc == quote) {
                    break;
                }
            }
            continue;
        }
        /* Multi-word C types: `const char*` or `const char *` -> `cstr` */
        if (len - i >= 11 && memcmp(s + i, "const char*", 11) == 0 &&
            (i == 0 || !is_ident_cont(s[i - 1]))) {
            pura_buf_puts(out, "cstr");
            i += 11;
            if (i < len && s[i] == ' ') i++;
            continue;
        }
        if (len - i >= 12 && memcmp(s + i, "const char *", 12) == 0 &&
            (i == 0 || !is_ident_cont(s[i - 1]))) {
            pura_buf_puts(out, "cstr");
            i += 12;
            continue;
        }
        if (len - i >= 5 && memcmp(s + i, "char*", 5) == 0 &&
            (i == 0 || !is_ident_cont(s[i - 1]))) {
            pura_buf_puts(out, "str");
            i += 5;
            continue;
        }
        if (len - i >= 6 && memcmp(s + i, "char *", 6) == 0 &&
            (i == 0 || !is_ident_cont(s[i - 1]))) {
            pura_buf_puts(out, "str ");
            i += 6;
            continue;
        }
        if (len - i >= 5 && memcmp(s + i, "void*", 5) == 0 &&
            (i == 0 || !is_ident_cont(s[i - 1]))) {
            pura_buf_puts(out, "any");
            i += 5;
            continue;
        }
        if (len - i >= 6 && memcmp(s + i, "void *", 6) == 0 &&
            (i == 0 || !is_ident_cont(s[i - 1]))) {
            pura_buf_puts(out, "any ");
            i += 6;
            continue;
        }
        /* Unified `.` member access in Pura: `ptr->member` -> `ptr.member` */
        if (c == '-' && i + 1 < len && s[i + 1] == '>' &&
            i + 2 < len && is_ident_start(s[i + 2])) {
            pura_buf_putc(out, '.');
            i += 2;
            continue;
        }
        /* Logical operators: && -> and, || -> or, !x -> not x */
        if (c == '&' && i + 1 < len && s[i + 1] == '&') {
            if (out->len > 0 && out->data[out->len - 1] != ' ') pura_buf_putc(out, ' ');
            pura_buf_puts(out, "and");
            i += 2;
            if (i < len && s[i] != ' ') pura_buf_putc(out, ' ');
            continue;
        }
        /* C prefix cast `(Type)expr` -> `cast(addr[T], expr)` for pointers or `Type(expr)` for scalars */
        if (c == '(' && is_c_unary_prefix_pos(s, i)) {
            size_t close_t = find_matching_paren(s, len, i);
            if (close_t < len && close_t > i + 1 && is_c_cast_type_slice(s + i + 1, close_t - (i + 1))) {
                size_t op_start = close_t + 1;
                while (op_start < len && s[op_start] == ' ') op_start++;
                size_t op_end = scan_c_unary_operand(s, len, op_start);
                if (op_end > op_start) {
                    const char *type_s = s + i + 1;
                    size_t type_len = close_t - (i + 1);
                    while (type_len > 0 && type_s[type_len - 1] == ' ') type_len--;

                    const char *arg_s = s + op_start;
                    size_t arg_len = op_end - op_start;
                    if (arg_len >= 2 && arg_s[0] == '(' && find_matching_paren(arg_s, arg_len, 0) == arg_len - 1) {
                        arg_s++;
                        arg_len -= 2;
                    }

                    bool is_ptr_cast = (type_len > 0 && type_s[type_len - 1] == '*');
                    if (is_ptr_cast) {
                        pura_buf_puts(out, "cast(");
                        translate_type_c_to_pura(out, type_s, type_len);
                        pura_buf_puts(out, ", ");
                        translate_tokens_c_to_pura(out, arg_s, arg_len);
                        pura_buf_putc(out, ')');
                    } else {
                        translate_type_c_to_pura(out, type_s, type_len);
                        pura_buf_putc(out, '(');
                        translate_tokens_c_to_pura(out, arg_s, arg_len);
                        pura_buf_putc(out, ')');
                    }
                    i = op_end;
                    continue;
                }
            }
        }
        /* Unary address-of `&expr` -> `expr.addr` */
        if (c == '&' && (i + 1 < len && s[i + 1] != '&' && s[i + 1] != '=' && s[i + 1] != ' ') &&
            is_c_unary_prefix_pos(s, i)) {
            size_t op_end = scan_c_unary_operand(s, len, i + 1);
            if (op_end > i + 1) {
                translate_tokens_c_to_pura(out, s + i + 1, op_end - (i + 1));
                pura_buf_puts(out, ".addr");
                i = op_end;
                continue;
            }
        }
        /* Unary dereference `*expr` -> `val(expr)` and `**expr` -> `val(2, expr)` */
        if (c == '*' && is_c_unary_prefix_pos(s, i)) {
            size_t stars = 0;
            while (i + stars < len && s[i + stars] == '*') stars++;
            if (i + stars < len && s[i + stars] != '=' && s[i + stars] != ' ' && s[i + stars] != ')') {
                size_t op_end = scan_c_unary_operand(s, len, i + stars);
                if (op_end > i + stars) {
                    const char *op_s = s + i + stars;
                    size_t op_len = op_end - (i + stars);
                    if (op_len >= 2 && op_s[0] == '(' && find_matching_paren(op_s, op_len, 0) == op_len - 1) {
                        op_s++;
                        op_len -= 2;
                    }
                    if (stars > 1) {
                        char lvl_buf[24];
                        snprintf(lvl_buf, sizeof(lvl_buf), "val(%zu, ", stars);
                        pura_buf_puts(out, lvl_buf);
                    } else {
                        pura_buf_puts(out, "val(");
                    }
                    translate_tokens_c_to_pura(out, op_s, op_len);
                    pura_buf_putc(out, ')');
                    i = op_end;
                    continue;
                }
            }
        }
        if (c == '|' && i + 1 < len && s[i + 1] == '|') {
            if (out->len > 0 && out->data[out->len - 1] != ' ') pura_buf_putc(out, ' ');
            pura_buf_puts(out, "or");
            i += 2;
            if (i < len && s[i] != ' ') pura_buf_putc(out, ' ');
            continue;
        }
        if (c == '!' && (i + 1 >= len || s[i + 1] != '=')) {
            pura_buf_puts(out, "not ");
            i++;
            continue;
        }
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < len && is_ident_cont(s[i])) i++;
            size_t wlen = i - start;
            bool is_member = false;
            if (start > 0 && s[start - 1] == '.') is_member = true;
            if (start > 1 && s[start - 2] == '-' && s[start - 1] == '>') is_member = true;

            const char *rep = is_member ? NULL : map_c_word_to_pura(s + start, wlen);
            if (rep) {
                pura_buf_puts(out, rep);
            } else {
                pura_buf_append(out, s + start, wlen);
            }
            continue;
        }
        pura_buf_putc(out, c);
        i++;
    }
}

/* Format `name: type` (including array suffixes like `buf: char[64]`) into C `type name[64]` */
static void emit_c_typed_decl(PuraBuf *out, const char *name_s, size_t name_len,
                              const char *type_s, size_t type_len) {
    /* Trim whitespace */
    while (name_len > 0 && name_s[0] == ' ') { name_s++; name_len--; }
    while (name_len > 0 && name_s[name_len - 1] == ' ') name_len--;
    while (type_len > 0 && type_s[0] == ' ') { type_s++; type_len--; }
    while (type_len > 0 && type_s[type_len - 1] == ' ') type_len--;

    /* Check for array dimension on type, e.g. `i32[16]` or `char[64]`, but NOT `addr[T]`! */
    const char *bracket = NULL;
    int depth = 0;
    bool is_addr_type = (type_len >= 6 && memcmp(type_s, "addr[", 5) == 0);
    for (size_t i = is_addr_type ? 4 : 0; i < type_len; i++) {
        if (type_s[i] == '<' || type_s[i] == '(' || (is_addr_type && i == 4 && type_s[i] == '[')) depth++;
        else if (type_s[i] == '>' || type_s[i] == ')' || (is_addr_type && depth > 0 && type_s[i] == ']')) depth--;
        else if (type_s[i] == '[' && depth == 0) {
            bracket = type_s + i;
            break;
        }
    }

    size_t base_type_len = bracket ? (size_t)(bracket - type_s) : type_len;
    while (base_type_len > 0 && type_s[base_type_len - 1] == ' ') base_type_len--;

    translate_type_pura_to_c(out, type_s, base_type_len);
    pura_buf_putc(out, ' ');
    translate_tokens_pura_to_c(out, name_s, name_len);
    if (bracket) {
        size_t arr_len = type_len - (size_t)(bracket - type_s);
        translate_tokens_pura_to_c(out, bracket, arr_len);
    }
}

/* Convert a Pura parameter list `(a: i32, b: cstr = nil)` into C `(int32_t a, const char* b = NULL)` */
static void emit_c_param_list(PuraBuf *out, const char *params, size_t len, size_t param_scope_level) {
    while (len > 0 && params[0] == ' ') { params++; len--; }
    while (len > 0 && params[len - 1] == ' ') len--;
    if (len == 0) {
        pura_buf_puts(out, "void");
        return;
    }
    if (len == 4 && memcmp(params, "void", 4) == 0) {
        pura_buf_puts(out, "void");
        return;
    }

    size_t i = 0;
    bool first = true;
    while (i < len) {
        size_t pstart = i;
        int depth = 0;
        char quote = 0;
        while (i < len) {
            char c = params[i];
            if (quote) {
                if (c == '\\' && i + 1 < len) i++;
                else if (c == quote) quote = 0;
            } else {
                if (c == '"' || c == '\'') quote = c;
                else if (c == '(' || c == '<' || c == '[') depth++;
                else if (c == ')' || c == '>' || c == ']') depth--;
                else if (c == ',' && depth == 0) break;
            }
            i++;
        }
        size_t plen = i - pstart;
        if (i < len && params[i] == ',') i++;

        const char *p = params + pstart;
        while (plen > 0 && p[0] == ' ') { p++; plen--; }
        while (plen > 0 && p[plen - 1] == ' ') plen--;
        if (plen == 0) continue;

        if (!first) pura_buf_puts(out, ", ");
        first = false;

        if (plen == 3 && memcmp(p, "...", 3) == 0) {
            pura_buf_puts(out, "...");
            continue;
        }

        /* Find single ':' at top level (not '::') */
        const char *colon = NULL;
        depth = 0;
        quote = 0;
        for (size_t k = 0; k < plen; k++) {
            char c = p[k];
            if (quote) {
                if (c == '\\' && k + 1 < plen) k++;
                else if (c == quote) quote = 0;
            } else {
                if (c == '"' || c == '\'') quote = c;
                else if (c == '(' || c == '<' || c == '[') depth++;
                else if (c == ')' || c == '>' || c == ']') depth--;
                else if (c == ':' && depth == 0) {
                    if (k + 1 < plen && p[k + 1] == ':') {
                        k++; /* skip :: */
                    } else {
                        colon = p + k;
                        break;
                    }
                }
            }
        }

        if (colon) {
            size_t nlen = (size_t)(colon - p);
            const char *rest = colon + 1;
            size_t rlen = plen - (size_t)(rest - p);
            /* Optional default value `= val` */
            const char *eq = NULL;
            depth = 0;
            quote = 0;
            for (size_t k = 0; k < rlen; k++) {
                char c = rest[k];
                if (quote) {
                    if (c == '\\' && k + 1 < rlen) k++;
                    else if (c == quote) quote = 0;
                } else {
                    if (c == '"' || c == '\'') quote = c;
                    else if (c == '(' || c == '<' || c == '[') depth++;
                    else if (c == ')' || c == '>' || c == ']') depth--;
                    else if (c == '=' && depth == 0) {
                        eq = rest + k;
                        break;
                    }
                }
            }
            size_t actual_tlen = eq ? (size_t)(eq - rest) : rlen;
            if (is_pura_ptr_type(rest, actual_tlen)) {
                symtab_add_ptr_var(p, nlen, param_scope_level);
            }
            if (eq) {
                emit_c_typed_decl(out, p, nlen, rest, (size_t)(eq - rest));
                pura_buf_puts(out, " = ");
                const char *defv = eq + 1;
                size_t deflen = rlen - (size_t)(defv - rest);
                while (deflen > 0 && defv[0] == ' ') { defv++; deflen--; }
                translate_tokens_pura_to_c(out, defv, deflen);
            } else {
                emit_c_typed_decl(out, p, nlen, rest, rlen);
            }
        } else {
            translate_tokens_pura_to_c(out, p, plen);
        }
    }
}

/* -------------------------------------------------------------------------
 * Pura -> C/C++ Transpiler
 * ------------------------------------------------------------------------- */

typedef enum {
    BLOCK_NORMAL = 0,
    BLOCK_STRUCT_OR_CLASS,
    BLOCK_ENUM,
    BLOCK_SWITCH,
    BLOCK_CASE,
    BLOCK_DO_WHILE
} BlockKind;

typedef struct {
    size_t level;
    BlockKind kind;
} BlockFrame;

#define MAX_INDENT_STACK 128

static bool starts_with_kw(const char *s, size_t len, const char *kw, size_t *after_idx) {
    size_t klen = strlen(kw);
    if (len < klen || memcmp(s, kw, klen) != 0) return false;
    if (len == klen) {
        if (after_idx) *after_idx = klen;
        return true;
    }
    char next = s[klen];
    if (next == ' ' || next == '(' || next == ':') {
        size_t idx = klen;
        while (idx < len && s[idx] == ' ') idx++;
        if (after_idx) *after_idx = idx;
        return true;
    }
    return false;
}

/* Find top-level single `:` for variable declaration `x: type = val` or `x: type` */
static const char *find_decl_colon(const char *s, size_t len, const char **out_eq) {
    int depth = 0;
    char quote = 0;
    const char *colon = NULL;
    const char *eq = NULL;

    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if (depth == 0) {
            if (!colon && c == ':') {
                if (i + 1 < len && s[i + 1] == ':') {
                    i++; /* skip C++ scope :: */
                    continue;
                }
                colon = s + i;
            } else if (colon && !eq && c == '=') {
                if (i + 1 < len && s[i + 1] == '=') {
                    i++;
                    continue;
                }
                eq = s + i;
                break;
            } else if (!colon && (c == '=' || c == ';' || c == '+' || c == '-' || c == '*' || c == '/')) {
                return NULL;
            }
        }
    }
    if (!colon) return NULL;

    /* Validate left of colon is an optional qualifier (`const`, `static`, `extern`, `mut`) + identifier */
    size_t l_len = (size_t)(colon - s);
    while (l_len > 0 && s[l_len - 1] == ' ') l_len--;
    if (l_len == 0) return NULL;
    for (size_t i = 0; i < l_len; i++) {
        if (!is_ident_cont(s[i]) && s[i] != ' ' && s[i] != '*') return NULL;
    }
    if (out_eq) *out_eq = eq;
    return colon;
}

/* Strip inline `#` comment outside string/char literals, returning comment pointer or NULL */
static const char *split_inline_hash_comment(const char *s, size_t len, size_t *code_len) {
    char quote = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '#' && (i == 0 || s[i - 1] == ' ')) {
            size_t clen = i;
            while (clen > 0 && s[clen - 1] == ' ') clen--;
            *code_len = clen;
            return s + i;
        }
    }
    *code_len = len;
    return NULL;
}

/* Check if a root-level statement `NAME = val` or `NAME(args) = val` is a Pythonic `#define` macro */
static bool try_emit_pura_macro_define(PuraBuf *out, const char *s, size_t len) {
    if (len == 0 || !is_ident_start(s[0])) return false;

    size_t i = 1;
    while (i < len && is_ident_cont(s[i])) i++;
    size_t name_end = i;

    size_t sig_end = name_end;
    if (i < len && s[i] == '(') {
        /* Function-like macro `NAME(a, b) = expr` (no space between NAME and '(') */
        int depth = 0;
        while (i < len) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') {
                depth--;
                if (depth == 0) { i++; break; }
            }
            i++;
        }
        if (depth != 0) return false;
        sig_end = i;
    }

    while (i < len && s[i] == ' ') i++;
    if (i >= len || s[i] != '=') return false;
    if (i + 1 < len && s[i + 1] == '=') return false; /* `==` comparison */

    const char *rhs = s + i + 1;
    size_t rhs_len = len - (i + 1);
    while (rhs_len > 0 && rhs[0] == ' ') { rhs++; rhs_len--; }
    if (rhs_len == 0) return false;

    pura_buf_puts(out, "#define ");
    pura_buf_append(out, s, sig_end);
    pura_buf_putc(out, ' ');
    translate_tokens_pura_to_c(out, rhs, rhs_len);
    return true;
}

static bool emit_pura_function_def(PuraBuf *out, const char *prefix_qual,
                                     const char *rest, size_t rlen, bool is_block,
                                     size_t body_scope_level) {
    /* Expect `name(params) -> ret` or `name(params)` */
    const char *lparen = NULL;
    int depth = 0;
    for (size_t i = 0; i < rlen; i++) {
        if (rest[i] == '(') { lparen = rest + i; break; }
    }
    if (!lparen) return false;

    const char *rparen = NULL;
    char quote = 0;
    for (size_t i = (size_t)(lparen - rest); i < rlen; i++) {
        char c = rest[i];
        if (quote) {
            if (c == '\\' && i + 1 < rlen) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') depth++;
        else if (c == ')') {
            depth--;
            if (depth == 0) { rparen = rest + i; break; }
        }
    }
    if (!rparen) return false;

    size_t fname_len = (size_t)(lparen - rest);
    while (fname_len > 0 && rest[fname_len - 1] == ' ') fname_len--;

    const char *after = rparen + 1;
    size_t alen = rlen - (size_t)(after - rest);
    while (alen > 0 && after[0] == ' ') { after++; alen--; }

    const char *ret_str = "void";
    size_t ret_len = 4;
    const char *const_qual = "";

    /* Check for `const` method qualifier before `->` */
    if (alen >= 5 && memcmp(after, "const", 5) == 0 && (alen == 5 || after[5] == ' ' || after[5] == '-')) {
        const_qual = " const";
        after += 5;
        alen -= 5;
        while (alen > 0 && after[0] == ' ') { after++; alen--; }
    }

    if (alen >= 2 && after[0] == '-' && after[1] == '>') {
        after += 2;
        alen -= 2;
        while (alen > 0 && after[0] == ' ') { after++; alen--; }
        while (alen > 0 && after[alen - 1] == ' ') alen--;
        ret_str = after;
        ret_len = alen;
    } else if (fname_len == 4 && memcmp(rest, "main", 4) == 0) {
        ret_str = "int";
        ret_len = 3;
    } else if (alen == 0) {
        /* Constructor/destructor if name matches or starts with ~ */
        if (fname_len > 0 && (rest[0] == '~' || isupper((unsigned char)rest[0]))) {
            ret_str = "";
            ret_len = 0;
        }
    }

    if (prefix_qual && prefix_qual[0]) {
        pura_buf_puts(out, prefix_qual);
        pura_buf_putc(out, ' ');
    }
    if (ret_len > 0) {
        translate_type_pura_to_c(out, ret_str, ret_len);
        pura_buf_putc(out, ' ');
    }
    translate_tokens_pura_to_c(out, rest, fname_len);
    pura_buf_putc(out, '(');
    emit_c_param_list(out, lparen + 1, (size_t)(rparen - lparen - 1), body_scope_level);
    pura_buf_putc(out, ')');
    pura_buf_puts(out, const_qual);
    if (is_block) {
        pura_buf_puts(out, " {");
    } else {
        pura_buf_putc(out, ';');
    }
    return true;
}

/* Convert Pythonic `for i in range(stop):`, `for i in range(start, stop):`, `for i in range(start, stop, step):`,
   or `for x: T in range(...):` */
static bool try_emit_for_in(PuraBuf *out, const char *expr, size_t len) {
    /* Look for ` in ` at depth 0 */
    int depth = 0;
    const char *in_pos = NULL;
    for (size_t i = 0; i + 4 <= len; i++) {
        char c = expr[i];
        if (c == '(' || c == '[' || c == '<') depth++;
        else if (c == ')' || c == ']' || c == '>') depth--;
        else if (depth == 0 && memcmp(expr + i, " in ", 4) == 0) {
            in_pos = expr + i;
            break;
        }
    }
    if (!in_pos) return false;

    const char *var_part = expr;
    size_t var_len = (size_t)(in_pos - expr);
    while (var_len > 0 && var_part[0] == ' ') { var_part++; var_len--; }
    while (var_len > 0 && var_part[var_len - 1] == ' ') var_len--;

    const char *rhs = in_pos + 4;
    size_t rhs_len = len - (size_t)(rhs - expr);
    while (rhs_len > 0 && rhs[0] == ' ') { rhs++; rhs_len--; }
    while (rhs_len > 0 && rhs[rhs_len - 1] == ' ') rhs_len--;

    /* Check if var_part has `: type` */
    const char *vcolon = NULL;
    for (size_t i = 0; i < var_len; i++) {
        if (var_part[i] == ':') { vcolon = var_part + i; break; }
    }

    const char *vname = var_part;
    size_t vname_len = vcolon ? (size_t)(vcolon - var_part) : var_len;
    while (vname_len > 0 && vname[vname_len - 1] == ' ') vname_len--;

    const char *vtype = vcolon ? (vcolon + 1) : "i32";
    size_t vtype_len = vcolon ? (var_len - (size_t)(vtype - var_part)) : 3;
    while (vtype_len > 0 && vtype[0] == ' ') { vtype++; vtype_len--; }

    /* Pythonic `range(...)`: `range(stop)`, `range(start, stop)`, `range(start, stop, step)` */
    if (rhs_len >= 7 && memcmp(rhs, "range(", 6) == 0 &&
        find_matching_paren(rhs, rhs_len, 5) == rhs_len - 1) {
        const char *rargs = rhs + 6;
        size_t rargs_len = rhs_len - 7;

        /* Split top-level commas (up to 3 arguments) */
        const char *parts[3] = {NULL, NULL, NULL};
        size_t part_lens[3] = {0, 0, 0};
        size_t nparts = 0;
        size_t seg_start = 0;
        int d = 0;
        for (size_t i = 0; i <= rargs_len; i++) {
            if (i < rargs_len) {
                if (rargs[i] == '(' || rargs[i] == '[') d++;
                else if (rargs[i] == ')' || rargs[i] == ']') d--;
            }
            if ((i == rargs_len || (rargs[i] == ',' && d == 0)) && nparts < 3) {
                const char *ps = rargs + seg_start;
                size_t pl = i - seg_start;
                while (pl > 0 && ps[0] == ' ') { ps++; pl--; }
                while (pl > 0 && ps[pl - 1] == ' ') pl--;
                parts[nparts] = ps;
                part_lens[nparts] = pl;
                nparts++;
                seg_start = i + 1;
            }
        }

        if (nparts >= 1 && nparts <= 3 && part_lens[0] > 0) {
            const char *start_s = (nparts == 1) ? "0" : parts[0];
            size_t start_l = (nparts == 1) ? 1 : part_lens[0];
            const char *stop_s = (nparts == 1) ? parts[0] : parts[1];
            size_t stop_l = (nparts == 1) ? part_lens[0] : part_lens[1];
            const char *step_s = (nparts == 3) ? parts[2] : NULL;
            size_t step_l = (nparts == 3) ? part_lens[2] : 0;

            bool neg_step = (step_s && step_l > 0 && step_s[0] == '-');

            pura_buf_puts(out, "for (");
            emit_c_typed_decl(out, vname, vname_len, vtype, vtype_len);
            pura_buf_puts(out, " = ");
            translate_tokens_pura_to_c(out, start_s, start_l);
            pura_buf_puts(out, "; ");
            translate_tokens_pura_to_c(out, vname, vname_len);
            pura_buf_puts(out, neg_step ? " > " : " < ");
            translate_tokens_pura_to_c(out, stop_s, stop_l);
            pura_buf_puts(out, "; ");
            translate_tokens_pura_to_c(out, vname, vname_len);
            if (!step_s || (step_l == 1 && step_s[0] == '1')) {
                pura_buf_puts(out, "++");
            } else if (neg_step && step_l == 2 && step_s[1] == '1') {
                pura_buf_puts(out, "--");
            } else if (neg_step) {
                pura_buf_puts(out, " -= ");
                translate_tokens_pura_to_c(out, step_s + 1, step_l - 1);
            } else {
                pura_buf_puts(out, " += ");
                translate_tokens_pura_to_c(out, step_s, step_l);
            }
            pura_buf_puts(out, ") {");
            return true;
        }
    }

    /* Check if rhs has `..=` or `..` */
    const char *dots = NULL;
    bool inclusive = false;
    for (size_t i = 0; i + 2 <= rhs_len; i++) {
        if (rhs[i] == '.' && rhs[i + 1] == '.') {
            dots = rhs + i;
            if (i + 2 < rhs_len && rhs[i + 2] == '=') inclusive = true;
            break;
        }
    }

    if (dots) {
        size_t start_len = (size_t)(dots - rhs);
        while (start_len > 0 && rhs[start_len - 1] == ' ') start_len--;
        const char *end_expr = dots + (inclusive ? 3 : 2);
        size_t end_len = rhs_len - (size_t)(end_expr - rhs);
        while (end_len > 0 && end_expr[0] == ' ') { end_expr++; end_len--; }

        /* Check optional `step S` in end_expr */
        const char *step_pos = NULL;
        for (size_t i = 0; i + 6 <= end_len; i++) {
            if (memcmp(end_expr + i, " step ", 6) == 0) {
                step_pos = end_expr + i;
                break;
            }
        }
        size_t bound_len = step_pos ? (size_t)(step_pos - end_expr) : end_len;
        while (bound_len > 0 && end_expr[bound_len - 1] == ' ') bound_len--;

        pura_buf_puts(out, "for (");
        emit_c_typed_decl(out, vname, vname_len, vtype, vtype_len);
        pura_buf_puts(out, " = ");
        translate_tokens_pura_to_c(out, rhs, start_len);
        pura_buf_puts(out, "; ");
        translate_tokens_pura_to_c(out, vname, vname_len);
        pura_buf_puts(out, inclusive ? " <= " : " < ");
        translate_tokens_pura_to_c(out, end_expr, bound_len);
        pura_buf_puts(out, "; ");
        translate_tokens_pura_to_c(out, vname, vname_len);
        if (step_pos) {
            const char *sval = step_pos + 6;
            size_t slen = end_len - (size_t)(sval - end_expr);
            while (slen > 0 && sval[0] == ' ') { sval++; slen--; }
            pura_buf_puts(out, " += ");
            translate_tokens_pura_to_c(out, sval, slen);
        } else {
            pura_buf_puts(out, "++");
        }
        pura_buf_puts(out, ") {");
        return true;
    }

    /* C++ range-based for: `for item in container:` -> `for (auto item : container) {` */
    pura_buf_puts(out, "for (");
    if (vcolon) {
        emit_c_typed_decl(out, vname, vname_len, vtype, vtype_len);
    } else {
        pura_buf_puts(out, "auto ");
        translate_tokens_pura_to_c(out, vname, vname_len);
    }
    pura_buf_puts(out, " : ");
    translate_tokens_pura_to_c(out, rhs, rhs_len);
    pura_buf_puts(out, ") {");
    return true;
}

static void close_block_frame(PuraBuf *out, BlockFrame frame, const char *c_indent) {
    if (frame.kind == BLOCK_CASE) {
        return; /* case blocks don't emit a closing '}' in C */
    }
    pura_buf_indent(out, c_indent, strlen(c_indent), frame.level);
    if (frame.kind == BLOCK_STRUCT_OR_CLASS || frame.kind == BLOCK_ENUM) {
        pura_buf_puts(out, "};\n");
    } else {
        pura_buf_puts(out, "}\n");
    }
}

char *pura_to_c(const char *src, size_t len, PuraError *err, size_t *out_len) {
    if (err) { err->message = NULL; err->line = 0; err->col = 0; }

    /* Reject UTF-8 BOM */
    if (len >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        if (err) { err->message = "UTF-8 BOM is not allowed"; err->line = 1; err->col = 1; }
        return NULL;
    }

    PuraBuf out;
    if (!pura_buf_init(&out, len * 2 + 128)) return NULL;

    symtab_reset();

    char indent_char = 0;
    size_t indent_width = 0;
    BlockFrame stack[MAX_INDENT_STACK];
    size_t sp = 0;
    size_t current_level = 0;
    bool prev_line_opened_block = false;
    size_t prev_block_line = 0;

    const char *c_indent = "    ";
    size_t p = 0;
    size_t line_no = 0;

    while (p < len) {
        line_no++;
        size_t line_start = p;
        while (p < len && src[p] != '\n' && src[p] != '\r') p++;
        size_t line_end = p;
        if (p < len) {
            if (src[p] == '\r' && p + 1 < len && src[p + 1] == '\n') p += 2;
            else p++;
        }

        /* Measure leading whitespace */
        size_t ws = 0;
        bool has_space = false;
        bool has_tab = false;
        while (line_start + ws < line_end) {
            char c = src[line_start + ws];
            if (c == ' ') { has_space = true; ws++; }
            else if (c == '\t') { has_tab = true; ws++; }
            else break;
        }

        /* Blank line? */
        if (line_start + ws == line_end) {
            pura_buf_putc(&out, '\n');
            continue;
        }

        if (has_space && has_tab) {
            if (err) {
                err->message = "mixed spaces and tabs in indentation";
                err->line = line_no;
                err->col = ws;
            }
            pura_buf_free(&out);
            return NULL;
        }

        /* Check for trailing whitespace */
        if (src[line_end - 1] == ' ' || src[line_end - 1] == '\t') {
            if (err) {
                err->message = "trailing whitespace is not allowed";
                err->line = line_no;
                err->col = line_end - line_start;
            }
            pura_buf_free(&out);
            return NULL;
        }

        size_t level = 0;
        if (ws > 0) {
            char ch = has_tab ? '\t' : ' ';
            if (indent_width == 0) {
                if (current_level == 0 && !prev_line_opened_block) {
                    if (err) {
                        err->message = "unexpected indentation at root level";
                        err->line = line_no;
                        err->col = 1;
                    }
                    pura_buf_free(&out);
                    return NULL;
                }
                indent_char = ch;
                indent_width = ws;
                level = 1;
            } else {
                if (ch != indent_char) {
                    if (err) {
                        err->message = "inconsistent indentation character (spaces vs tabs)";
                        err->line = line_no;
                        err->col = 1;
                    }
                    pura_buf_free(&out);
                    return NULL;
                }
                if (ws % indent_width != 0) {
                    if (err) {
                        err->message = "indentation is not a multiple of the file's indent unit";
                        err->line = line_no;
                        err->col = ws + 1;
                    }
                    pura_buf_free(&out);
                    return NULL;
                }
                level = ws / indent_width;
            }
        }

        const char *content = src + line_start + ws;
        size_t clen = line_end - (line_start + ws);

        /* Verify that if the previous line opened a block, this non-comment line indented */
        if (prev_line_opened_block && content[0] != '#') {
            if (level != current_level) {
                if (err) {
                    err->message = "expected indented block after ':'";
                    err->line = prev_block_line;
                    err->col = 1;
                }
                pura_buf_free(&out);
                return NULL;
            }
            prev_line_opened_block = false;
        } else if (!prev_line_opened_block && level > current_level && content[0] != '#') {
            if (err) {
                err->message = "unexpected indentation increase without a block header";
                err->line = line_no;
                err->col = ws + 1;
            }
            pura_buf_free(&out);
            return NULL;
        }

        /* Full-line comment `# ...` */
        if (content[0] == '#') {
            pura_buf_indent(&out, c_indent, strlen(c_indent), level);
            pura_buf_puts(&out, "//");
            pura_buf_append(&out, content + 1, clen - 1);
            pura_buf_putc(&out, '\n');
            continue;
        }

        /* Split inline `#` comment if any */
        size_t code_len = clen;
        const char *inline_comment = split_inline_hash_comment(content, clen, &code_len);

        /* Check if this line is `elif ...:` or `else:` or `while ...` closing a `do:` block */
        size_t kw_after = 0;
        bool is_elif = starts_with_kw(content, code_len, "elif", &kw_after);
        bool is_else = (code_len == 5 && memcmp(content, "else:", 5) == 0);
        bool is_catch = starts_with_kw(content, code_len, "catch", &kw_after) ||
                        starts_with_kw(content, code_len, "except", &kw_after);
        bool is_case = starts_with_kw(content, code_len, "case", &kw_after) ||
                       (code_len == 8 && memcmp(content, "default:", 8) == 0);

        /* Close blocks down to `level` */
        while (sp > 0 && stack[sp - 1].level >= level) {
            /* If we are at `level` and this is `elif`/`else`/`catch` continuing the block at `level`,
               emit `} else ...` on the same line instead of a standalone `}`! */
            if (stack[sp - 1].level == level && stack[sp - 1].kind == BLOCK_NORMAL &&
                (is_elif || is_else || is_catch)) {
                break;
            }
            /* If we are closing a `case` block because a sibling `case`/`default` arrived at `level` */
            if (stack[sp - 1].level == level && stack[sp - 1].kind == BLOCK_CASE && is_case) {
                sp--;
                break;
            }
            /* If we are closing a `do:` block with `while cond` at `level` */
            if (stack[sp - 1].level == level && stack[sp - 1].kind == BLOCK_DO_WHILE &&
                starts_with_kw(content, code_len, "while", &kw_after) &&
                content[code_len - 1] != ':') {
                break;
            }
            sp--;
            close_block_frame(&out, stack[sp], c_indent);
        }
        symtab_pop_scope(level);
        current_level = level;

        /* Handle `do:` followed by `while cond` at same level */
        if (sp > 0 && stack[sp - 1].level == level && stack[sp - 1].kind == BLOCK_DO_WHILE &&
            starts_with_kw(content, code_len, "while", &kw_after) && content[code_len - 1] != ':') {
            sp--;
            pura_buf_indent(&out, c_indent, strlen(c_indent), level);
            pura_buf_puts(&out, "} while (");
            translate_tokens_pura_to_c(&out, content + kw_after, code_len - kw_after);
            pura_buf_puts(&out, ");");
            if (inline_comment) {
                pura_buf_puts(&out, " //");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
            }
            pura_buf_putc(&out, '\n');
            continue;
        }

        /* Handle `pass` (no-op empty block statement) */
        if (code_len == 4 && memcmp(content, "pass", 4) == 0) {
            if (inline_comment) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "//");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
                pura_buf_putc(&out, '\n');
            }
            continue;
        }

        /* Preprocessor & module imports:
         *   - Extensionless (`import math_utils` or `from math_utils import ...`):
         *     Imports a Pura `.pu` module -> `#include "math_utils.h"`
         *   - With `.h`/`.c`/`.hpp` extension (`import stdio.h`, `import "my.h"`):
         *     Imports an untranspiled C/C++ header or source file directly (`#include <stdio.h>` or `#include "my.h"`).
         *   - `define NAME val` -> `#define NAME val`
         *   - `ifdef NAME` / `ifndef NAME` / `endif` / `pragma ...`
         */
        if (starts_with_kw(content, code_len, "import", &kw_after)) {
            const char *imp = content + kw_after;
            size_t ilen = code_len - kw_after;
            pura_buf_puts(&out, "#include ");
            if (ilen > 0 && (imp[0] == '<' || imp[0] == '"')) {
                pura_buf_append(&out, imp, ilen);
            } else if (memchr(imp, '.', ilen) != NULL) {
                /* Explicit extension (.h, .c, .hpp): untranspiled C/C++ header/file */
                pura_buf_putc(&out, '<');
                pura_buf_append(&out, imp, ilen);
                pura_buf_putc(&out, '>');
            } else {
                /* Extensionless: Pura .pu module (transpiled to local "module.h") */
                pura_buf_putc(&out, '"');
                for (size_t k = 0; k < ilen; k++) {
                    pura_buf_putc(&out, imp[k]);
                }
                pura_buf_puts(&out, ".h\"");
            }
            if (inline_comment) {
                pura_buf_puts(&out, " //");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
            }
            pura_buf_putc(&out, '\n');
            continue;
        }

        if (starts_with_kw(content, code_len, "from", &kw_after)) {
            const char *f = content + kw_after;
            size_t flen = code_len - kw_after;
            /* Extract module up to ` import` */
            size_t mlen = 0;
            while (mlen < flen && f[mlen] != ' ') mlen++;
            pura_buf_puts(&out, "#include ");
            if (mlen > 0 && (f[0] == '<' || f[0] == '"')) {
                pura_buf_append(&out, f, mlen);
            } else if (memchr(f, '.', mlen) != NULL) {
                pura_buf_putc(&out, '<');
                pura_buf_append(&out, f, mlen);
                pura_buf_putc(&out, '>');
            } else {
                pura_buf_putc(&out, '"');
                pura_buf_append(&out, f, mlen);
                pura_buf_puts(&out, ".h\"");
            }
            pura_buf_putc(&out, '\n');
            continue;
        }

        if (starts_with_kw(content, code_len, "define", &kw_after)) {
            pura_buf_puts(&out, "#define ");
            translate_tokens_pura_to_c(&out, content + kw_after, code_len - kw_after);
            if (inline_comment) {
                pura_buf_puts(&out, " //");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
            }
            pura_buf_putc(&out, '\n');
            continue;
        }

        if (starts_with_kw(content, code_len, "ifdef", &kw_after)) {
            pura_buf_puts(&out, "#ifdef ");
            pura_buf_append(&out, content + kw_after, code_len - kw_after);
            pura_buf_putc(&out, '\n');
            continue;
        }
        if (starts_with_kw(content, code_len, "ifndef", &kw_after)) {
            pura_buf_puts(&out, "#ifndef ");
            pura_buf_append(&out, content + kw_after, code_len - kw_after);
            pura_buf_putc(&out, '\n');
            continue;
        }
        if (code_len == 5 && memcmp(content, "endif", 5) == 0) {
            pura_buf_puts(&out, "#endif\n");
            continue;
        }
        if (starts_with_kw(content, code_len, "pragma", &kw_after)) {
            pura_buf_puts(&out, "#pragma ");
            pura_buf_append(&out, content + kw_after, code_len - kw_after);
            pura_buf_putc(&out, '\n');
            continue;
        }

        /* Check if line is a block header ending with `:` */
        bool ends_with_colon = (code_len > 0 && content[code_len - 1] == ':');
        if (ends_with_colon) {
            size_t hlen = code_len - 1;
            while (hlen > 0 && content[hlen - 1] == ' ') hlen--;

            /* `elif cond:` or `else:` continuing an open `if` block */
            if (is_elif && sp > 0 && stack[sp - 1].level == level) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "} else if (");
                translate_tokens_pura_to_c(&out, content + kw_after, hlen - kw_after);
                pura_buf_puts(&out, ") {");
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (is_else && sp > 0 && stack[sp - 1].level == level) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "} else {");
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (is_catch && sp > 0 && stack[sp - 1].level == level) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "} catch (");
                const char *cexpr = content + kw_after;
                size_t cl = hlen - kw_after;
                if (cl == 0) {
                    pura_buf_puts(&out, "...");
                } else {
                    emit_c_param_list(&out, cexpr, cl, level + 1);
                }
                pura_buf_puts(&out, ") {");
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "if", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "if (");
                translate_tokens_pura_to_c(&out, content + kw_after, hlen - kw_after);
                pura_buf_puts(&out, ") {");
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "while", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "while (");
                translate_tokens_pura_to_c(&out, content + kw_after, hlen - kw_after);
                pura_buf_puts(&out, ") {");
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (hlen == 2 && memcmp(content, "do", 2) == 0) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "do {");
                stack[sp++] = (BlockFrame){level, BLOCK_DO_WHILE};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "for", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                const char *fexpr = content + kw_after;
                size_t flen = hlen - kw_after;
                if (!try_emit_for_in(&out, fexpr, flen)) {
                    pura_buf_puts(&out, "for (");
                    const char *semi1 = memchr(fexpr, ';', flen);
                    if (semi1) {
                        size_t init_len = (size_t)(semi1 - fexpr);
                        const char *d_eq = NULL;
                        const char *d_col = find_decl_colon(fexpr, init_len, &d_eq);
                        if (d_col) {
                            size_t nlen = (size_t)(d_col - fexpr);
                            const char *ts = d_col + 1;
                            size_t tlen = d_eq ? (size_t)(d_eq - ts) : (init_len - (size_t)(ts - fexpr));
                            if (is_pura_ptr_type(ts, tlen)) {
                                symtab_add_ptr_var(fexpr, nlen, level + 1);
                            }
                            emit_c_typed_decl(&out, fexpr, nlen, ts, tlen);
                            if (d_eq) {
                                const char *v = d_eq + 1;
                                size_t vlen = init_len - (size_t)(v - fexpr);
                                while (vlen > 0 && v[0] == ' ') { v++; vlen--; }
                                pura_buf_puts(&out, " = ");
                                translate_tokens_pura_to_c(&out, v, vlen);
                            }
                        } else {
                            translate_tokens_pura_to_c(&out, fexpr, init_len);
                        }
                        translate_tokens_pura_to_c(&out, semi1, flen - init_len);
                    } else {
                        translate_tokens_pura_to_c(&out, fexpr, flen);
                    }
                    pura_buf_puts(&out, ") {");
                }
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "switch", &kw_after) ||
                       starts_with_kw(content, hlen, "match", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "switch (");
                translate_tokens_pura_to_c(&out, content + kw_after, hlen - kw_after);
                pura_buf_puts(&out, ") {");
                stack[sp++] = (BlockFrame){level, BLOCK_SWITCH};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "case", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "case ");
                translate_tokens_pura_to_c(&out, content + kw_after, hlen - kw_after);
                pura_buf_putc(&out, ':');
                stack[sp++] = (BlockFrame){level, BLOCK_CASE};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (hlen == 7 && memcmp(content, "default", 7) == 0) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "default:");
                stack[sp++] = (BlockFrame){level, BLOCK_CASE};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if ((hlen == 6 && memcmp(content, "public", 6) == 0) ||
                       (hlen == 7 && memcmp(content, "private", 7) == 0) ||
                       (hlen == 9 && memcmp(content, "protected", 9) == 0)) {
                /* C++ access specifiers inside class/struct */
                pura_buf_indent(&out, c_indent, strlen(c_indent), level > 0 ? level - 1 : 0);
                pura_buf_append(&out, content, hlen);
                pura_buf_puts(&out, ":\n");
                continue;
            } else if (starts_with_kw(content, hlen, "struct", &kw_after) ||
                       starts_with_kw(content, hlen, "class", &kw_after) ||
                       starts_with_kw(content, hlen, "union", &kw_after)) {
                if (starts_with_kw(content, hlen, "struct", &kw_after)) {
                    size_t tlen = hlen - kw_after;
                    while (tlen > 0 && content[kw_after + tlen - 1] == ' ') tlen--;
                    symtab_add_tag("struct", content + kw_after, tlen);
                } else if (starts_with_kw(content, hlen, "union", &kw_after)) {
                    size_t tlen = hlen - kw_after;
                    while (tlen > 0 && content[kw_after + tlen - 1] == ' ') tlen--;
                    symtab_add_tag("union", content + kw_after, tlen);
                }
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                /* Check for inheritance `class Dog(Animal)` -> `class Dog : public Animal` */
                const char *lp = memchr(content, '(', hlen);
                const char *rp = lp ? memchr(lp, ')', hlen - (size_t)(lp - content)) : NULL;
                if (lp && rp) {
                    translate_tokens_pura_to_c(&out, content, (size_t)(lp - content));
                    pura_buf_puts(&out, " : public ");
                    translate_tokens_pura_to_c(&out, lp + 1, (size_t)(rp - lp - 1));
                    pura_buf_puts(&out, " {");
                } else {
                    translate_tokens_pura_to_c(&out, content, hlen);
                    pura_buf_puts(&out, " {");
                }
                stack[sp++] = (BlockFrame){level, BLOCK_STRUCT_OR_CLASS};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "enum", &kw_after)) {
                size_t tlen = hlen - kw_after;
                while (tlen > 0 && content[kw_after + tlen - 1] == ' ') tlen--;
                symtab_add_tag("enum", content + kw_after, tlen);
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                translate_tokens_pura_to_c(&out, content, hlen);
                pura_buf_puts(&out, " {");
                stack[sp++] = (BlockFrame){level, BLOCK_ENUM};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "namespace", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                translate_tokens_pura_to_c(&out, content, hlen);
                pura_buf_puts(&out, " {");
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (hlen == 3 && memcmp(content, "try", 3) == 0) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                pura_buf_puts(&out, "try {");
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "def", &kw_after) ||
                       starts_with_kw(content, hlen, "fn", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                if (!emit_pura_function_def(&out, NULL, content + kw_after, hlen - kw_after, true, level + 1)) {
                    if (err) { err->message = "invalid function definition"; err->line = line_no; err->col = 1; }
                    pura_buf_free(&out);
                    return NULL;
                }
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else if (starts_with_kw(content, hlen, "static inline def", &kw_after) ||
                       starts_with_kw(content, hlen, "static def", &kw_after) ||
                       starts_with_kw(content, hlen, "inline def", &kw_after) ||
                       starts_with_kw(content, hlen, "virtual def", &kw_after)) {
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                char qual[32];
                /* Copy everything before ` def` */
                size_t qlen = kw_after;
                while (qlen > 0 && content[qlen - 1] == ' ') qlen--;
                if (qlen >= 4 && memcmp(content + qlen - 4, " def", 4) == 0) {
                    qlen -= 4;
                }
                memcpy(qual, content, qlen);
                qual[qlen] = '\0';
                if (!emit_pura_function_def(&out, qual, content + kw_after, hlen - kw_after, true, level + 1)) {
                    if (err) { err->message = "invalid qualified function definition"; err->line = line_no; err->col = 1; }
                    pura_buf_free(&out);
                    return NULL;
                }
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            } else {
                /* Generic C/C++ block header ending in `:` */
                pura_buf_indent(&out, c_indent, strlen(c_indent), level);
                translate_tokens_pura_to_c(&out, content, hlen);
                pura_buf_puts(&out, " {");
                stack[sp++] = (BlockFrame){level, BLOCK_NORMAL};
                current_level = level + 1;
                prev_line_opened_block = true;
                prev_block_line = line_no;
            }

            if (inline_comment) {
                pura_buf_puts(&out, " //");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
            }
            pura_buf_putc(&out, '\n');
            continue;
        }

        /* Non-block statement at `level` */
        pura_buf_indent(&out, c_indent, strlen(c_indent), level);

        /* Inside an `enum` block, entries are comma-separated rather than semicolon-terminated */
        if (sp > 0 && stack[sp - 1].kind == BLOCK_ENUM && stack[sp - 1].level + 1 == level) {
            translate_tokens_pura_to_c(&out, content, code_len);
            if (code_len > 0 && content[code_len - 1] != ',') {
                pura_buf_putc(&out, ',');
            }
            if (inline_comment) {
                pura_buf_puts(&out, " //");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
            }
            pura_buf_putc(&out, '\n');
            continue;
        }

        /* Function prototype without body: `[static|inline] def foo(a: i32) -> i32` */
        if (starts_with_kw(content, code_len, "def", &kw_after) ||
            starts_with_kw(content, code_len, "fn", &kw_after)) {
            if (!emit_pura_function_def(&out, NULL, content + kw_after, code_len - kw_after, false, level + 1)) {
                if (err) { err->message = "invalid function prototype"; err->line = line_no; err->col = 1; }
                pura_buf_free(&out);
                return NULL;
            }
            if (inline_comment) {
                pura_buf_puts(&out, " //");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
            }
            pura_buf_putc(&out, '\n');
            continue;
        }
        if (starts_with_kw(content, code_len, "static inline def", &kw_after) ||
            starts_with_kw(content, code_len, "static def", &kw_after) ||
            starts_with_kw(content, code_len, "inline def", &kw_after)) {
            char qual[32];
            size_t qlen = kw_after;
            while (qlen > 0 && content[qlen - 1] == ' ') qlen--;
            if (qlen >= 4 && memcmp(content + qlen - 4, " def", 4) == 0) {
                qlen -= 4;
            }
            memcpy(qual, content, qlen);
            qual[qlen] = '\0';
            if (!emit_pura_function_def(&out, qual, content + kw_after, code_len - kw_after, false, level + 1)) {
                if (err) { err->message = "invalid qualified function prototype"; err->line = line_no; err->col = 1; }
                pura_buf_free(&out);
                return NULL;
            }
            if (inline_comment) {
                pura_buf_puts(&out, " //");
                pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
            }
            pura_buf_putc(&out, '\n');
            continue;
        }

        /* Forward tag declaration without body: `struct Foo` or `union Foo` */
        if (starts_with_kw(content, code_len, "struct", &kw_after) ||
            starts_with_kw(content, code_len, "union", &kw_after)) {
            const char *tag = content + kw_after;
            size_t tlen = code_len - kw_after;
            while (tlen > 0 && tag[tlen - 1] == ' ') tlen--;
            bool simple_tag = (tlen > 0);
            for (size_t k = 0; k < tlen; k++) {
                if (!is_ident_cont(tag[k])) { simple_tag = false; break; }
            }
            if (simple_tag) {
                symtab_add_tag(content[0] == 's' ? "struct" : "union", tag, tlen);
            }
        }

        /* Type alias: `type Name = TargetType` -> `typedef TargetType Name;` */
        if (starts_with_kw(content, code_len, "type", &kw_after)) {
            const char *rest = content + kw_after;
            size_t rlen = code_len - kw_after;
            const char *eq = memchr(rest, '=', rlen);
            if (eq) {
                size_t alias_len = (size_t)(eq - rest);
                while (alias_len > 0 && rest[alias_len - 1] == ' ') alias_len--;
                const char *target = eq + 1;
                size_t tlen = rlen - (size_t)(target - rest);
                while (tlen > 0 && target[0] == ' ') { target++; tlen--; }
                pura_buf_puts(&out, "typedef ");
                emit_c_typed_decl(&out, rest, alias_len, target, tlen);
                pura_buf_putc(&out, ';');
                if (inline_comment) {
                    pura_buf_puts(&out, " //");
                    pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
                }
                pura_buf_putc(&out, '\n');
                continue;
            }
        }

        /* Variable or field declaration: `[const|static|extern] name: type [= value]` */
        const char *decl_eq = NULL;
        const char *decl_colon = find_decl_colon(content, code_len, &decl_eq);
        if (decl_colon) {
            const char *lhs = content;
            size_t lhs_len = (size_t)(decl_colon - content);
            while (lhs_len > 0 && lhs[lhs_len - 1] == ' ') lhs_len--;

            /* Extract optional storage/const prefix before variable name */
            const char *last_space = NULL;
            for (size_t i = 0; i < lhs_len; i++) {
                if (lhs[i] == ' ') last_space = lhs + i;
            }
            if (last_space) {
                size_t qlen = (size_t)(last_space - lhs);
                pura_buf_append(&out, lhs, qlen);
                pura_buf_putc(&out, ' ');
                lhs = last_space + 1;
                lhs_len -= (qlen + 1);
            }

            const char *ts = decl_colon + 1;
            size_t tlen = decl_eq ? (size_t)(decl_eq - ts) : (code_len - (size_t)(ts - content));
            if (is_pura_ptr_type(ts, tlen) &&
                !(sp > 0 && stack[sp - 1].kind == BLOCK_STRUCT_OR_CLASS && stack[sp - 1].level + 1 == level)) {
                symtab_add_ptr_var(lhs, lhs_len, level);
            }
            emit_c_typed_decl(&out, lhs, lhs_len, ts, tlen);

            if (decl_eq) {
                const char *val = decl_eq + 1;
                size_t vlen = code_len - (size_t)(val - content);
                while (vlen > 0 && val[0] == ' ') { val++; vlen--; }
                pura_buf_puts(&out, " = ");
                translate_tokens_pura_to_c(&out, val, vlen);
            }
            pura_buf_putc(&out, ';');
        } else if (level == 0 && try_emit_pura_macro_define(&out, content, code_len)) {
            /* Root-level Pythonic constant/macro assignment:
             *   `MAX_SIZE = 1024`         -> `#define MAX_SIZE 1024`
             *   `MIN(a, b) = ((a) < (b))` -> `#define MIN(a, b) ((a) < (b))`
             */
        } else {
            /* Standard expression / statement */
            translate_tokens_pura_to_c(&out, content, code_len);
            if (code_len > 0 && content[code_len - 1] != ';' && content[code_len - 1] != ',') {
                pura_buf_putc(&out, ';');
            }
        }

        if (inline_comment) {
            pura_buf_puts(&out, " //");
            pura_buf_append(&out, inline_comment + 1, (size_t)(content + clen - (inline_comment + 1)));
        }
        pura_buf_putc(&out, '\n');
    }

    if (prev_line_opened_block) {
        if (err) {
            err->message = "dangling block header at end of file (expected indented block)";
            err->line = prev_block_line;
            err->col = 1;
        }
        pura_buf_free(&out);
        return NULL;
    }

    while (sp > 0) {
        sp--;
        close_block_frame(&out, stack[sp], c_indent);
    }

    if (out_len) *out_len = out.len;
    return out.data;
}

/* -------------------------------------------------------------------------
 * C/C++ -> Pura Transpiler & Canonical Pura Formatter
 * ------------------------------------------------------------------------- */

static bool valid_indent_unit(const char *unit, size_t *out_len) {
    if (!unit || unit[0] == '\0') return false;
    char ch = unit[0];
    if (ch != ' ' && ch != '\t') return false;
    size_t n = 1;
    while (unit[n] != '\0') {
        if (unit[n] != ch) return false;
        n++;
    }
    *out_len = n;
    return true;
}

/* Try to parse a C parameter `const char* name` or `int32_t arr[10]` into Pura `name: cstr` */
static void emit_pura_param(PuraBuf *out, const char *p, size_t len) {
    while (len > 0 && p[0] == ' ') { p++; len--; }
    while (len > 0 && p[len - 1] == ' ') len--;
    if (len == 0 || (len == 4 && memcmp(p, "void", 4) == 0)) return;
    if (len == 3 && memcmp(p, "...", 3) == 0) {
        pura_buf_puts(out, "...");
        return;
    }

    /* Check for array brackets at end `type name[N]` */
    const char *bracket = memchr(p, '[', len);
    size_t pre_bracket_len = bracket ? (size_t)(bracket - p) : len;
    while (pre_bracket_len > 0 && p[pre_bracket_len - 1] == ' ') pre_bracket_len--;

    /* Find identifier at end of pre_bracket_len */
    size_t id_end = pre_bracket_len;
    size_t id_start = id_end;
    while (id_start > 0 && is_ident_cont(p[id_start - 1])) id_start--;

    if (id_start == 0 || id_start == id_end) {
        translate_tokens_c_to_pura(out, p, len);
        return;
    }

    size_t type_end = id_start;
    while (type_end > 0 && p[type_end - 1] == ' ') type_end--;

    /* Move any leading `*` or `&` attached to variable name onto the type */
    while (type_end < id_start && (p[type_end] == '*' || p[type_end] == '&')) {
        type_end++;
    }

    pura_buf_append(out, p + id_start, id_end - id_start);
    pura_buf_puts(out, ": ");
    translate_type_c_to_pura(out, p, type_end);
    if (bracket) {
        translate_tokens_c_to_pura(out, bracket, len - (size_t)(bracket - p));
    }
}

/* Try to convert a C function header `ret_type func_name(params)` into `def func_name(params) -> ret_type:` */
static bool try_emit_c_func_to_pura(PuraBuf *out, const char *s, size_t len, bool is_block) {
    /* Exclude control statements */
    size_t dummy = 0;
    if (starts_with_kw(s, len, "if", &dummy) ||
        starts_with_kw(s, len, "for", &dummy) ||
        starts_with_kw(s, len, "while", &dummy) ||
        starts_with_kw(s, len, "switch", &dummy) ||
        starts_with_kw(s, len, "catch", &dummy) ||
        starts_with_kw(s, len, "struct", &dummy) ||
        starts_with_kw(s, len, "class", &dummy) ||
        starts_with_kw(s, len, "enum", &dummy) ||
        starts_with_kw(s, len, "namespace", &dummy)) {
        return false;
    }

    const char *lparen = memchr(s, '(', len);
    if (!lparen) return false;
    /* Must not contain '=' before '(' (otherwise it is a variable initialization `int x = fn()`) */
    if (memchr(s, '=', (size_t)(lparen - s)) != NULL) return false;
    const char *rparen = NULL;
    int depth = 0;
    for (size_t i = (size_t)(lparen - s); i < len; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            depth--;
            if (depth == 0) { rparen = s + i; break; }
        }
    }
    if (!rparen) return false;

    /* Before lparen must be ` [static|inline] ret_type func_name` */
    size_t pre_len = (size_t)(lparen - s);
    while (pre_len > 0 && s[pre_len - 1] == ' ') pre_len--;
    size_t fn_start = pre_len;
    while (fn_start > 0 && (is_ident_cont(s[fn_start - 1]) || s[fn_start - 1] == ':' || s[fn_start - 1] == '~')) {
        fn_start--;
    }
    if (fn_start == 0 || fn_start == pre_len) return false;

    size_t ret_end = fn_start;
    while (ret_end > 0 && s[ret_end - 1] == ' ') ret_end--;
    if (ret_end == 0) return false;

    /* Check for leading `static inline `, `static `, or `inline ` */
    const char *ret_start = s;
    if (ret_end > 14 && memcmp(s, "static inline ", 14) == 0) {
        pura_buf_puts(out, "static inline ");
        ret_start += 14;
        ret_end -= 14;
    } else if (ret_end > 7 && memcmp(s, "static ", 7) == 0) {
        pura_buf_puts(out, "static ");
        ret_start += 7;
        ret_end -= 7;
    } else if (ret_end > 7 && memcmp(s, "inline ", 7) == 0) {
        pura_buf_puts(out, "inline ");
        ret_start += 7;
        ret_end -= 7;
    }

    pura_buf_puts(out, "def ");
    pura_buf_append(out, s + fn_start, pre_len - fn_start);
    pura_buf_putc(out, '(');

    /* Parse comma-separated parameters */
    const char *params = lparen + 1;
    size_t plen = (size_t)(rparen - lparen - 1);
    size_t i = 0;
    bool first = true;
    while (i < plen) {
        size_t ps = i;
        int d = 0;
        while (i < plen) {
            if (params[i] == '(' || params[i] == '<' || params[i] == '[') d++;
            else if (params[i] == ')' || params[i] == '>' || params[i] == ']') d--;
            else if (params[i] == ',' && d == 0) break;
            i++;
        }
        size_t item_len = i - ps;
        if (i < plen && params[i] == ',') i++;

        const char *item = params + ps;
        while (item_len > 0 && item[0] == ' ') { item++; item_len--; }
        while (item_len > 0 && item[item_len - 1] == ' ') item_len--;
        if (item_len == 0 || (item_len == 4 && memcmp(item, "void", 4) == 0)) continue;

        if (!first) pura_buf_puts(out, ", ");
        first = false;
        emit_pura_param(out, item, item_len);
    }
    pura_buf_putc(out, ')');

    /* Trailing `const` after rparen? */
    const char *after_rp = rparen + 1;
    size_t after_len = len - (size_t)(after_rp - s);
    while (after_len > 0 && after_rp[0] == ' ') { after_rp++; after_len--; }
    if (after_len >= 5 && memcmp(after_rp, "const", 5) == 0) {
        pura_buf_puts(out, " const");
    }

    /* Emit `-> ret_type` unless void */
    if (!(ret_end == 4 && memcmp(ret_start, "void", 4) == 0)) {
        pura_buf_puts(out, " -> ");
        translate_type_c_to_pura(out, ret_start, ret_end);
    }
    if (is_block) pura_buf_putc(out, ':');
    return true;
}

/* Try to convert a simple C variable declaration `int32_t x = 10` or `const char* msg = "hi"` to `x: i32 = 10` */
static bool try_emit_c_var_decl_to_pura(PuraBuf *out, const char *s, size_t len) {
    size_t dummy = 0;
    if (starts_with_kw(s, len, "return", &dummy) ||
        starts_with_kw(s, len, "break", &dummy) ||
        starts_with_kw(s, len, "continue", &dummy) ||
        starts_with_kw(s, len, "goto", &dummy) ||
        starts_with_kw(s, len, "case", &dummy) ||
        starts_with_kw(s, len, "delete", &dummy) ||
        starts_with_kw(s, len, "throw", &dummy)) {
        return false;
    }

    /* Split at first top-level `=` if present */
    const char *eq = NULL;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if (depth == 0) {
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '<' || c == '>' || c == '!') {
                if (i + 1 < len && s[i + 1] == '=') return false; /* compound assignment */
            }
            if (c == '=') {
                if (i + 1 < len && s[i + 1] == '=') { i++; continue; }
                eq = s + i;
                break;
            }
        }
    }

    size_t lhs_len = eq ? (size_t)(eq - s) : len;
    while (lhs_len > 0 && s[lhs_len - 1] == ' ') lhs_len--;
    if (lhs_len == 0) return false;

    /* Must not be a function call or unary dereference/address-of on LHS */
    if (memchr(s, '(', lhs_len) != NULL) return false;
    if (lhs_len > 0 && (s[0] == '*' || s[0] == '&')) return false;

    const char *lhs = s;
    const char *qual = NULL;
    size_t qual_len = 0;
    if (lhs_len > 6 && memcmp(lhs, "const ", 6) == 0 &&
        !(lhs_len >= 10 && memcmp(lhs, "const char", 10) == 0)) {
        qual = "const";
        qual_len = 5;
        lhs += 6;
        lhs_len -= 6;
    } else if (lhs_len > 7 && memcmp(lhs, "static ", 7) == 0) {
        qual = "static";
        qual_len = 6;
        lhs += 7;
        lhs_len -= 7;
    }

    /* Check for array brackets `type name[N]` */
    const char *bracket = memchr(lhs, '[', lhs_len);
    size_t pre_bracket = bracket ? (size_t)(bracket - lhs) : lhs_len;
    while (pre_bracket > 0 && lhs[pre_bracket - 1] == ' ') pre_bracket--;

    size_t name_end = pre_bracket;
    size_t name_start = name_end;
    while (name_start > 0 && is_ident_cont(lhs[name_start - 1])) name_start--;
    if (name_start == 0 || name_start == name_end) return false;

    size_t type_end = name_start;
    while (type_end > 0 && lhs[type_end - 1] == ' ') type_end--;
    if (type_end == 0) return false;

    /* Verify type part looks like a valid type (alphanumeric, `_`, `*`, `&`, ` `, `:`, `<`, `>`) */
    for (size_t i = 0; i < type_end; i++) {
        char c = lhs[i];
        if (!is_ident_cont(c) && c != ' ' && c != '*' && c != '&' && c != ':' && c != '<' && c != '>') {
            return false;
        }
    }

    if (qual) {
        pura_buf_append(out, qual, qual_len);
        pura_buf_putc(out, ' ');
    }
    pura_buf_append(out, lhs + name_start, name_end - name_start);
    pura_buf_puts(out, ": ");
    translate_type_c_to_pura(out, lhs, type_end);
    if (bracket) {
        translate_tokens_c_to_pura(out, bracket, lhs_len - (size_t)(bracket - lhs));
    }
    if (eq) {
        const char *rhs = eq + 1;
        size_t rhs_len = len - (size_t)(rhs - s);
        while (rhs_len > 0 && rhs[0] == ' ') { rhs++; rhs_len--; }
        pura_buf_puts(out, " = ");
        translate_tokens_c_to_pura(out, rhs, rhs_len);
    }
    return true;
}

/* Try to convert a C 3-clause `for (T i = start; i < stop; i++)` into Pythonic `for i in range(...)` */
static bool try_emit_c_for_range_to_pura(PuraBuf *out, const char *init_s, size_t init_len,
                                           const char *cond_s, size_t cond_len,
                                           const char *step_s, size_t step_len) {
    while (init_len > 0 && init_s[0] == ' ') { init_s++; init_len--; }
    while (init_len > 0 && init_s[init_len - 1] == ' ') init_len--;
    while (cond_len > 0 && cond_s[0] == ' ') { cond_s++; cond_len--; }
    while (cond_len > 0 && cond_s[cond_len - 1] == ' ') cond_len--;
    while (step_len > 0 && step_s[0] == ' ') { step_s++; step_len--; }
    while (step_len > 0 && step_s[step_len - 1] == ' ') step_len--;

    /* 1. Parse `init`: `T var = start` */
    const char *eq = memchr(init_s, '=', init_len);
    if (!eq) return false;
    size_t lhs_len = (size_t)(eq - init_s);
    while (lhs_len > 0 && init_s[lhs_len - 1] == ' ') lhs_len--;
    if (lhs_len == 0) return false;

    const char *start_s = eq + 1;
    size_t start_len = init_len - (size_t)(start_s - init_s);
    while (start_len > 0 && start_s[0] == ' ') { start_s++; start_len--; }
    if (start_len == 0) return false;

    size_t v_end = lhs_len;
    size_t v_start = v_end;
    while (v_start > 0 && is_ident_cont(init_s[v_start - 1])) v_start--;
    if (v_start == 0 || v_start == v_end) return false;

    const char *vname = init_s + v_start;
    size_t vlen = v_end - v_start;

    size_t type_len = v_start;
    while (type_len > 0 && init_s[type_len - 1] == ' ') type_len--;
    if (type_len == 0) return false;
    /* Do not convert pointer loops (`struct cal_month* m = ...`) into `range()` */
    if (init_s[type_len - 1] == '*') return false;

    /* 2. Parse `cond`: must start with `var < stop` or `var > stop` (without top-level `&&` or `||`) */
    if (cond_len <= vlen + 2 || memcmp(cond_s, vname, vlen) != 0) return false;
    for (size_t i = 0; i + 1 < cond_len; i++) {
        if ((cond_s[i] == '&' && cond_s[i + 1] == '&') ||
            (cond_s[i] == '|' && cond_s[i + 1] == '|')) {
            return false;
        }
    }
    size_t cp = vlen;
    while (cp < cond_len && cond_s[cp] == ' ') cp++;
    if (cp >= cond_len || (cond_s[cp] != '<' && cond_s[cp] != '>')) return false;
    char cmp_op = cond_s[cp++];
    if (cp < cond_len && cond_s[cp] == '=') return false; /* `<=` or `>=` not open-ended range(stop) */
    while (cp < cond_len && cond_s[cp] == ' ') cp++;
    const char *stop_s = cond_s + cp;
    size_t stop_len = cond_len - cp;
    if (stop_len == 0) return false;

    /* 3. Parse `step`: `var++`, `++var`, `var--`, `--var`, `var += s`, `var -= s` */
    const char *custom_step = NULL;
    size_t custom_step_len = 0;
    bool neg_step = false;

    if (step_len == vlen + 2 && memcmp(step_s, vname, vlen) == 0 &&
        step_s[vlen] == '+' && step_s[vlen + 1] == '+') {
        if (cmp_op != '<') return false;
    } else if (step_len == vlen + 2 && step_s[0] == '+' && step_s[1] == '+' &&
               memcmp(step_s + 2, vname, vlen) == 0) {
        if (cmp_op != '<') return false;
    } else if (step_len == vlen + 2 && memcmp(step_s, vname, vlen) == 0 &&
               step_s[vlen] == '-' && step_s[vlen + 1] == '-') {
        if (cmp_op != '>') return false;
        custom_step = "-1";
        custom_step_len = 2;
        neg_step = true;
    } else if (step_len > vlen + 2 && memcmp(step_s, vname, vlen) == 0) {
        size_t sp = vlen;
        while (sp < step_len && step_s[sp] == ' ') sp++;
        if (sp + 2 <= step_len && (step_s[sp] == '+' || step_s[sp] == '-') && step_s[sp + 1] == '=') {
            neg_step = (step_s[sp] == '-');
            if ((neg_step && cmp_op != '>') || (!neg_step && cmp_op != '<')) return false;
            sp += 2;
            while (sp < step_len && step_s[sp] == ' ') sp++;
            custom_step = step_s + sp;
            custom_step_len = step_len - sp;
            if (custom_step_len == 0) return false;
        } else {
            return false;
        }
    } else {
        return false;
    }

    /* Emit `vname` (and `: type` if not default `int` / `int32_t`) */
    pura_buf_append(out, vname, vlen);
    bool is_default_i32 = (type_len == 3 && memcmp(init_s, "int", 3) == 0) ||
                          (type_len == 7 && memcmp(init_s, "int32_t", 7) == 0);
    if (!is_default_i32) {
        pura_buf_puts(out, ": ");
        translate_type_c_to_pura(out, init_s, type_len);
    }
    pura_buf_puts(out, " in range(");
    bool start_is_zero = (start_len == 1 && start_s[0] == '0');
    if (start_is_zero && !custom_step) {
        translate_tokens_c_to_pura(out, stop_s, stop_len);
    } else {
        translate_tokens_c_to_pura(out, start_s, start_len);
        pura_buf_puts(out, ", ");
        translate_tokens_c_to_pura(out, stop_s, stop_len);
        if (custom_step) {
            pura_buf_puts(out, ", ");
            if (neg_step && custom_step[0] != '-') pura_buf_putc(out, '-');
            translate_tokens_c_to_pura(out, custom_step, custom_step_len);
        }
    }
    pura_buf_putc(out, ')');
    return true;
}

char *pura_from_c_indent(const char *src, size_t len, const char *indent_unit,
                           PuraError *err, size_t *out_len) {
    if (err) { err->message = NULL; err->line = 0; err->col = 0; }
    const char *unit = indent_unit ? indent_unit : "    ";
    size_t unit_len = 0;
    if (!valid_indent_unit(unit, &unit_len)) {
        if (err) { err->message = "invalid indent unit"; err->line = 1; err->col = 1; }
        return NULL;
    }

    PuraBuf out;
    if (!pura_buf_init(&out, len + 128)) return NULL;

    symtab_reset();

    size_t depth = 0;
    bool in_case_body = false;
    size_t case_switch_depth = 0;

    size_t p = 0;
    while (p < len) {
        size_t ls = p;
        while (p < len && src[p] != '\n' && src[p] != '\r') p++;
        size_t le = p;
        if (p < len) {
            if (src[p] == '\r' && p + 1 < len && src[p + 1] == '\n') p += 2;
            else p++;
        }

        /* Trim leading and trailing whitespace */
        while (ls < le && (src[ls] == ' ' || src[ls] == '\t')) ls++;
        while (le > ls && (src[le - 1] == ' ' || src[le - 1] == '\t')) le--;

        if (ls == le) {
            pura_buf_putc(&out, '\n');
            continue;
        }

        const char *line = src + ls;
        size_t llen = le - ls;

        /* Single-line comment `// ...` -> `# ...` */
        if (llen >= 2 && line[0] == '/' && line[1] == '/') {
            size_t eff_depth = depth + (in_case_body ? 1 : 0);
            pura_buf_indent(&out, unit, unit_len, eff_depth);
            pura_buf_putc(&out, '#');
            pura_buf_append(&out, line + 2, llen - 2);
            pura_buf_putc(&out, '\n');
            continue;
        }

        /* Preprocessor directives `#include`, `#define`, `#ifdef`, `#ifndef`, `#endif`, `#pragma` */
        if (line[0] == '#') {
            const char *d = line + 1;
            size_t dlen = llen - 1;
            while (dlen > 0 && d[0] == ' ') { d++; dlen--; }
            size_t kw_after = 0;
            if (starts_with_kw(d, dlen, "include", &kw_after)) {
                const char *inc = d + kw_after;
                size_t ilen = dlen - kw_after;
                pura_buf_puts(&out, "import ");
                if (ilen >= 2 && inc[0] == '<' && inc[ilen - 1] == '>') {
                    /* `#include <stdio.h>` -> `import stdio.h` */
                    pura_buf_append(&out, inc + 1, ilen - 2);
                } else if (ilen > 4 && inc[0] == '"' && inc[ilen - 1] == '"' &&
                           memcmp(inc + ilen - 3, ".h\"", 3) == 0) {
                    /* `#include "math_utils.h"` -> `import math_utils` (Pura .pu module) */
                    pura_buf_append(&out, inc + 1, ilen - 4);
                } else {
                    pura_buf_append(&out, inc, ilen);
                }
                pura_buf_putc(&out, '\n');
                continue;
            }
            if (starts_with_kw(d, dlen, "define", &kw_after)) {
                const char *m = d + kw_after;
                size_t mlen = dlen - kw_after;
                while (mlen > 0 && m[0] == ' ') { m++; mlen--; }

                /* Parse `NAME` or `NAME(args)` and optional value */
                size_t i = 0;
                while (i < mlen && is_ident_cont(m[i])) i++;
                if (i < mlen && m[i] == '(') {
                    int pdepth = 0;
                    while (i < mlen) {
                        if (m[i] == '(') pdepth++;
                        else if (m[i] == ')') {
                            pdepth--;
                            if (pdepth == 0) { i++; break; }
                        }
                        i++;
                    }
                }
                size_t sig_len = i;
                while (i < mlen && m[i] == ' ') i++;
                if (sig_len > 0 && i < mlen) {
                    /* `#define NAME value` -> `NAME = value` */
                    pura_buf_append(&out, m, sig_len);
                    pura_buf_puts(&out, " = ");
                    translate_tokens_c_to_pura(&out, m + i, mlen - i);
                } else {
                    /* Valueless `#define FLAG` -> `define FLAG` */
                    pura_buf_puts(&out, "define ");
                    translate_tokens_c_to_pura(&out, m, mlen);
                }
                pura_buf_putc(&out, '\n');
                continue;
            }
            if (starts_with_kw(d, dlen, "ifdef", &kw_after)) {
                pura_buf_puts(&out, "ifdef ");
                pura_buf_append(&out, d + kw_after, dlen - kw_after);
                pura_buf_putc(&out, '\n');
                continue;
            }
            if (starts_with_kw(d, dlen, "ifndef", &kw_after)) {
                pura_buf_puts(&out, "ifndef ");
                pura_buf_append(&out, d + kw_after, dlen - kw_after);
                pura_buf_putc(&out, '\n');
                continue;
            }
            if (starts_with_kw(d, dlen, "endif", &kw_after)) {
                pura_buf_puts(&out, "endif\n");
                continue;
            }
            if (starts_with_kw(d, dlen, "pragma", &kw_after)) {
                pura_buf_puts(&out, "pragma ");
                pura_buf_append(&out, d + kw_after, dlen - kw_after);
                pura_buf_putc(&out, '\n');
                continue;
            }
        }

        /* Split trailing `//` comment if present */
        const char *inline_comment = NULL;
        size_t code_len = llen;
        char quote = 0;
        for (size_t i = 0; i + 1 < llen; i++) {
            char c = line[i];
            if (quote) {
                if (c == '\\') i++;
                else if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '/' && line[i + 1] == '/') {
                inline_comment = line + i + 2;
                code_len = i;
                while (code_len > 0 && line[code_len - 1] == ' ') code_len--;
                break;
            }
        }

        /* Handle leading `}` (such as `}`, `};`, `} else if (...) {`, `} else {`, `} while (...);`) */
        if (code_len > 0 && line[0] == '}') {
            if (in_case_body && depth == case_switch_depth) {
                in_case_body = false;
            }
            if (depth > 0) depth--;

            const char *rest = line + 1;
            size_t rlen = code_len - 1;
            while (rlen > 0 && rest[0] == ' ') { rest++; rlen--; }
            if (rlen == 0 || (rlen == 1 && rest[0] == ';')) {
                continue;
            }
            line = rest;
            code_len = rlen;
        }

        /* Check if line is `case ...:` or `default:` */
        size_t kw_after = 0;
        if (starts_with_kw(line, code_len, "case", &kw_after) ||
            (code_len == 8 && memcmp(line, "default:", 8) == 0)) {
            in_case_body = false;
            pura_buf_indent(&out, unit, unit_len, depth);
            translate_tokens_c_to_pura(&out, line, code_len);
            if (inline_comment) {
                pura_buf_puts(&out, " #");
                pura_buf_append(&out, inline_comment, (size_t)(line + llen - inline_comment));
            }
            pura_buf_putc(&out, '\n');
            in_case_body = true;
            case_switch_depth = depth;
            continue;
        }

        size_t eff_depth = depth + (in_case_body ? 1 : 0);

        /* Check if line opens a block with `{` at end */
        if (code_len > 0 && line[code_len - 1] == '{') {
            size_t hlen = code_len - 1;
            while (hlen > 0 && line[hlen - 1] == ' ') hlen--;

            pura_buf_indent(&out, unit, unit_len, eff_depth);

            if (starts_with_kw(line, hlen, "else if", &kw_after)) {
                const char *cond = line + kw_after;
                size_t clen = hlen - kw_after;
                if (clen >= 2 && cond[0] == '(' && cond[clen - 1] == ')') {
                    cond++;
                    clen -= 2;
                }
                pura_buf_puts(&out, "elif ");
                translate_tokens_c_to_pura(&out, cond, clen);
                pura_buf_putc(&out, ':');
            } else if (hlen == 4 && memcmp(line, "else", 4) == 0) {
                pura_buf_puts(&out, "else:");
            } else if (starts_with_kw(line, hlen, "if", &kw_after)) {
                const char *cond = line + kw_after;
                size_t clen = hlen - kw_after;
                if (clen >= 2 && cond[0] == '(' && cond[clen - 1] == ')') {
                    cond++;
                    clen -= 2;
                }
                pura_buf_puts(&out, "if ");
                translate_tokens_c_to_pura(&out, cond, clen);
                pura_buf_putc(&out, ':');
            } else if (starts_with_kw(line, hlen, "while", &kw_after)) {
                const char *cond = line + kw_after;
                size_t clen = hlen - kw_after;
                if (clen >= 2 && cond[0] == '(' && cond[clen - 1] == ')') {
                    cond++;
                    clen -= 2;
                }
                pura_buf_puts(&out, "while ");
                translate_tokens_c_to_pura(&out, cond, clen);
                pura_buf_putc(&out, ':');
            } else if (starts_with_kw(line, hlen, "for", &kw_after)) {
                const char *cond = line + kw_after;
                size_t clen = hlen - kw_after;
                if (clen >= 2 && cond[0] == '(' && cond[clen - 1] == ')') {
                    cond++;
                    clen -= 2;
                }
                pura_buf_puts(&out, "for ");
                /* Split 3-clause C for `init; cond; step` */
                const char *semi1 = memchr(cond, ';', clen);
                const char *semi2 = semi1 ? memchr(semi1 + 1, ';', clen - (size_t)(semi1 + 1 - cond)) : NULL;
                if (semi1 && semi2) {
                    size_t init_len = (size_t)(semi1 - cond);
                    const char *c2 = semi1 + 1;
                    size_t c2_len = (size_t)(semi2 - c2);
                    const char *c3 = semi2 + 1;
                    size_t c3_len = clen - (size_t)(c3 - cond);
                    if (!try_emit_c_for_range_to_pura(&out, cond, init_len, c2, c2_len, c3, c3_len)) {
                        if (!try_emit_c_var_decl_to_pura(&out, cond, init_len)) {
                            translate_tokens_c_to_pura(&out, cond, init_len);
                        }
                        pura_buf_puts(&out, "; ");
                        while (c2_len > 0 && *c2 == ' ') { c2++; c2_len--; }
                        translate_tokens_c_to_pura(&out, c2, c2_len);
                        pura_buf_puts(&out, "; ");
                        while (c3_len > 0 && *c3 == ' ') { c3++; c3_len--; }
                        translate_tokens_c_to_pura(&out, c3, c3_len);
                    }
                } else {
                    translate_tokens_c_to_pura(&out, cond, clen);
                }
                pura_buf_putc(&out, ':');
            } else if (starts_with_kw(line, hlen, "switch", &kw_after)) {
                const char *cond = line + kw_after;
                size_t clen = hlen - kw_after;
                if (clen >= 2 && cond[0] == '(' && cond[clen - 1] == ')') {
                    cond++;
                    clen -= 2;
                }
                pura_buf_puts(&out, "switch ");
                translate_tokens_c_to_pura(&out, cond, clen);
                pura_buf_putc(&out, ':');
            } else if (!try_emit_c_func_to_pura(&out, line, hlen, true)) {
                size_t tag_after = 0;
                if (starts_with_kw(line, hlen, "struct", &tag_after)) {
                    size_t tlen = hlen - tag_after;
                    while (tlen > 0 && line[tag_after + tlen - 1] == ' ') tlen--;
                    symtab_add_tag("struct", line + tag_after, tlen);
                } else if (starts_with_kw(line, hlen, "enum", &tag_after)) {
                    size_t tlen = hlen - tag_after;
                    while (tlen > 0 && line[tag_after + tlen - 1] == ' ') tlen--;
                    symtab_add_tag("enum", line + tag_after, tlen);
                } else if (starts_with_kw(line, hlen, "union", &tag_after)) {
                    size_t tlen = hlen - tag_after;
                    while (tlen > 0 && line[tag_after + tlen - 1] == ' ') tlen--;
                    symtab_add_tag("union", line + tag_after, tlen);
                }
                translate_tokens_c_to_pura(&out, line, hlen);
                pura_buf_putc(&out, ':');
            }

            if (inline_comment) {
                pura_buf_puts(&out, " #");
                pura_buf_append(&out, inline_comment, (size_t)(line + llen - inline_comment));
            }
            pura_buf_putc(&out, '\n');
            depth++;
            continue;
        }

        /* Strip trailing `;` (or `,` inside enum) */
        if (code_len > 0 && line[code_len - 1] == ';') {
            code_len--;
            while (code_len > 0 && line[code_len - 1] == ' ') code_len--;
        }

        pura_buf_indent(&out, unit, unit_len, eff_depth);

        /* `typedef Target Alias` -> `type Alias = Target` */
        if (starts_with_kw(line, code_len, "typedef", &kw_after)) {
            const char *rest = line + kw_after;
            size_t rlen = code_len - kw_after;
            size_t id_end = rlen;
            size_t id_start = id_end;
            while (id_start > 0 && is_ident_cont(rest[id_start - 1])) id_start--;
            if (id_start > 0 && id_start < id_end) {
                size_t tend = id_start;
                while (tend > 0 && rest[tend - 1] == ' ') tend--;
                pura_buf_puts(&out, "type ");
                pura_buf_append(&out, rest + id_start, id_end - id_start);
                pura_buf_puts(&out, " = ");
                translate_tokens_c_to_pura(&out, rest, tend);
            } else {
                translate_tokens_c_to_pura(&out, line, code_len);
            }
        } else if (!try_emit_c_func_to_pura(&out, line, code_len, false) &&
                   !try_emit_c_var_decl_to_pura(&out, line, code_len)) {
            translate_tokens_c_to_pura(&out, line, code_len);
        }

        if (inline_comment) {
            pura_buf_puts(&out, " #");
            pura_buf_append(&out, inline_comment, (size_t)(line + llen - inline_comment));
        }
        pura_buf_putc(&out, '\n');
    }

    if (out_len) *out_len = out.len;
    return out.data;
}

char *pura_format_indent(const char *src, size_t len, const char *indent_unit,
                           PuraError *err, size_t *out_len) {
    size_t c_len = 0;
    char *c_code = pura_to_c(src, len, err, &c_len);
    if (!c_code) return NULL;
    char *st_code = pura_from_c_indent(c_code, c_len, indent_unit, err, out_len);
    free(c_code);
    return st_code;
}

/* -------------------------------------------------------------------------
 * Standalone CLI, Conformance Tests, and Head-to-Head Benchmark
 * ------------------------------------------------------------------------- */
#ifndef PURA_NO_MAIN

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    for (;;) {
        if (cap - len < 1024) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); fclose(fp); return NULL; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        len += n;
        if (n == 0) {
            if (ferror(fp)) { free(buf); fclose(fp); return NULL; }
            break;
        }
    }
    fclose(fp);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static double monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int run_tests(void) {
    const char *pura_cases[] = {
        "import stdio.h\n"
        "import stdint.h\n"
        "import math_utils\n"
        "MAX_ITEMS = 64\n"
        "MIN_VAL(a, b) = ((a) < (b) ? (a) : (b))\n"
        "\n"
        "struct Point:\n"
        "    x: f64\n"
        "    y: f64\n"
        "\n"
        "def move_x(p: addr[Point], dx: f64):\n"
        "    p.x += dx\n"
        "\n"
        "def clamp(val: i32, lo: i32, hi: i32) -> i32:\n"
        "    if val < lo:\n"
        "        return lo\n"
        "    elif val > hi:\n"
        "        return hi\n"
        "    else:\n"
        "        return val\n"
        "\n"
        "def main() -> i32:\n"
        "    pt: Point\n"
        "    pt.x = f64(1)\n"
        "    raw: any = pt.addr\n"
        "    move_x(cast(addr[Point], raw), 2.5)\n"
        "    total: i32 = 0\n"
        "    for i in range(10):\n"
        "        if i == 5 and not (total < 0):\n"
        "            total += clamp(i, 0, MAX_ITEMS)\n"
        "    printf(\"total=%d\\n\", total)\n"
        "    return 0\n",

        /* Tab-indented with switch/case, pointers, multi-level deref val(level, ptr), and enum */
        "import stdint.h\n"
        "enum Status:\n"
        "\tOK = 0\n"
        "\tERR = 1\n"
        "def check(argv: addr[cstr], out_code: addr[i32], code: i32) -> bool:\n"
        "\tif argv == nil or val(2, argv) == '\\0' or code != 0:\n"
        "\t\tval(out_code) = ERR\n"
        "\t\treturn false\n"
        "\tswitch code:\n"
        "\t\tcase 0:\n"
        "\t\t\tval(out_code) = OK\n"
        "\t\t\treturn true\n"
        "\t\tdefault:\n"
        "\t\t\treturn false\n"
    };

    const char *indents[] = {"  ", "    ", "\t"};
    size_t num_cases = sizeof(pura_cases) / sizeof(pura_cases[0]);
    size_t num_indents = sizeof(indents) / sizeof(indents[0]);
    size_t passed = 0;

    for (size_t i = 0; i < num_cases; i++) {
        for (size_t u = 0; u < num_indents; u++) {
            PuraError err;
            size_t c1_len = 0;
            char *c1 = pura_to_c(pura_cases[i], strlen(pura_cases[i]), &err, &c1_len);
            if (!c1) {
                fprintf(stderr, "FAIL [case %zu pura_to_c]: %s (line %zu)\n", i, err.message, err.line);
                return 1;
            }
            size_t st_len = 0;
            char *st = pura_from_c_indent(c1, c1_len, indents[u], &err, &st_len);
            if (!st) {
                fprintf(stderr, "FAIL [case %zu pura_from_c]: %s\n", i, err.message);
                free(c1);
                return 1;
            }
            size_t c2_len = 0;
            char *c2 = pura_to_c(st, st_len, &err, &c2_len);
            if (!c2) {
                fprintf(stderr, "FAIL [case %zu roundtrip pura_to_c]: %s (line %zu)\n", i, err.message, err.line);
                free(c1);
                free(st);
                return 1;
            }
            if (strcmp(c1, c2) != 0) {
                fprintf(stderr, "FAIL [case %zu roundtrip mismatch]\n--- Expected C ---\n%s--- Got C ---\n%s",
                        i, c1, c2);
                free(c1);
                free(st);
                free(c2);
                return 1;
            }
            free(c1);
            free(st);
            free(c2);
            passed++;
        }
    }

    /* Universal newlines test (\r\n and \r) */
    {
        const char *crlf_src = "def add(a: i32, b: i32) -> i32:\r\n    return a + b\r";
        PuraError err;
        char *c_out = pura_to_c(crlf_src, strlen(crlf_src), &err, NULL);
        if (!c_out) {
            fprintf(stderr, "FAIL [universal_newlines]: %s\n", err.message);
            return 1;
        }
        const char *expected = "int32_t add(int32_t a, int32_t b) {\n    return a + b;\n}\n";
        if (strcmp(c_out, expected) != 0) {
            fprintf(stderr, "FAIL [universal_newlines output]:\nGot:\n%sExpected:\n%s", c_out, expected);
            free(c_out);
            return 1;
        }
        free(c_out);
        passed++;
    }

    /* Invalid syntax tests */
    const char *invalid_cases[] = {
        "    x: i32 = 1\n",                        /* Unexpected root indent */
        "def foo():\n    x: i32 = 1\n  y: i32 = 2\n", /* Inconsistent indent multiple */
        "def foo():\n\tx: i32 = 1\n    y: i32 = 2\n", /* Mixed tab and space lines */
        "def foo():\n \tx: i32 = 1\n",             /* Mixed space+tab on same line */
        "def dangling():\n",                       /* Dangling block header at EOF */
        "def dangling():\nx: i32 = 1\n",           /* Missing indent after ':' */
        "x: i32 = 1   \n"                          /* Trailing whitespace */
    };
    size_t num_invalid = sizeof(invalid_cases) / sizeof(invalid_cases[0]);
    for (size_t i = 0; i < num_invalid; i++) {
        PuraError err;
        char *c_out = pura_to_c(invalid_cases[i], strlen(invalid_cases[i]), &err, NULL);
        if (c_out != NULL) {
            fprintf(stderr, "FAIL [invalid %zu accepted]: %s\n", i, invalid_cases[i]);
            free(c_out);
            return 1;
        }
        passed++;
    }

    printf("C Pura: all %zu conformance tests passed.\n", passed);
    return 0;
}

static int run_bench(const char *path) {
    char *st4_sample = NULL;
    size_t st4_len = 0;

    if (path) {
        st4_sample = read_file_all(path, &st4_len);
        if (!st4_sample) {
            fprintf(stderr, "pura error: cannot read '%s'\n", path);
            return 1;
        }
    } else {
        PuraBuf b;
        pura_buf_init(&b, 65536);
        pura_buf_puts(&b,
            "import stdint.h\n"
            "import stdbool.h\n"
            "import stddef.h\n\n"
            "struct Node:\n"
            "    id: i32\n"
            "    weight: f64\n"
            "    active: bool\n"
            "    label: cstr\n\n"
        );
        for (int i = 0; i < 250; i++) {
            char fn[512];
            int n = snprintf(fn, sizeof(fn),
                "def compute_layer_%d(n: addr[Node], count: usize, threshold: f64) -> f64:\n"
                "    acc: f64 = 0.0\n"
                "    for i: usize in range(count):\n"
                "        if n[i].active and n[i].weight > threshold:\n"
                "            acc += n[i].weight * 1.25\n"
                "        elif not n[i].active or n[i].id == %d:\n"
                "            acc -= 0.5\n"
                "        else:\n"
                "            acc += 0.1\n"
                "    return acc\n\n",
                i, i
            );
            pura_buf_append(&b, fn, (size_t)n);
        }
        st4_sample = b.data;
        st4_len = b.len;
    }

    PuraError err;
    size_t c_len = 0;
    char *c_sample = pura_to_c(st4_sample, st4_len, &err, &c_len);
    if (!c_sample) {
        fprintf(stderr, "pura bench error: %s (line %zu)\n", err.message, err.line);
        free(st4_sample);
        return 1;
    }
    size_t st_tab_len = 0;
    char *st_tab_sample = pura_from_c_indent(c_sample, c_len, "\t", &err, &st_tab_len);

    int iters = 1000;
    double t0 = monotonic_sec();
    for (int i = 0; i < iters; i++) {
        size_t out_l = 0;
        char *r = pura_to_c(st4_sample, st4_len, &err, &out_l);
        free(r);
    }
    double dt_st4 = monotonic_sec() - t0;

    t0 = monotonic_sec();
    for (int i = 0; i < iters; i++) {
        size_t out_l = 0;
        char *r = pura_to_c(st_tab_sample, st_tab_len, &err, &out_l);
        free(r);
    }
    double dt_tab = monotonic_sec() - t0;

    t0 = monotonic_sec();
    for (int i = 0; i < iters; i++) {
        size_t out_l = 0;
        char *r = pura_from_c_indent(c_sample, c_len, "    ", &err, &out_l);
        free(r);
    }
    double dt_from_c = monotonic_sec() - t0;

    double mib_st4 = ((double)st4_len * iters / (1024.0 * 1024.0)) / dt_st4;
    double mib_tab = ((double)st_tab_len * iters / (1024.0 * 1024.0)) / dt_tab;
    double mib_from_c = ((double)c_len * iters / (1024.0 * 1024.0)) / dt_from_c;

    printf("Pura Benchmark (250 functions, %d iterations):\n", iters);
    printf("  C source size          : %zu bytes (%.1f KiB)\n", c_len, (double)c_len / 1024.0);
    printf("  Pura 4-space size    : %zu bytes (%.1f KiB, %+.1f%% vs C)\n",
           st4_len, (double)st4_len / 1024.0, 100.0 * ((double)st4_len - (double)c_len) / (double)c_len);
    printf("  Pura tab (\\t) size   : %zu bytes (%.1f KiB, %+.1f%% vs C)\n",
           st_tab_len, (double)st_tab_len / 1024.0, 100.0 * ((double)st_tab_len - (double)c_len) / (double)c_len);
    printf("  Pura (4-sp) -> C     : %.0f files/s (%.1f MiB/s)\n", (double)iters / dt_st4, mib_st4);
    printf("  Pura (tab)  -> C     : %.0f files/s (%.1f MiB/s)\n", (double)iters / dt_tab, mib_tab);
    printf("  C -> Pura (4-sp)     : %.0f files/s (%.1f MiB/s)\n", (double)iters / dt_from_c, mib_from_c);

    free(st4_sample);
    free(c_sample);
    free(st_tab_sample);
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <file.pu>                         Transpile Pura to C/C++ on stdout\n"
        "  %s --pura [--indent <N|tab>] <f>   Format/canonicalize Pura\n"
        "  %s --from-c [--indent <N|tab>] <f>   Convert C/C++ source to Pura\n"
        "  %s --compile <file.pu> [-o <bin>]    Transpile and compile via cc/c++\n"
        "  %s --run <file.pu>                   Transpile, compile, and execute immediately\n"
        "  %s --check <file.pu>                 Validate Pura syntax only\n"
        "  %s --test                            Run built-in conformance test suite\n"
        "  %s --bench [file.pu]                 Run transpilation throughput benchmark\n",
        prog, prog, prog, prog, prog, prog, prog, prog
    );
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    bool mode_pura = false;
    bool mode_from_c = false;
    bool mode_check = false;
    bool mode_compile = false;
    bool mode_run = false;
    const char *indent_opt = "    ";
    char custom_spaces[64];
    const char *input_path = NULL;
    const char *output_bin = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            return run_tests();
        } else if (strcmp(argv[i], "--bench") == 0) {
            const char *bpath = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[i + 1] : NULL;
            return run_bench(bpath);
        } else if (strcmp(argv[i], "--pura") == 0) {
            mode_pura = true;
        } else if (strcmp(argv[i], "--from-c") == 0) {
            mode_from_c = true;
        } else if (strcmp(argv[i], "--check") == 0) {
            mode_check = true;
        } else if (strcmp(argv[i], "--compile") == 0) {
            mode_compile = true;
        } else if (strcmp(argv[i], "--run") == 0) {
            mode_run = true;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_bin = argv[++i];
        } else if (strcmp(argv[i], "--indent") == 0 && i + 1 < argc) {
            const char *arg = argv[++i];
            if (strcmp(arg, "tab") == 0 || strcmp(arg, "\\t") == 0) {
                indent_opt = "\t";
            } else {
                int n = atoi(arg);
                if (n <= 0 || n >= (int)sizeof(custom_spaces)) {
                    fprintf(stderr, "pura error: --indent must be 1..63 or 'tab'\n");
                    return 1;
                }
                memset(custom_spaces, ' ', (size_t)n);
                custom_spaces[n] = '\0';
                indent_opt = custom_spaces;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        } else {
            fprintf(stderr, "pura error: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!input_path) {
        fprintf(stderr, "pura error: missing input file\n");
        return 1;
    }

    size_t src_len = 0;
    char *src = read_file_all(input_path, &src_len);
    if (!src) {
        fprintf(stderr, "pura error: cannot read '%s'\n", input_path);
        return 1;
    }

    PuraError err;
    if (mode_from_c) {
        size_t out_len = 0;
        char *st = pura_from_c_indent(src, src_len, indent_opt, &err, &out_len);
        free(src);
        if (!st) {
            fprintf(stderr, "pura error:%zu:%zu: %s\n", err.line, err.col, err.message);
            return 1;
        }
        fwrite(st, 1, out_len, stdout);
        free(st);
        return 0;
    }

    if (mode_pura) {
        size_t out_len = 0;
        char *st = pura_format_indent(src, src_len, indent_opt, &err, &out_len);
        free(src);
        if (!st) {
            fprintf(stderr, "pura error:%zu:%zu: %s\n", err.line, err.col, err.message);
            return 1;
        }
        fwrite(st, 1, out_len, stdout);
        free(st);
        return 0;
    }

    size_t c_len = 0;
    char *c_code = pura_to_c(src, src_len, &err, &c_len);
    free(src);
    if (!c_code) {
        fprintf(stderr, "pura error:%zu:%zu: %s\n", err.line, err.col, err.message);
        return 1;
    }

    if (mode_check) {
        free(c_code);
        return 0;
    }

    if (mode_compile || mode_run) {
        char tmp_c[256];
        snprintf(tmp_c, sizeof(tmp_c), "/tmp/pura_%d.c", (int)getpid());
        FILE *fp = fopen(tmp_c, "wb");
        if (!fp) {
            fprintf(stderr, "pura error: failed to create temporary C file\n");
            free(c_code);
            return 1;
        }
        fwrite(c_code, 1, c_len, fp);
        fclose(fp);
        free(c_code);

        char bin_path[256];
        if (output_bin) {
            snprintf(bin_path, sizeof(bin_path), "%s", output_bin);
        } else if (mode_run) {
            snprintf(bin_path, sizeof(bin_path), "/tmp/pura_bin_%d", (int)getpid());
        } else {
            snprintf(bin_path, sizeof(bin_path), "a.out");
        }

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cc -O2 -std=c11 %s -o %s", tmp_c, bin_path);
        int rc = system(cmd);
        unlink(tmp_c);
        if (rc != 0) return 1;

        if (mode_run) {
            rc = system(bin_path);
            if (!output_bin) unlink(bin_path);
            return rc == 0 ? 0 : 1;
        }
        return 0;
    }

    fwrite(c_code, 1, c_len, stdout);
    free(c_code);
    return 0;
}
#endif /* PURA_NO_MAIN */
