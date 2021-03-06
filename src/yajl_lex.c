/*
 * Copyright (c) 2007-2014, Lloyd Hilaiel <me@lloyd.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "yajl_lex.h"
#include "yajl_buf.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#ifdef YAJL_LEXER_DEBUG
static const char *
tokToStr(yajl_tok tok)
{
    switch (tok) {
        case yajl_tok_bool: return "bool";
        case yajl_tok_colon: return "colon";
        case yajl_tok_comma: return "comma";
        case yajl_tok_eof: return "eof";
        case yajl_tok_error: return "error";
        case yajl_tok_left_brace: return "brace";
        case yajl_tok_left_bracket: return "bracket";
        case yajl_tok_null: return "null";
        case yajl_tok_integer: return "integer";
        case yajl_tok_double: return "double";
        case yajl_tok_right_brace: return "brace";
        case yajl_tok_right_bracket: return "bracket";
        case yajl_tok_string: return "string";
        case yajl_tok_string_with_escapes: return "string_with_escapes";
    }
    return "unknown";
}
#endif

/* Impact of the stream parsing feature on the lexer:
 *
 * YAJL support stream parsing.  That is, the ability to parse the first
 * bits of a chunk of JSON before the last bits are available (still on
 * the network or disk).  This makes the lexer more complex.  The
 * responsibility of the lexer is to handle transparently the case where
 * a chunk boundary falls in the middle of a token.  This is
 * accomplished is via a buffer and a character reading abstraction.
 *
 * Overview of implementation
 *
 * When we lex to end of input string before end of token is hit, we
 * copy all of the input text composing the token into our lexBuf.
 *
 * Every time we read a character, we first check for end of input, and if
 * so we return yajl_tok_eof after setting up some state so we can continue.
 * The outer lexer routine is responsible for the lexBuf copying and trying
 * to avoid the lexBuf copy in the common case where no end of input seen.
 * It is also responsible for resetting stale states after a complete token.
 *
 * There are 3 levels: state (an enum), substate and subsubstate (integers).
 * This roughly corresponds to the amount of subroutine nesting in the lexer,
 * but an extra level of state can be used as a loop counter instead of a
 * subroutine state where necessary (in the expect and the uNNNN routines).
 *
 * Parsing the state is done on entry to the subroutine using switch / goto.
 * A convenience macro CHECK_EOF() is provided, similar to the macros used
 * when it was done the old way (by re-scanning the text instead of by state),
 * it sets the given state variable on EOF, and defines a label for re-entry.
 *
 * Be careful that the subroutines don't rely on any local variable state.
 */

typedef enum {
    state_start,
    state_expect,
    state_string,
    state_number,
    state_comment
} yajl_lex_state;

#define CHECK_EOF(state, n) \
if (*offset >= jsonTextLen) { \
    (state) = n; \
    return yajl_tok_eof; \
entry_##n: \
    ; \
}

struct yajl_lexer_t {
    /* the overal line and char offset into the data */
    size_t lineOff;
    size_t charOff;

    /* error */
    yajl_lex_error error;

    /* a input buffer to handle the case where a token is spread over
     * multiple chunks */
    yajl_buf buf;

    /* instead of former bufInUse flag, remember what we did so far */
    yajl_lex_state state;
    unsigned int substate;
    unsigned int subsubstate;
    yajl_tok resultTok; /* optional scratch area to track tok to return */

    /* shall we allow comments? */
    unsigned int allowComments;

    /* shall we validate utf8 inside strings? */
    unsigned int validateUTF8;

    yajl_alloc_funcs * alloc;
};

#define readChar(lxr, txt, off) ((txt)[(*(off))++])
#define unreadChar(lxr, off) ((*(off))--)

yajl_lexer
yajl_lex_alloc(yajl_alloc_funcs * alloc,
               unsigned int allowComments, unsigned int validateUTF8)
{
    yajl_lexer lxr = (yajl_lexer) YA_MALLOC(alloc, sizeof(struct yajl_lexer_t));
    memset((void *) lxr, 0, sizeof(struct yajl_lexer_t));
    lxr->buf = yajl_buf_alloc(alloc);
    lxr->allowComments = allowComments;
    lxr->validateUTF8 = validateUTF8;
    lxr->alloc = alloc;
    return lxr;
}

void
yajl_lex_reset(yajl_lexer lxr)
{
    lxr->error = yajl_lex_e_ok;
    lxr->state = state_start;
}

void
yajl_lex_free(yajl_lexer lxr)
{
    yajl_buf_free(lxr->buf);
    YA_FREE(lxr->alloc, lxr);
    return;
}

/* a lookup table which lets us quickly determine three things:
 * VEC - valid escaped control char
 * note.  the solidus '/' may be escaped or not.
 * IJC - invalid json char
 * VHC - valid hex char
 * NFP - needs further processing (from a string scanning perspective)
 * NUC - needs utf8 checking when enabled (from a string scanning perspective)
 */
#define VEC 0x01
#define IJC 0x02
#define VHC 0x04
#define NFP 0x08
#define NUC 0x10

static const char charLookupTable[256] =
{
/*00*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,
/*08*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,
/*10*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,
/*18*/ IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    , IJC    ,

/*20*/ 0      , 0      , NFP|VEC|IJC, 0      , 0      , 0      , 0      , 0      ,
/*28*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , VEC    ,
/*30*/ VHC    , VHC    , VHC    , VHC    , VHC    , VHC    , VHC    , VHC    ,
/*38*/ VHC    , VHC    , 0      , 0      , 0      , 0      , 0      , 0      ,

/*40*/ 0      , VHC    , VHC    , VHC    , VHC    , VHC    , VHC    , 0      ,
/*48*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0      ,
/*50*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0      ,
/*58*/ 0      , 0      , 0      , 0      , NFP|VEC|IJC, 0      , 0      , 0      ,

/*60*/ 0      , VHC    , VEC|VHC, VHC    , VHC    , VHC    , VEC|VHC, 0      ,
/*68*/ 0      , 0      , 0      , 0      , 0      , 0      , VEC    , 0      ,
/*70*/ 0      , 0      , VEC    , 0      , VEC    , 0      , 0      , 0      ,
/*78*/ 0      , 0      , 0      , 0      , 0      , 0      , 0      , 0      ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,

       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    ,
       NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC    , NUC
};

/** process a variable length utf8 encoded codepoint.
 *
 *  returns:
 *    yajl_tok_string - if valid utf8 char was parsed and offset was
 *                      advanced
 *    yajl_tok_eof - if end of input was hit before validation could
 *                   complete
 *    yajl_tok_error - if invalid utf8 was encountered
 *
 *  NOTE: on error the offset will point to the first char of the
 *  invalid utf8 */

static yajl_tok
yajl_lex_utf8_char(yajl_lexer lexer, const unsigned char * jsonText,
                   size_t jsonTextLen, size_t * offset,
                   unsigned char curChar)
{
    switch (lexer->subsubstate) {
    case 0:
        break;
    case 1:
        goto entry_1;
    case 2:
        goto entry_2;
    case 3:
        goto entry_3;
    case 4:
        goto entry_4;
    case 5:
        goto entry_5;
    case 6:
        goto entry_6;
    default:
        assert(0);
    }

    if (curChar <= 0x7f) {
        /* single byte */
        return yajl_tok_string;
    } else if ((curChar >> 5) == 0x6) {
        /* two byte */
        CHECK_EOF(lexer->subsubstate, 1);
        curChar = readChar(lexer, jsonText, offset);
        if ((curChar >> 6) == 0x2) return yajl_tok_string;
    } else if ((curChar >> 4) == 0x0e) {
        /* three byte */
        CHECK_EOF(lexer->subsubstate, 2);
        curChar = readChar(lexer, jsonText, offset);
        if ((curChar >> 6) == 0x2) {
            CHECK_EOF(lexer->subsubstate, 3);
            curChar = readChar(lexer, jsonText, offset);
            if ((curChar >> 6) == 0x2) return yajl_tok_string;
        }
    } else if ((curChar >> 3) == 0x1e) {
        /* four byte */
        CHECK_EOF(lexer->subsubstate, 4);
        curChar = readChar(lexer, jsonText, offset);
        if ((curChar >> 6) == 0x2) {
            CHECK_EOF(lexer->subsubstate, 5);
            curChar = readChar(lexer, jsonText, offset);
            if ((curChar >> 6) == 0x2) {
                CHECK_EOF(lexer->subsubstate, 6);
                curChar = readChar(lexer, jsonText, offset);
                if ((curChar >> 6) == 0x2) return yajl_tok_string;
            }
        }
    }

    lexer->error = yajl_lex_string_invalid_utf8;
    return yajl_tok_error;
}

/* lex a string.  input is the lexer, pointer to beginning of
 * json text, and start of string (offset).
 * a token is returned which has the following meanings:
 * yajl_tok_string: lex of string was successful.  offset points to
 *                  terminating '"'.
 * yajl_tok_eof: end of text was encountered before we could complete
 *               the lex.
 * yajl_tok_error: embedded in the string were unallowable chars.  offset
 *               points to the offending char
 */

/** scan a string for interesting characters that might need further
 *  review.  return the number of chars that are uninteresting and can
 *  be skipped.
 * (lth) hi world, any thoughts on how to make this routine faster? */
static size_t
yajl_string_scan(const unsigned char * buf, size_t len, int utf8check)
{
    unsigned char mask = IJC|NFP|(utf8check ? NUC : 0);
    size_t skip = 0;
    for (; skip < len && !(charLookupTable[buf[skip]] & mask); skip++)
        ;
    return skip;
}

static yajl_tok
yajl_lex_string(yajl_lexer lexer, const unsigned char * jsonText,
                size_t jsonTextLen, size_t * offset)
{
    /* compiler isn't smart enough to figure out that yajl_lex_utf8_char()
     * will not access curChar when we jump in from case 4 below, so we'll
     * just initialize it here, it would be better to unread the first char
     * of the UTF-8 sequence and let yajl_lex_utf8_char() read it by itself
     */
    unsigned char curChar = 0;

    switch (lexer->substate) {
    case 0:
        break;
    case 1:
        goto entry_1;
    case 2:
        goto entry_2;
    case 3:
        goto entry_3;
    case 4:
        goto entry_utf8_char;
    default:
        assert(0);
    }
    lexer->resultTok = yajl_tok_string;

    for (;;) {
        /* now jump into a faster scanning routine to skip as much
         * of the buffers as possible */
        {
            const unsigned char * p;
            size_t len;

            if (*offset < jsonTextLen)
            {
                p = jsonText + *offset;
                len = jsonTextLen - *offset;
                *offset += yajl_string_scan(p, len, lexer->validateUTF8);
            }
        }

        CHECK_EOF(lexer->substate, 1);
        curChar = readChar(lexer, jsonText, offset);

        /* quote terminates */
        if (curChar == '"') {
            return lexer->resultTok; /* string with or without escapes */
        }
        /* backslash escapes a set of control chars, */
        else if (curChar == '\\') {
            lexer->resultTok = yajl_tok_string_with_escapes;

            /* special case \u */
            CHECK_EOF(lexer->substate, 2);
            curChar = readChar(lexer, jsonText, offset);
            if (curChar == 'u') {
                for (
                    lexer->subsubstate = 0;
                    lexer->subsubstate < 4;
                    lexer->subsubstate++
                ) {
                    CHECK_EOF(lexer->substate, 3);
                    curChar = readChar(lexer, jsonText, offset);
                    if (!(charLookupTable[curChar] & VHC)) {
                        /* back up to offending char */
                        unreadChar(lexer, offset);
                        lexer->error = yajl_lex_string_invalid_hex_char;
                        return yajl_tok_error;
                    }
                }
            } else if (!(charLookupTable[curChar] & VEC)) {
                /* back up to offending char */
                unreadChar(lexer, offset);
                lexer->error = yajl_lex_string_invalid_escaped_char;
                return yajl_tok_error;
            }
        }
        /* when not validating UTF8 it's a simple table lookup to determine
         * if the present character is invalid */
        else if(charLookupTable[curChar] & IJC) {
            /* back up to offending char */
            unreadChar(lexer, offset);
            lexer->error = yajl_lex_string_invalid_json_char;
            return yajl_tok_error;
        }
        /* when in validate UTF8 mode we need to do some extra work */
        else if (lexer->validateUTF8) {
            lexer->substate = 4;
            lexer->subsubstate = 0;
        entry_utf8_char:
            ;
            yajl_tok t = yajl_lex_utf8_char(lexer, jsonText, jsonTextLen,
                                            offset, curChar);
            if (t != yajl_tok_string) {
                return t;
            }
        }
        /* accept it, and move on */
    }
}

static yajl_tok
yajl_lex_number(yajl_lexer lexer, const unsigned char * jsonText,
                size_t jsonTextLen, size_t * offset)
{
    /** XXX: numbers are the only entities in json that we must lex
     *       _beyond_ in order to know that they are complete.  There
     *       is an ambiguous case for integers at EOF. */

    unsigned char c;

    switch (lexer->substate) {
    case 0:
        break;
    case 1:
        goto entry_1;
    case 2:
        goto entry_2;
    case 3:
        goto entry_3;
    case 4:
        goto entry_4;
    case 5:
        goto entry_5;
    case 6:
        goto entry_6;
    case 7:
        goto entry_7;
    case 8:
        goto entry_8;
    case 9:
        goto entry_9;
    default:
        assert(0);
    }
    lexer->resultTok = yajl_tok_integer;

    CHECK_EOF(lexer->substate, 1);
    c = readChar(lexer, jsonText, offset);

    /* optional leading minus */
    if (c == '-') {
        CHECK_EOF(lexer->substate, 2);
        c = readChar(lexer, jsonText, offset);
    }

    /* a single zero, or a series of integers */
    if (c == '0') {
        CHECK_EOF(lexer->substate, 3);
        c = readChar(lexer, jsonText, offset);
        if (c >= '0' && c <= '9') {
            unreadChar(lexer, offset);
            lexer->error = yajl_lex_leading_zeros;
            return yajl_tok_error;
        }
    } else if (c >= '1' && c <= '9') {
        do {
            CHECK_EOF(lexer->substate, 4);
            c = readChar(lexer, jsonText, offset);
        } while (c >= '0' && c <= '9');
    } else {
        unreadChar(lexer, offset);
        lexer->error = yajl_lex_missing_integer_after_minus;
        return yajl_tok_error;
    }

    /* optional fraction (indicates this is floating point) */
    if (c == '.') {
        CHECK_EOF(lexer->substate, 5);
        c = readChar(lexer, jsonText, offset);
        if (c < '0' || c > '9') {
            unreadChar(lexer, offset);
            lexer->error = yajl_lex_missing_integer_after_decimal;
            return yajl_tok_error;
        }

        do {
            CHECK_EOF(lexer->substate, 6);
            c = readChar(lexer, jsonText, offset);
        } while (c >= '0' && c <= '9');
        lexer->resultTok = yajl_tok_double;
    }

    /* optional exponent (indicates this is floating point) */
    if (c == 'e' || c == 'E') {
        CHECK_EOF(lexer->substate, 7);
        c = readChar(lexer, jsonText, offset);

        /* optional sign */
        if (c == '+' || c == '-') {
            CHECK_EOF(lexer->substate, 8);
            c = readChar(lexer, jsonText, offset);
        }

        if (c >= '0' && c <= '9') {
            do {
                CHECK_EOF(lexer->substate, 9);
                c = readChar(lexer, jsonText, offset);
            } while (c >= '0' && c <= '9');
        } else {
            unreadChar(lexer, offset);
            lexer->error = yajl_lex_missing_integer_after_exponent;
            return yajl_tok_error;
        }
        lexer->resultTok = yajl_tok_double;
    }

    /* we always go "one too far" */
    unreadChar(lexer, offset);

    return lexer->resultTok; /* integer or double */
}

static yajl_tok
yajl_lex_comment(yajl_lexer lexer, const unsigned char * jsonText,
                 size_t jsonTextLen, size_t * offset)
{
    unsigned char c;

    switch (lexer->substate) {
    case 0:
        break;
    case 1:
        goto entry_1;
    case 2:
        goto entry_2;
    case 3:
        goto entry_3;
    case 4:
        goto entry_4;
    default:
        assert(0);
    }

    CHECK_EOF(lexer->substate, 1);
    c = readChar(lexer, jsonText, offset);

    /* either slash or star expected */
    if (c == '/') {
        /* now we throw away until end of line */
        do {
            CHECK_EOF(lexer->substate, 2);
            c = readChar(lexer, jsonText, offset);
        } while (c != '\n');
    } else if (c == '*') {
        /* now we throw away until end of comment */
        for (;;) {
            CHECK_EOF(lexer->substate, 3);
            c = readChar(lexer, jsonText, offset);
            if (c == '*') {
                CHECK_EOF(lexer->substate, 4);
                c = readChar(lexer, jsonText, offset);
                if (c == '/') {
                    break;
                } else {
                    unreadChar(lexer, offset);
                }
            }
        }
    } else {
        lexer->error = yajl_lex_invalid_char;
        return yajl_tok_error;
    }

    return yajl_tok_comment;
}

yajl_tok
yajl_lex_lex(yajl_lexer lexer, const unsigned char * jsonText,
             size_t jsonTextLen, size_t * offset,
             const unsigned char ** outBuf, size_t * outLen)
{
    yajl_tok tok = yajl_tok_error;
    unsigned char c;
    size_t startOffset = *offset;
    static unsigned char expect[] = "rue\0alse\0ull";

    *outBuf = NULL;
    *outLen = 0;

    /* if entryState != state_start then buffer is in use */
    yajl_lex_state entryState = lexer->state;
    switch (entryState) {
    case state_start:
        break;
    case state_expect:
        goto entry_expect;
    case state_string:
        goto entry_string;
    case state_number:
        goto entry_number;
    case state_comment:
        goto entry_comment;
    default:
        assert(0);
    }

    yajl_buf_clear(lexer->buf);
    for (;;) {
        assert(*offset <= jsonTextLen);

        if (*offset >= jsonTextLen) {
            tok = yajl_tok_eof;
            goto lexed;
        }

        c = readChar(lexer, jsonText, offset);

        switch (c) {
            case '{':
                tok = yajl_tok_left_bracket;
                goto lexed;
            case '}':
                tok = yajl_tok_right_bracket;
                goto lexed;
            case '[':
                tok = yajl_tok_left_brace;
                goto lexed;
            case ']':
                tok = yajl_tok_right_brace;
                goto lexed;
            case ',':
                tok = yajl_tok_comma;
                goto lexed;
            case ':':
                tok = yajl_tok_colon;
                goto lexed;
            case '\t': case '\n': case '\v': case '\f': case '\r': case ' ':
                startOffset++;
                break;
            case 't':
                lexer->state = state_expect;
                lexer->substate = 0; /* offset into expect[] string above */
                lexer->resultTok = yajl_tok_bool;
                goto entry_expect;
            case 'f':
                lexer->state = state_expect;
                lexer->substate = 4; /* offset into expect[] string above */
                lexer->resultTok = yajl_tok_bool;
                goto entry_expect;
            case 'n':
                lexer->state = state_expect;
                lexer->substate = 9; /* offset into expect[] string above */
                lexer->resultTok = yajl_tok_null;
            entry_expect:
                do {
                    if (*offset >= jsonTextLen) {
                        tok = yajl_tok_eof;
                        goto lexed;
                    }
                    c = readChar(lexer, jsonText, offset);
                    if (c != expect[lexer->substate]) {
                        unreadChar(lexer, offset);
                        lexer->error = yajl_lex_invalid_string;
                        tok = yajl_tok_error;
                        goto lexed;
                    }
                } while (expect[++lexer->substate]);
                tok = lexer->resultTok;
                goto lexed;
            case '"': {
                lexer->state = state_string;
                lexer->substate = 0;
            entry_string:
                tok = yajl_lex_string(lexer, jsonText,
                                      jsonTextLen, offset);
                goto lexed;
            }
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9': {
                /* integer parsing wants to start from the beginning */
                unreadChar(lexer, offset);
                lexer->state = state_number;
                lexer->substate = 0;
            entry_number:
                tok = yajl_lex_number(lexer, jsonText,
                                      jsonTextLen, offset);
                goto lexed;
            }
            case '/':
                /* hey, look, a probable comment!  If comments are disabled
                 * it's an error. */
                if (!lexer->allowComments) {
                    unreadChar(lexer, offset);
                    lexer->error = yajl_lex_unallowed_comment;
                    tok = yajl_tok_error;
                    goto lexed;
                }
                /* if comments are enabled, then we should try to lex
                 * the thing.  possible outcomes are
                 * - successful lex (tok_comment, which means continue),
                 * - malformed comment opening (slash not followed by
                 *   '*' or '/') (tok_error)
                 * - eof hit. (tok_eof) */
                lexer->state = state_comment;
                lexer->substate = 0;
            entry_comment:
                tok = yajl_lex_comment(lexer, jsonText,
                                       jsonTextLen, offset);
                if (tok == yajl_tok_comment) {
                    /* behave as if we had returned a token then re-entered */
                    tok = yajl_tok_error;
                    yajl_buf_clear(lexer->buf);
                    lexer->state = state_start;
                    entryState = state_start;
                    startOffset = *offset;
                    break;
                }
                /* hit error or eof, bail */
                goto lexed;
            default:
                lexer->error = yajl_lex_invalid_char;
                tok = yajl_tok_error;
                goto lexed;
        }
    }


  lexed:
    /* need to append to buffer if the buffer is in use or
     * if it's an EOF token */
    if (tok == yajl_tok_eof || entryState != state_start) {
        yajl_buf_append(lexer->buf, jsonText + startOffset,
                        *offset - startOffset);
        if (tok != yajl_tok_eof) {
            if (tok != yajl_tok_error) { /* Nick added this test, see below */
                *outBuf = yajl_buf_data(lexer->buf);
                *outLen = yajl_buf_len(lexer->buf);
 /*fwrite(*outBuf, *outLen, 1, stderr);
 fputc('\n', stderr);*/
            }
            lexer->state = state_start;
        }
    } else {
        if (tok != yajl_tok_error) {
            *outBuf = jsonText + startOffset;
            *outLen = *offset - startOffset;
 /*fwrite(*outBuf, *outLen, 1, stderr);
 fputc('\n', stderr);*/
        }
        lexer->state = state_start;
    }

    /* special case for strings. skip the quotes. */
    if (tok == yajl_tok_string || tok == yajl_tok_string_with_escapes)
    {
        assert(*outLen >= 2);
        (*outBuf)++;
        *outLen -= 2;
    }


#ifdef YAJL_LEXER_DEBUG
    if (tok == yajl_tok_error) {
        printf("lexical error: %s\n",
               yajl_lex_error_to_string(yajl_lex_get_error(lexer)));
    } else if (tok == yajl_tok_eof) {
        printf("EOF hit\n");
    } else {
        printf("lexed %s: '", tokToStr(tok));
        fwrite(*outBuf, 1, *outLen, stdout);
        printf("'\n");
    }
#endif

    return tok;
}

const char *
yajl_lex_error_to_string(yajl_lex_error error)
{
    switch (error) {
        case yajl_lex_e_ok:
            return "ok, no error";
        case yajl_lex_string_invalid_utf8:
            return "invalid bytes in UTF8 string.";
        case yajl_lex_string_invalid_escaped_char:
            return "inside a string, '\\' occurs before a character "
                   "which it may not.";
        case yajl_lex_string_invalid_json_char:
            return "invalid character inside string.";
        case yajl_lex_string_invalid_hex_char:
            return "invalid (non-hex) character occurs after '\\u' inside "
                   "string.";
        case yajl_lex_invalid_char:
            return "invalid char in json text.";
        case yajl_lex_invalid_string:
            return "invalid string in json text.";
        case yajl_lex_leading_zeros:
            return "malformed number, extra leading zeros are not allowed.";
        case yajl_lex_missing_integer_after_exponent:
            return "malformed number, a digit is required after the exponent.";
        case yajl_lex_missing_integer_after_decimal:
            return "malformed number, a digit is required after the "
                   "decimal point.";
        case yajl_lex_missing_integer_after_minus:
            return "malformed number, a digit is required after the "
                   "minus sign.";
        case yajl_lex_unallowed_comment:
            return "probable comment found in input text, comments are "
                   "not enabled.";
        /* yajl_rev_lex_lex() specific error messages: */
        case yajl_lex_missing_integer_before_exponent:
            return "malformed number, a digit is required before the exponent.";
        case yajl_lex_missing_integer_before_decimal:
            return "malformed number, a digit is required before the "
                   "decimal point.";
        case yajl_lex_missing_exponent_before_plus:
            return "malformed number, an exponent is required before the "
                   "plus sign.";
    }
    return "unknown error code";
}


/** allows access to more specific information about the lexical
 *  error when yajl_lex_lex returns yajl_tok_error. */
yajl_lex_error
yajl_lex_get_error(yajl_lexer lexer)
{
    if (lexer == NULL) return (yajl_lex_error) -1;
    return lexer->error;
}

size_t yajl_lex_current_line(yajl_lexer lexer)
{
    return lexer->lineOff;
}

size_t yajl_lex_current_char(yajl_lexer lexer)
{
    return lexer->charOff;
}

yajl_tok yajl_lex_peek(yajl_lexer lexer, const unsigned char * jsonText,
                       size_t jsonTextLen, size_t offset)
{
    const unsigned char * outBuf;
    size_t outLen;
    size_t bufLen = yajl_buf_len(lexer->buf);
    yajl_lex_state state = lexer->state;
    int substate = lexer->substate;
    int subsubstate = lexer->subsubstate;
    yajl_tok tok;

    tok = yajl_lex_lex(lexer, jsonText, jsonTextLen, &offset,
                       &outBuf, &outLen);

    lexer->state = state;
    lexer->substate = substate;
    lexer->subsubstate = subsubstate;
    yajl_buf_truncate(lexer->buf, bufLen);

    return tok;
}
