#ifndef BC_FWL_H
#define BC_FWL_H

#include <stddef.h>

/*
 * FWL, the Flow Writeup Language: an agent's FLOW.fwl. Plain English sentences, one per
 * line, inside a little structure:
 *
 *   IF <condition>:  /  ELSE IF <condition>:  /  ELSE:
 *   FOR EACH {item} in {list}:
 *   WHILE <condition>, at most N times:
 *   RETRY N times:
 *   ASK "question" [choice, choice] into {name}
 *   STEP n: NAME            a label, shown as progress
 *   RETURN <what the user gets>
 *
 * A line ending in ':' opens a block; the lines indented under it are the block. Only these
 * words are in CAPITALS. Every other line is a sentence the model carries out. Sentences and
 * conditions name things with marks: $tool, #skill, @agent, and {value} for something
 * remembered: the first sentence that mentions a new {value} fills it.
 *
 * This file is the shared front end: sq_parse() builds the tree, reports syntax errors and
 * works out which sentence fills which value; sq_check() checks the marks against what the
 * agent has (packer, at build time); fwl_run.c runs the tree (botcore, at run time).
 */

enum {
    SQ_INSTR = 1, /* a sentence                           a = text, pk[] = the values it fills */
    SQ_STEP,      /* STEP n: NAME                         a = NAME, n = n */
    SQ_ASK,       /* ASK <a> [pk...] into {b}             pk[] = choices, b = value name ("answer" if none) */
    SQ_IF,        /* IF / ELSE IF / ELSE                  br[] (cond NULL = ELSE) */
    SQ_FOR,       /* FOR EACH {a} in <b>:                 */
    SQ_WHILE,     /* WHILE <a>, at most n times:          n = bound (0 = none) */
    SQ_RETRY,     /* RETRY n times:                       */
    SQ_RETURN,    /* RETURN <a>                           */
};

typedef struct sq_node sq_node;

typedef struct {
    sq_node **v;
    int       n, cap;
} sq_list;

typedef struct {
    char   *cond; /* NULL = ELSE */
    int     line;
    sq_list body;
} sq_branch;

struct sq_node {
    int        kind, line;
    char      *a, *b;
    int        n;
    char     **pk; /* names: values a sentence fills, or the choices of an ASK */
    int        np;
    sq_list    body;
    sq_branch *br;
    int        nbr;
};

typedef struct {
    sq_list top;
    char    title[200];
    int     errors, warnings;
    /* diagnostics: line, 1 = error / 0 = warning, message */
    void (*diag)(void *ud, int line, int error, const char *msg);
    void *ud;
} sq_prog;

/* Parse FLOW.fwl text. Errors go to p->diag; returns the error count. */
int  sq_parse(const char *src, size_t n, sq_prog *p);
void sq_free(sq_prog *p);

/* Build-time: every $tool, #skill and @agent a line names must exist. */
void sq_check(sq_prog *p, int (*has_tool)(const char *), int (*has_skill)(const char *), int (*has_agent)(const char *));

/* Length of the {name.field[n]} value reference at c (braces included), or 0 if c is not one. */
size_t sq_val(const char *c);
/* Reference syntax (without the braces): identifier then .field / [n] parts; root name into root[65]. */
int  sq_ref(const char *s, size_t n, char root[65]);
/* The names with this mark ('$' tool, '#' skill, '@' agent) in text, outside "strings". Returns how many (<= max). */
int  sq_marks(const char *text, char mark, char out[][65], int max);
/* Expression made only of "strings", {values}, numbers, true/false/null and + (no prose). */
int  sq_is_literal(const char *expr);

#endif
