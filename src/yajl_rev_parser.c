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

#include "api/yajl_parse.h"
#include "yajl_rev_lex.h"
#include "yajl_rev_parser.h"
#include "yajl_encode.h"
#include "yajl_bytestack.h"

#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <math.h>

#if 0
#define MAX_VALUE_TO_MULTIPLY ((LLONG_MAX / 10) + (LLONG_MAX % 10))

 /* same semantics as strtol */
long long
yajl_parse_integer(const unsigned char *number, unsigned int length)
{
    long long ret  = 0;
    long sign = 1;
    const unsigned char *pos = number;
    if (*pos == '-') { pos++; sign = -1; }
    if (*pos == '+') { pos++; }

    while (pos < number + length) {
        if ( ret > MAX_VALUE_TO_MULTIPLY ) {
            errno = ERANGE;
            return sign == 1 ? LLONG_MAX : LLONG_MIN;
        }
        ret *= 10;
        if (LLONG_MAX - ret < (*pos - '0')) {
            errno = ERANGE;
            return sign == 1 ? LLONG_MAX : LLONG_MIN;
        }
        if (*pos < '0' || *pos > '9') {
            errno = ERANGE;
            return sign == 1 ? LLONG_MAX : LLONG_MIN;
        }
        ret += (*pos++ - '0');
    }

    return sign * ret;
}

unsigned char *
yajl_render_error_string(yajl_handle hand, const unsigned char * jsonText,
                         size_t jsonTextLen, int verbose)
{
    size_t offset = hand->bytesConsumed;
    unsigned char * str;
    const char * errorType = NULL;
    const char * errorText = NULL;
    char text[72];
    const char * arrow = "                     (right here) ------^\n";

    if (yajl_bs_current(hand->stateStack) == yajl_state_parse_error) {
        errorType = "parse";
        errorText = hand->parseError;
    } else if (yajl_bs_current(hand->stateStack) == yajl_state_lexical_error) {
        errorType = "lexical";
        errorText = yajl_lex_error_to_string(yajl_lex_get_error(hand->lexer));
    } else {
        errorType = "unknown";
    }

    {
        size_t memneeded = 0;
        memneeded += strlen(errorType);
        memneeded += strlen(" error");
        if (errorText != NULL) {
            memneeded += strlen(": ");
            memneeded += strlen(errorText);
        }
        str = (unsigned char *) YA_MALLOC(&(hand->alloc), memneeded + 2);
        if (!str) return NULL;
        str[0] = 0;
        strcat((char *) str, errorType);
        strcat((char *) str, " error");
        if (errorText != NULL) {
            strcat((char *) str, ": ");
            strcat((char *) str, errorText);
        }
        strcat((char *) str, "\n");
    }

    /* now we append as many spaces as needed to make sure the error
     * falls at char 41, if verbose was specified */
    if (verbose) {
        size_t start, end, i;
        size_t spacesNeeded;

        spacesNeeded = (offset < 30 ? 40 - offset : 10);
        start = (offset >= 30 ? offset - 30 : 0);
        end = (offset + 30 > jsonTextLen ? jsonTextLen : offset + 30);

        for (i=0;i<spacesNeeded;i++) text[i] = ' ';

        for (;start < end;start++, i++) {
            if (jsonText[start] != '\n' && jsonText[start] != '\r')
            {
                text[i] = jsonText[start];
            }
            else
            {
                text[i] = ' ';
            }
        }
        assert(i <= 71);
        text[i++] = '\n';
        text[i] = 0;
        {
            char * newStr = (char *)
                YA_MALLOC(&(hand->alloc), (unsigned int)(strlen((char *) str) +
                                                         strlen((char *) text) +
                                                         strlen(arrow) + 1));
            if (newStr) {
                newStr[0] = 0;
                strcat((char *) newStr, (char *) str);
                strcat((char *) newStr, text);
                strcat((char *) newStr, arrow);
            }
            YA_FREE(&(hand->alloc), str);
            str = (unsigned char *) newStr;
        }
    }
    return str;
}
#endif


#if 0 /* alternative version with map key *after* value */
yajl_status
yajl_rev_do_finish(yajl_handle hand)
{
    yajl_status stat;
    stat = yajl_rev_do_parse(hand,(const unsigned char *) " " + 1, -1);

    if (stat != yajl_status_ok) return stat;

    switch(yajl_bs_current(hand->stateStack))
    {
        case yajl_state_parse_error:
        case yajl_state_lexical_error:
            return yajl_status_error;
        case yajl_state_got_value:
        case yajl_state_parse_complete:
            return yajl_status_ok;
        default:
            if (!(hand->flags & yajl_allow_partial_values))
            {
                yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                hand->parseError = "premature EOF";
                return yajl_status_error;
            }
            return yajl_status_ok;
    }
}


yajl_status
yajl_rev_do_parse(yajl_handle hand, const unsigned char * jsonText,
                  ssize_t jsonTextLen)
{
    yajl_tok tok;
    const unsigned char * buf;
    size_t bufLen;
    ssize_t offset = 0;
    int cont = 1;

#ifdef YAJL_SUPPLEMENTARY
    /* in this case there can be a delay between parsing a token and
     * its callback, if so then both offsets can be outside buffer */
    hand->startOffset -= jsonTextLen;
    hand->endOffset -= jsonTextLen;
#endif

around_again:
    if (!cont) {
        if (!(hand->flags & yajl_resume_after_cancel)) {
            yajl_bs_set(hand->stateStack, yajl_state_parse_error);
            hand->parseError =
                "client cancelled parse via callback return value";
        }
        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
        return yajl_status_client_canceled;
    }
    switch (yajl_bs_current(hand->stateStack)) {
        case yajl_state_parse_complete:
            if (hand->flags & yajl_allow_multiple_values) {
                yajl_bs_set(hand->stateStack, yajl_state_got_value);
                goto around_again;
            }
#ifdef YAJL_SUPPLEMENTARY
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_string:
                    if (hand->callbacks && hand->callbacks->yajl_sup_string) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_string(hand->ctx,
                            buf, bufLen);
                        goto around_again;
                    }
                    goto root_unallowed;
                case yajl_tok_string_with_escapes:
                    if (hand->callbacks && hand->callbacks->yajl_sup_string) {
                        yajl_buf_clear(hand->decodeBuf);
                        yajl_string_decode(hand->decodeBuf, buf, bufLen);
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_string(hand->ctx,
                            (const unsigned char *)
                                yajl_buf_data(hand->decodeBuf),
                            yajl_buf_len(hand->decodeBuf));
                        goto around_again;
                    }
                    goto root_unallowed;
                case yajl_tok_bool:
                    if (hand->callbacks && hand->callbacks->yajl_sup_boolean) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_boolean(hand->ctx,
                            *buf == 't');
                        goto around_again;
                    }
                    goto root_unallowed;
                case yajl_tok_null:
                    if (hand->callbacks && hand->callbacks->yajl_sup_null) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_null(hand->ctx);
                        goto around_again;
                    }
                    goto root_unallowed;
                case yajl_tok_integer:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_sup_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_number(hand->ctx,
                                (const char *) buf, bufLen);
                            goto around_again;
                        } else if (hand->callbacks->yajl_sup_integer) {
                            long long int i = 0;
                            errno = 0;
                            i = yajl_parse_integer(buf, bufLen);
                            if ((i == LLONG_MIN || i == LLONG_MAX) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "integer overflow" ;
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_integer(hand->ctx,
                                i);
                            goto around_again;
                        }
                    }
                    goto root_unallowed;
                case yajl_tok_double:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_sup_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_number(hand->ctx,
                                (const char *) buf, bufLen);
                            goto around_again;
                        } else if (hand->callbacks->yajl_sup_double) {
                            double d = 0.0;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            buf = yajl_buf_data(hand->decodeBuf);
                            errno = 0;
                            d = strtod((char *) buf, NULL);
                            if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "numeric (floating point) "
                                    "overflow";
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_double(hand->ctx,
                                d);
                            goto around_again;
                        }
                    }
                    /* intentional fallthru */
                root_unallowed:
                default:
                    if (hand->flags & yajl_allow_trailing_garbage) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        return yajl_status_ok;
                    }
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError = "trailing garbage";
                    goto around_again;
            }
#else
            if (!(hand->flags & yajl_allow_trailing_garbage)) {
                if (offset != jsonTextLen) {
                    tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                           &offset, &buf, &bufLen);
                    if (tok != yajl_tok_eof) {
                        yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                        hand->parseError = "trailing garbage";
                    }
                    goto around_again;
                }
            }
            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
            return yajl_status_ok;
#endif
        case yajl_state_lexical_error:
        case yajl_state_parse_error:
            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
            return yajl_status_error;
        case yajl_state_start:
        case yajl_state_got_value:
        case yajl_state_map_need_val:
        case yajl_state_array_need_val:
        case yajl_state_array_start:  {
            /* for arrays and maps, we advance the state for this
             * depth, then push the state of the next depth.
             * If an error occurs during the parsing of the nesting
             * enitity, the state at this level will not matter.
             * a state that needs pushing will be anything other
             * than state_start */

            yajl_state stateToPush = yajl_state_start;

            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);

            switch (tok) {
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                case yajl_tok_string:
                    if (hand->callbacks && hand->callbacks->yajl_string) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_string(hand->ctx,
                            buf, bufLen);
                    }
                    break;
                case yajl_tok_string_with_escapes:
                    if (hand->callbacks && hand->callbacks->yajl_string) {
                        yajl_buf_clear(hand->decodeBuf);
                        yajl_string_decode(hand->decodeBuf, buf, bufLen);
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_string(hand->ctx,
                            yajl_buf_data(hand->decodeBuf),
                            yajl_buf_len(hand->decodeBuf));
                    }
                    break;
                case yajl_tok_bool:
                    if (hand->callbacks && hand->callbacks->yajl_boolean) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_boolean(hand->ctx,
                            *buf == 't');
                    }
                    break;
                case yajl_tok_null:
                    if (hand->callbacks && hand->callbacks->yajl_null) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_null(hand->ctx);
                    }
                    break;
                case yajl_tok_right_bracket:
                    if (hand->callbacks && hand->callbacks->yajl_end_map) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_end_map(hand->ctx);
                    }
                    stateToPush = yajl_state_map_start;
                    break;
                case yajl_tok_right_brace:
                    if (hand->callbacks && hand->callbacks->yajl_end_array) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_end_array(hand->ctx);
                    }
                    stateToPush = yajl_state_array_start;
                    break;
                case yajl_tok_integer:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_number(hand->ctx,
                                (const char *) buf, bufLen);
                        } else if (hand->callbacks->yajl_integer) {
                            long long int i = 0;
                            errno = 0;
                            i = yajl_parse_integer(buf, bufLen);
                            if ((i == LLONG_MIN || i == LLONG_MAX) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "integer overflow" ;
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_integer(hand->ctx,
                                i);
                        }
                    }
                    break;
                case yajl_tok_double:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_number(hand->ctx,
                                (const char *) buf, bufLen);
                        } else if (hand->callbacks->yajl_double) {
                            double d = 0.0;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            buf = yajl_buf_data(hand->decodeBuf);
                            errno = 0;
                            d = strtod((char *) buf, NULL);
                            if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "numeric (floating point) "
                                    "overflow";
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_double(hand->ctx,
                                d);
                        }
                    }
                    break;
                case yajl_tok_left_brace:
                    if (yajl_bs_current(hand->stateStack) ==
                        yajl_state_array_start)
                    {
                        if (hand->callbacks &&
                            hand->callbacks->yajl_start_array)
                        {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_start_array(hand->ctx);
                        }
                        yajl_bs_pop(hand->stateStack);
                        goto around_again;
                    }
                    /* intentional fall-through */
                case yajl_tok_colon:
                case yajl_tok_comma:
                case yajl_tok_left_bracket:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError =
                        "unallowed token at this point in JSON text";
                    goto around_again;
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError = "invalid token, internal error";
                    goto around_again;
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_got_val);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            if (stateToPush != yajl_state_start) {
                yajl_bs_push(hand->stateStack, stateToPush);
            }

            goto around_again;
        }
        case yajl_state_map_start:
        case yajl_state_map_need_key: {
            /* only difference between these two states is that in
             * start '}' is valid, whereas in need_key, we've parsed
             * a comma, and a string key _must_ follow */
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                case yajl_tok_string_with_escapes:
                    if (hand->callbacks && hand->callbacks->yajl_map_key) {
                        yajl_buf_clear(hand->decodeBuf);
                        yajl_string_decode(hand->decodeBuf, buf, bufLen);
                        buf = yajl_buf_data(hand->decodeBuf);
                        bufLen = yajl_buf_len(hand->decodeBuf);
                    }
                    /* intentional fall-through */
                case yajl_tok_string:
                    if (hand->callbacks && hand->callbacks->yajl_map_key) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_map_key(hand->ctx,
                            buf, bufLen);
                    }
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                    goto around_again;
                case yajl_tok_left_bracket:
                    if (yajl_bs_current(hand->stateStack) ==
                        yajl_state_map_start)
                    {
                        if (hand->callbacks && hand->callbacks->yajl_start_map) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_start_map(hand->ctx);
                        }
                        yajl_bs_pop(hand->stateStack);
                        goto around_again;
                    }
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError =
                        "invalid object key (must be a string)"; 
                    goto around_again;
            }
        }
        case yajl_state_map_sep: {
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_colon:
                    yajl_bs_set(hand->stateStack, yajl_state_map_need_val);
                    goto around_again;
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError = "object key and value must "
                        "be separated by a colon (':')";
                    goto around_again;
            }
        }
        case yajl_state_map_got_val: {
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_left_bracket:
                    if (hand->callbacks && hand->callbacks->yajl_start_map) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_start_map(hand->ctx);
                    }
                    yajl_bs_pop(hand->stateStack);
                    goto around_again;
                case yajl_tok_comma:
                    yajl_bs_set(hand->stateStack, yajl_state_map_need_key);
                    goto around_again;
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
#ifdef YAJL_SUPPLEMENTARY
                case yajl_tok_string:
                    if (hand->callbacks && hand->callbacks->yajl_sup_string) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_string(hand->ctx,
                            buf, bufLen);
                        goto around_again;
                    }
                    goto map_unallowed;
                case yajl_tok_string_with_escapes:
                    if (hand->callbacks && hand->callbacks->yajl_sup_string) {
                        yajl_buf_clear(hand->decodeBuf);
                        yajl_string_decode(hand->decodeBuf, buf, bufLen);
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_string(hand->ctx,
                            yajl_buf_data(hand->decodeBuf),
                            yajl_buf_len(hand->decodeBuf));
                        goto around_again;
                    }
                    goto map_unallowed;
                case yajl_tok_bool:
                    if (hand->callbacks && hand->callbacks->yajl_sup_boolean) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_boolean(hand->ctx,
                            *buf == 't');
                        goto around_again;
                    }
                    goto map_unallowed;
                case yajl_tok_null:
                    if (hand->callbacks && hand->callbacks->yajl_sup_null) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_null(hand->ctx);
                        goto around_again;
                    }
                    goto map_unallowed;
                case yajl_tok_integer:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_sup_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_number(hand->ctx,
                                (const char *) buf, bufLen);
                            goto around_again;
                        } else if (hand->callbacks->yajl_sup_integer) {
                            long long int i = 0;
                            errno = 0;
                            i = yajl_parse_integer(buf, bufLen);
                            if ((i == LLONG_MIN || i == LLONG_MAX) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "integer overflow" ;
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_integer(hand->ctx,
                                i);
                            goto around_again;
                        }
                    }
                    goto map_unallowed;
                case yajl_tok_double:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_sup_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_number(hand->ctx,
                                (const char *) buf, bufLen);
                            goto around_again;
                        } else if (hand->callbacks->yajl_sup_double) {
                            double d = 0.0;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            buf = yajl_buf_data(hand->decodeBuf);
                            errno = 0;
                            d = strtod((char *) buf, NULL);
                            if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "numeric (floating point) "
                                    "overflow";
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_double(hand->ctx,
                                d);
                            goto around_again;
                        }
                    }
                    /* intentional fallthru */
                map_unallowed:
#endif
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError = "before key and value, inside map, "
                                       "I expect ',' or '{'";
                    goto around_again;
            }
        }
        case yajl_state_array_got_val: {
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_left_brace:
                    if (hand->callbacks && hand->callbacks->yajl_start_array) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_start_array(hand->ctx);
                    }
                    yajl_bs_pop(hand->stateStack);
                    goto around_again;
                case yajl_tok_comma:
                    yajl_bs_set(hand->stateStack, yajl_state_array_need_val);
                    goto around_again;
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
#ifdef YAJL_SUPPLEMENTARY
                case yajl_tok_string:
                    if (hand->callbacks && hand->callbacks->yajl_sup_string) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_string(hand->ctx,
                            buf, bufLen);
                        goto around_again;
                    }
                    goto array_unallowed;
                case yajl_tok_string_with_escapes:
                    if (hand->callbacks && hand->callbacks->yajl_sup_string) {
                        yajl_buf_clear(hand->decodeBuf);
                        yajl_string_decode(hand->decodeBuf, buf, bufLen);
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_string(hand->ctx,
                            yajl_buf_data(hand->decodeBuf),
                            yajl_buf_len(hand->decodeBuf));
                        goto around_again;
                    }
                    goto array_unallowed;
                case yajl_tok_bool:
                    if (hand->callbacks && hand->callbacks->yajl_sup_boolean) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_boolean(hand->ctx,
                            *buf == 't');
                        goto around_again;
                    }
                    goto array_unallowed;
                case yajl_tok_null:
                    if (hand->callbacks && hand->callbacks->yajl_sup_null) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_sup_null(hand->ctx);
                        goto around_again;
                    }
                    goto array_unallowed;
                case yajl_tok_integer:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_sup_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_number(hand->ctx,
                                (const char *) buf, bufLen);
                            goto around_again;
                        } else if (hand->callbacks->yajl_sup_integer) {
                            long long int i = 0;
                            errno = 0;
                            i = yajl_parse_integer(buf, bufLen);
                            if ((i == LLONG_MIN || i == LLONG_MAX) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "integer overflow" ;
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_integer(hand->ctx,
                                i);
                            goto around_again;
                        }
                    }
                    goto array_unallowed;
                case yajl_tok_double:
                    if (hand->callbacks) {
                        if (hand->callbacks->yajl_sup_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_number(hand->ctx,
                                (const char *) buf, bufLen);
                            goto around_again;
                        } else if (hand->callbacks->yajl_sup_double) {
                            double d = 0.0;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            buf = yajl_buf_data(hand->decodeBuf);
                            errno = 0;
                            d = strtod((char *) buf, NULL);
                            if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "numeric (floating point) "
                                    "overflow";
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_sup_double(hand->ctx,
                                d);
                            goto around_again;
                        }
                    }
                    /* intentional fallthru */
                array_unallowed:
#endif
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError =
                        "before array element, I expect ',' or '['";
                    goto around_again;
            }
        }
#ifdef YAJL_SUPPLEMENTARY
        default:
            break;
#endif 
    }

    abort();
    return yajl_status_error;
}
#else
yajl_status
yajl_rev_do_finish(yajl_handle hand)
{
    yajl_status stat;
#ifdef YAJL_SUPPLEMENTARY
    int cont = 1;
#endif

    stat = yajl_rev_do_parse(hand,(const unsigned char *) " " + 1, -1);

    if (stat != yajl_status_ok) return stat;

#ifdef YAJL_SUPPLEMENTARY
around_again:
    if (!cont) {
        if (!(hand->flags & yajl_resume_after_cancel)) {
            yajl_bs_set(hand->stateStack, yajl_state_parse_error);
            hand->parseError =
                "client cancelled parse via callback return value";
        }
        hand->bytesConsumed = 0;
        return yajl_status_client_canceled;
    }
#endif
    switch(yajl_bs_current(hand->stateStack))
    {
        case yajl_state_parse_error:
        case yajl_state_lexical_error:
            return yajl_status_error;
        case yajl_state_got_value:
        case yajl_state_parse_complete:
            return yajl_status_ok;
#ifdef YAJL_SUPPLEMENTARY
        case yajl_state_sup_null: {
            yajl_bs_pop(hand->stateStack);
            if (hand->callbacks->yajl_null) {
                hand->bytesConsumed = 0;
                cont = hand->callbacks->yajl_null(hand->ctx);
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_boolean: {
            yajl_bs_pop(hand->stateStack);
            if (hand->callbacks->yajl_boolean) {
                hand->bytesConsumed = 0;
                cont = hand->callbacks->yajl_boolean(hand->ctx,
                    *yajl_buf_data(hand->decodeBuf) == 't');
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_integer: {
            yajl_bs_pop(hand->stateStack);
            if (hand->callbacks->yajl_number) {
                hand->bytesConsumed = 0;
                cont = hand->callbacks->yajl_number(hand->ctx,
                    (const char *) yajl_buf_data(hand->decodeBuf),
                    yajl_buf_len(hand->decodeBuf));
            } else if (hand->callbacks->yajl_integer) {
                long long int i = 0;
                errno = 0;
                i = yajl_parse_integer(yajl_buf_data(hand->decodeBuf),
                                       yajl_buf_len(hand->decodeBuf));
                if ((i == LLONG_MIN || i == LLONG_MAX) &&
                    errno == ERANGE)
                {
                    yajl_bs_set(hand->stateStack,
                                yajl_state_parse_error);
                    hand->parseError = "integer overflow" ;
                    goto around_again;
                }
                hand->bytesConsumed = 0;
                cont = hand->callbacks->yajl_integer(hand->ctx, i);
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_double: {
            yajl_bs_pop(hand->stateStack);
            if (hand->callbacks->yajl_number) {
                hand->bytesConsumed = 0;
                cont = hand->callbacks->yajl_number(hand->ctx,
                    (const char *) yajl_buf_data(hand->decodeBuf),
                    yajl_buf_len(hand->decodeBuf));
            } else if (hand->callbacks->yajl_double) {
                double d = 0.0;
                errno = 0;
                d = strtod((char *) yajl_buf_data(hand->decodeBuf),
                           NULL);
                if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                    errno == ERANGE)
                {
                    yajl_bs_set(hand->stateStack,
                                yajl_state_parse_error);
                    hand->parseError = "numeric (floating point) "
                        "overflow";
                    goto around_again;
                }
                hand->bytesConsumed = 0;
                cont = hand->callbacks->yajl_double(hand->ctx, d);
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_string: {
            yajl_bs_pop(hand->stateStack);
            if (hand->callbacks->yajl_string) {
                hand->bytesConsumed = 0;
                cont = hand->callbacks->yajl_string(hand->ctx,
                    yajl_buf_data(hand->decodeBuf),
                    yajl_buf_len(hand->decodeBuf));
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
#endif
        default:
            if (!(hand->flags & yajl_allow_partial_values))
            {
                yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                hand->parseError = "premature EOF";
                return yajl_status_error;
            }
            return yajl_status_ok;
    }
}

yajl_status
yajl_rev_do_parse(yajl_handle hand, const unsigned char * jsonText,
                  ssize_t jsonTextLen)
{
    yajl_tok tok;
    const unsigned char * buf;
    size_t bufLen;
    ssize_t offset = 0;
    int cont = 1;

around_again:
    if (!cont) {
        if (!(hand->flags & yajl_resume_after_cancel)) {
            yajl_bs_set(hand->stateStack, yajl_state_parse_error);
            hand->parseError =
                "client cancelled parse via callback return value";
        }
        hand->endOffset = (size_t) (offset - jsonTextLen) +
                          bufLen;
        return yajl_status_client_canceled;
    }
    switch (yajl_bs_current(hand->stateStack)) {
        case yajl_state_parse_complete:
            if (hand->flags & yajl_allow_multiple_values) {
                yajl_bs_set(hand->stateStack, yajl_state_got_value);
                goto around_again;
            }
            if (!(hand->flags & yajl_allow_trailing_garbage)) {
                if (offset != jsonTextLen) {
                    tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                           &offset, &buf, &bufLen);
                    if (tok != yajl_tok_eof) {
                        yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                        hand->parseError = "trailing garbage";
                    }
                    goto around_again;
                }
            }
            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
            return yajl_status_ok;
        case yajl_state_lexical_error:
        case yajl_state_parse_error:
            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
            return yajl_status_error;
        case yajl_state_start:
        case yajl_state_got_value:
        /* only difference between these two states is that in
         * start '}' is valid, whereas in need_key, we've parsed
         * a comma, and a string key _must_ follow */
        case yajl_state_map_need_val:
        case yajl_state_map_start:
        case yajl_state_array_need_val:
        case yajl_state_array_start:  {
            /* for arrays and maps, we advance the state for this
             * depth, then push the state of the next depth.
             * If an error occurs during the parsing of the nesting
             * enitity, the state at this level will not matter.
             * a state that needs pushing will be anything other
             * than state_start */

            yajl_state stateToPush = yajl_state_start;

            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);

            switch (tok) {
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                case yajl_tok_string:
                    if (hand->callbacks) {
#ifdef YAJL_SUPPLEMENTARY
                        if (hand->callbacks->yajl_sup_string) {
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            yajl_bs_push(hand->stateStack,
                                         yajl_state_sup_string);
                            goto around_again;
                        }
#endif
                        if (hand->callbacks->yajl_string) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_string(hand->ctx,
                                buf, bufLen);
                        }
                    }
                    break;
                case yajl_tok_string_with_escapes:
                    if (hand->callbacks) {
#ifdef YAJL_SUPPLEMENTARY
                        if (hand->callbacks->yajl_sup_string) {
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_string_decode(hand->decodeBuf, buf, bufLen);
                            yajl_bs_push(hand->stateStack,
                                         yajl_state_sup_string);
                            goto around_again;
                        }
#endif
                        if (hand->callbacks->yajl_string) {
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_string_decode(hand->decodeBuf, buf, bufLen);
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_string(hand->ctx,
                                yajl_buf_data(hand->decodeBuf),
                                yajl_buf_len(hand->decodeBuf));
                        }
                    }
                    break;
                case yajl_tok_bool:
                    if (hand->callbacks) {
#ifdef YAJL_SUPPLEMENTARY
                        if (hand->callbacks->yajl_sup_boolean) {
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, 1/*bufLen*/);
                            yajl_bs_push(hand->stateStack,
                                         yajl_state_sup_boolean);
                            goto around_again;
                        }
#endif
                        if (hand->callbacks->yajl_boolean) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_boolean(hand->ctx,
                                *buf == 't');
                        }
                    }
                    break;
                case yajl_tok_null:
                    if (hand->callbacks) {
#ifdef YAJL_SUPPLEMENTARY
                        if (hand->callbacks->yajl_sup_null) {
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            yajl_bs_push(hand->stateStack,
                                         yajl_state_sup_null);
                            goto around_again;
                        }
#endif
                        if (hand->callbacks->yajl_null) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_null(hand->ctx);
                        }
                    }
                    break;
                case yajl_tok_right_bracket:
                    if (hand->callbacks && hand->callbacks->yajl_end_map) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_end_map(hand->ctx);
                    }
                    stateToPush = yajl_state_map_start;
                    break;
                case yajl_tok_right_brace:
                    if (hand->callbacks && hand->callbacks->yajl_end_array) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_end_array(hand->ctx);
                    }
                    stateToPush = yajl_state_array_start;
                    break;
                case yajl_tok_integer:
                    if (hand->callbacks) {
#ifdef YAJL_SUPPLEMENTARY
                        if (hand->callbacks->yajl_sup_number ||
                            hand->callbacks->yajl_sup_integer)
                        {
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            yajl_bs_push(hand->stateStack,
                                         yajl_state_sup_integer);
                            goto around_again;
                        }
#endif
                        if (hand->callbacks->yajl_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_number(hand->ctx,
                                (const char *) buf, bufLen);
                        } else if (hand->callbacks->yajl_integer) {
                            long long int i = 0;
                            errno = 0;
                            i = yajl_parse_integer(buf, bufLen);
                            if ((i == LLONG_MIN || i == LLONG_MAX) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "integer overflow" ;
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_integer(hand->ctx, i);
                        }
                    }
                    break;
                case yajl_tok_double:
                    if (hand->callbacks) {
#ifdef YAJL_SUPPLEMENTARY
                        if (hand->callbacks->yajl_sup_number ||
                            hand->callbacks->yajl_sup_double)
                        {
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            yajl_bs_push(hand->stateStack,
                                         yajl_state_sup_double);
                            goto around_again;
                        }
#endif
                        if (hand->callbacks->yajl_number) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_number(hand->ctx,
                                (const char *) buf, bufLen);
                        } else if (hand->callbacks->yajl_double) {
                            double d = 0.0;
                            yajl_buf_clear(hand->decodeBuf);
                            yajl_buf_append(hand->decodeBuf, buf, bufLen);
                            buf = yajl_buf_data(hand->decodeBuf);
                            errno = 0;
                            d = strtod((char *) buf, NULL);
                            if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                                errno == ERANGE)
                            {
                                yajl_bs_set(hand->stateStack,
                                            yajl_state_parse_error);
                                hand->parseError = "numeric (floating point) "
                                    "overflow";
                                goto around_again;
                            }
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_double(hand->ctx, d);
                        }
                    }
                    break;
                case yajl_tok_left_brace:
                    if (yajl_bs_current(hand->stateStack) ==
                        yajl_state_array_start)
                    {
                        if (hand->callbacks &&
                            hand->callbacks->yajl_start_array)
                        {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_start_array(hand->ctx);
                        }
                        yajl_bs_pop(hand->stateStack);
                        goto around_again;
                    }
                    goto unallowed;
                case yajl_tok_left_bracket:
                    if (yajl_bs_current(hand->stateStack) ==
                        yajl_state_map_start)
                    {
                        if (hand->callbacks && hand->callbacks->yajl_start_map) {
                            hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                            hand->startOffset = (size_t) (offset - jsonTextLen);
                            hand->endOffset = (size_t) (offset - jsonTextLen) +
                                              bufLen;
                            cont = hand->callbacks->yajl_start_map(hand->ctx);
                        }
                        yajl_bs_pop(hand->stateStack);
                        goto around_again;
                    }
                    /* intentional fall-through */
                unallowed: 
                case yajl_tok_colon:
                case yajl_tok_comma:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError =
                        "unallowed token at this point in JSON text";
                    goto around_again;
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError = "invalid token, internal error";
                    goto around_again;
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            if (stateToPush != yajl_state_start) {
                yajl_bs_push(hand->stateStack, stateToPush);
            }

            goto around_again;
        }
        case yajl_state_map_need_key: {
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                case yajl_tok_string_with_escapes:
                    if (hand->callbacks && hand->callbacks->yajl_map_key) {
                        yajl_buf_clear(hand->decodeBuf);
                        yajl_string_decode(hand->decodeBuf, buf, bufLen);
                        buf = yajl_buf_data(hand->decodeBuf);
                        bufLen = yajl_buf_len(hand->decodeBuf);
                    }
                    /* intentional fall-through */
                case yajl_tok_string:
                    if (hand->callbacks && hand->callbacks->yajl_map_key) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_map_key(hand->ctx,
                            buf, bufLen);
                    }
                    yajl_bs_set(hand->stateStack, yajl_state_map_got_val);
                    goto around_again;
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError =
                        "invalid object key (must be a string)"; 
                    goto around_again;
            }
        }
        case yajl_state_map_sep: {
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_colon:
                    yajl_bs_set(hand->stateStack, yajl_state_map_need_key);
                    goto around_again;
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError = "object key and value must "
                        "be separated by a colon (':')";
                    goto around_again;
            }
        }
        case yajl_state_map_got_val: { /* actually means just got key */
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_left_bracket:
                    if (hand->callbacks && hand->callbacks->yajl_start_map) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_start_map(hand->ctx);
                    }
                    yajl_bs_pop(hand->stateStack);
                    goto around_again;
                case yajl_tok_comma:
                    yajl_bs_set(hand->stateStack, yajl_state_map_need_val);
                    goto around_again;
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError = "before key and value, inside map, "
                                       "I expect ',' or '{'";
                    goto around_again;
            }
        }
        case yajl_state_array_got_val: {
            tok = yajl_rev_lex_lex(hand->lexer, jsonText, jsonTextLen,
                                   &offset, &buf, &bufLen);
            switch (tok) {
                case yajl_tok_left_brace:
                    if (hand->callbacks && hand->callbacks->yajl_start_array) {
                        hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                        hand->startOffset = (size_t) (offset - jsonTextLen);
                        hand->endOffset = (size_t) (offset - jsonTextLen) +
                                          bufLen;
                        cont = hand->callbacks->yajl_start_array(hand->ctx);
                    }
                    yajl_bs_pop(hand->stateStack);
                    goto around_again;
                case yajl_tok_comma:
                    yajl_bs_set(hand->stateStack, yajl_state_array_need_val);
                    goto around_again;
                case yajl_tok_eof:
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    return yajl_status_ok;
                case yajl_tok_error:
                    yajl_bs_set(hand->stateStack, yajl_state_lexical_error);
                    goto around_again;
                default:
                    yajl_bs_set(hand->stateStack, yajl_state_parse_error);
                    hand->parseError =
                        "before array element, I expect ',' or '['";
                    goto around_again;
            }
        }
#ifdef YAJL_SUPPLEMENTARY
        case yajl_state_sup_null: {
            unsigned char c;
            do {
                if (offset <= jsonTextLen) {
                    hand->bytesConsumed = 0;
                    return yajl_status_ok;
                }
                c = jsonText[--offset];
            } while ((c >= '\t' && c <= '\r') || c == ' ');
            offset++;
            yajl_bs_pop(hand->stateStack);
            if ((c >= '0' && c <= '9') || c == '"' ||
                (c >= 'A' && c <= 'Z') || c == ']' ||
                (c >= 'a' && c <= 'z') || c == '}') {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_sup_null(hand->ctx);
                goto around_again;
            }
            if (hand->callbacks->yajl_null) {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_null(hand->ctx);
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_boolean: {
            unsigned char c;
            do {
                if (offset <= jsonTextLen) {
                    hand->bytesConsumed = 0;
                    return yajl_status_ok;
                }
                c = jsonText[--offset];
            } while ((c >= '\t' && c <= '\r') || c == ' ');
            offset++;
            yajl_bs_pop(hand->stateStack);
            if ((c >= '0' && c <= '9') || c == '"' ||
                (c >= 'A' && c <= 'Z') || c == ']' ||
                (c >= 'a' && c <= 'z') || c == '}') {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_sup_boolean(hand->ctx,
                    *yajl_buf_data(hand->decodeBuf) == 't');
                goto around_again;
            }
            if (hand->callbacks->yajl_boolean) {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_boolean(hand->ctx,
                    *yajl_buf_data(hand->decodeBuf) == 't');
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_integer: {
            unsigned char c;
            do {
                if (offset <= jsonTextLen) {
                    hand->bytesConsumed = 0;
                    return yajl_status_ok;
                }
                c = jsonText[--offset];
            } while ((c >= '\t' && c <= '\r') || c == ' ');
            offset++;
            yajl_bs_pop(hand->stateStack);
            if ((c >= '0' && c <= '9') || c == '"' ||
                (c >= 'A' && c <= 'Z') || c == ']' ||
                (c >= 'a' && c <= 'z') || c == '}') {
                if (hand->callbacks->yajl_sup_number) {
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    cont = hand->callbacks->yajl_sup_number(hand->ctx,
                        (const char *) yajl_buf_data(hand->decodeBuf),
                        yajl_buf_len(hand->decodeBuf));
                } else {
                    long long int i = 0;
                    errno = 0;
                    i = yajl_parse_integer(yajl_buf_data(hand->decodeBuf),
                                           yajl_buf_len(hand->decodeBuf));
                    if ((i == LLONG_MIN || i == LLONG_MAX) &&
                        errno == ERANGE)
                    {
                        yajl_bs_set(hand->stateStack,
                                    yajl_state_parse_error);
                        hand->parseError = "integer overflow" ;
                        goto around_again;
                    }
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    cont = hand->callbacks->yajl_sup_integer(hand->ctx, i);
                }
                goto around_again;
            }
            if (hand->callbacks->yajl_number) {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_number(hand->ctx,
                    (const char *) yajl_buf_data(hand->decodeBuf),
                    yajl_buf_len(hand->decodeBuf));
            } else if (hand->callbacks->yajl_integer) {
                long long int i = 0;
                errno = 0;
                i = yajl_parse_integer(yajl_buf_data(hand->decodeBuf),
                                       yajl_buf_len(hand->decodeBuf));
                if ((i == LLONG_MIN || i == LLONG_MAX) &&
                    errno == ERANGE)
                {
                    yajl_bs_set(hand->stateStack,
                                yajl_state_parse_error);
                    hand->parseError = "integer overflow" ;
                    goto around_again;
                }
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_integer(hand->ctx, i);
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_double: {
            unsigned char c;
            do {
                if (offset <= jsonTextLen) {
                    hand->bytesConsumed = 0;
                    return yajl_status_ok;
                }
                c = jsonText[--offset];
            } while ((c >= '\t' && c <= '\r') || c == ' ');
            offset++;
            yajl_bs_pop(hand->stateStack);
            if ((c >= '0' && c <= '9') || c == '"' ||
                (c >= 'A' && c <= 'Z') || c == ']' ||
                (c >= 'a' && c <= 'z') || c == '}') {
                if (hand->callbacks->yajl_sup_number) {
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    cont = hand->callbacks->yajl_sup_number(hand->ctx,
                        (const char *) yajl_buf_data(hand->decodeBuf),
                        yajl_buf_len(hand->decodeBuf));
                } else {
                    double d = 0.0;
                    errno = 0;
                    d = strtod((char *) yajl_buf_data(hand->decodeBuf),
                               NULL);
                    if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                        errno == ERANGE)
                    {
                        yajl_bs_set(hand->stateStack,
                                    yajl_state_parse_error);
                        hand->parseError = "numeric (floating point) "
                            "overflow";
                        goto around_again;
                    }
                    hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                    cont = hand->callbacks->yajl_sup_double(hand->ctx, d);
                }
                goto around_again;
            }
            if (hand->callbacks->yajl_number) {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_number(hand->ctx,
                    (const char *) yajl_buf_data(hand->decodeBuf),
                    yajl_buf_len(hand->decodeBuf));
            } else if (hand->callbacks->yajl_double) {
                double d = 0.0;
                errno = 0;
                d = strtod((char *) yajl_buf_data(hand->decodeBuf),
                           NULL);
                if ((d == HUGE_VAL || d == -HUGE_VAL) &&
                    errno == ERANGE)
                {
                    yajl_bs_set(hand->stateStack,
                                yajl_state_parse_error);
                    hand->parseError = "numeric (floating point) "
                        "overflow";
                    goto around_again;
                }
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_double(hand->ctx, d);
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
        case yajl_state_sup_string: {
            unsigned char c;
            do {
                if (offset <= jsonTextLen) {
                    hand->bytesConsumed = 0;
                    return yajl_status_ok;
                }
                c = jsonText[--offset];
            } while ((c >= '\t' && c <= '\r') || c == ' ');
            offset++;
            yajl_bs_pop(hand->stateStack);
            if ((c >= '0' && c <= '9') || c == '"' ||
                (c >= 'A' && c <= 'Z') || c == ']' ||
                (c >= 'a' && c <= 'z') || c == '}') {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_sup_string(hand->ctx,
                    yajl_buf_data(hand->decodeBuf),
                    yajl_buf_len(hand->decodeBuf));
                goto around_again;
            }
            if (hand->callbacks->yajl_string) {
                hand->bytesConsumed = (size_t) (offset - jsonTextLen);
                cont = hand->callbacks->yajl_string(hand->ctx,
                    yajl_buf_data(hand->decodeBuf),
                    yajl_buf_len(hand->decodeBuf));
            }
            /* got a value.  transition depends on the state we're in. */
            {
                yajl_state s = yajl_bs_current(hand->stateStack);
                if (s == yajl_state_start || s == yajl_state_got_value) {
                    yajl_bs_set(hand->stateStack, yajl_state_parse_complete);
                } else if (s == yajl_state_map_need_val ||
                           s == yajl_state_map_start) {
                    yajl_bs_set(hand->stateStack, yajl_state_map_sep);
                } else {
                    yajl_bs_set(hand->stateStack, yajl_state_array_got_val);
                }
            }
            goto around_again;
        }
#endif
    }

    abort();
    return yajl_status_error;
}
#endif


