/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

/*
 * This file contains code originally derrived from OpenBSD fnmatch.c
 *
 * Copyright (c) 1989, 1993, 1994
 *      The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Guido van Rossum.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Function fnmatch() as specified in POSIX 1003.2-1992, section B.6.
 * Compares a filename or pathname to a pattern.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "fnmatch.h"

#define EOS		'\0'

#define RANGE_MATCH		1
#define RANGE_NOMATCH		0
#define RANGE_ERROR		(-1)

static int rangematch(const char *, char, int, char **);

static int
p_fnmatchx(const char *pattern, const char *string, int flags, size_t recurs)
{
	const char *stringstart;
	char *newp;
	char c, test;
	int recurs_flags = flags & ~FNM_PERIOD;

	if (recurs-- == 0)
		return FNM_NORES;

	for (stringstart = string;;)
		switch (c = *pattern++) {
		case EOS:
			if ((flags & FNM_LEADING_DIR) && *string == '/')
				return (0);

			return (*string == EOS ? 0 : FNM_NOMATCH);

		case '?':
			if (*string == EOS)
				return (FNM_NOMATCH);

			if (*string == '/' && (flags & FNM_PATHNAME))
				return (FNM_NOMATCH);

			if (*string == '.' && (flags & FNM_PERIOD) &&
			    (string == stringstart ||
			     ((flags & FNM_PATHNAME) && *(string - 1) == '/')))
				return (FNM_NOMATCH);

			++string;
			break;

		case '*':
			c = *pattern;

			/* Let '**' override PATHNAME match for this segment.
			 * It will be restored if/when we recurse below.
			 */
			if (c == '*') {
				flags &= ~FNM_PATHNAME;

				while (c == '*')
					c = *++pattern;

				if (c == '/')
					c = *++pattern;
			}

			if (*string == '.' && (flags & FNM_PERIOD) &&
			    (string == stringstart ||
			     ((flags & FNM_PATHNAME) && *(string - 1) == '/')))
				return (FNM_NOMATCH);

			/* Optimize for pattern with * at end or before /. */
			if (c == EOS) {
				if (flags & FNM_PATHNAME)
					return ((flags & FNM_LEADING_DIR) ||
					        strchr(string, '/') == NULL ?
					        0 : FNM_NOMATCH);
				else
					return (0);
			} else if (c == '/' && (flags & FNM_PATHNAME)) {
				if ((string = strchr(string, '/')) == NULL)
					return (FNM_NOMATCH);

				break;
			}

			/* General case, use recursion. */
			while ((test = *string) != EOS) {
				int e;
				e = p_fnmatchx(pattern, string, recurs_flags, recurs);

				if (e != FNM_NOMATCH)
					return e;

				if (test == '/' && (flags & FNM_PATHNAME))
					break;

				++string;
			}

			return (FNM_NOMATCH);

		case '[':
			if (*string == EOS)
				return (FNM_NOMATCH);

			if (*string == '/' && (flags & FNM_PATHNAME))
				return (FNM_NOMATCH);

			if (*string == '.' && (flags & FNM_PERIOD) &&
			    (string == stringstart ||
			     ((flags & FNM_PATHNAME) && *(string - 1) == '/')))
				return (FNM_NOMATCH);

			switch (rangematch(pattern, *string, flags, &newp)) {
			case RANGE_ERROR:
				/* not a good range, treat as normal text */
				goto normal;

			case RANGE_MATCH:
				pattern = newp;
				break;

			case RANGE_NOMATCH:
				return (FNM_NOMATCH);
			}

			++string;
			break;

		case '\\':
			if (!(flags & FNM_NOESCAPE)) {
				if ((c = *pattern++) == EOS) {
					c = '\\';
					--pattern;
				}
			}

		/* FALLTHROUGH */
		default:
normal:
			if (c != *string && !((flags & FNM_CASEFOLD) &&
			                      (tolower((unsigned char)c) ==
			                       tolower((unsigned char)*string))))
				return (FNM_NOMATCH);

			++string;
			break;
		}

	/* NOTREACHED */
}

static int
rangematch(const char *pattern, char test, int flags, char **newp)
{
	int negate, ok;
	char c, c2;

	/*
		* A bracket expression starting with an unquoted circumflex
		* character produces unspecified results (IEEE 1003.2-1992,
		* 3.13.2). This implementation treats it like '!', for
		* consistency with the regular expression syntax.
		* J.T. Conklin (conklin@ngai.kaleida.com)
		*/
	if ((negate = (*pattern == '!' || *pattern == '^')) != 0)
		++pattern;

	if (flags & FNM_CASEFOLD)
		test = (char)tolower((unsigned char)test);

	/*
		* A right bracket shall lose its special meaning and represent
		* itself in a bracket expression if it occurs first in the list.
		* -- POSIX.2 2.8.3.2
		*/
	ok = 0;
	c = *pattern++;

	do {
		if (c == '\\' && !(flags & FNM_NOESCAPE))
			c = *pattern++;

		if (c == EOS)
			return (RANGE_ERROR);

		if (c == '/' && (flags & FNM_PATHNAME))
			return (RANGE_NOMATCH);

		if ((flags & FNM_CASEFOLD))
			c = (char)tolower((unsigned char)c);

		if (*pattern == '-'
		    && (c2 = *(pattern + 1)) != EOS && c2 != ']') {
			pattern += 2;

			if (c2 == '\\' && !(flags & FNM_NOESCAPE))
				c2 = *pattern++;

			if (c2 == EOS)
				return (RANGE_ERROR);

			if (flags & FNM_CASEFOLD)
				c2 = (char)tolower((unsigned char)c2);

			if (c <= test && test <= c2)
				ok = 1;
		} else if (c == test)
			ok = 1;
	} while ((c = *pattern++) != ']');

	*newp = (char *)pattern;
	return (ok == negate ? RANGE_NOMATCH : RANGE_MATCH);
}

int
p_fnmatch(const char *pattern, const char *string, int flags)
{
	return p_fnmatchx(pattern, string, flags, 64);
}

