/*	$OpenBSD: stringlist.c,v 1.12 2015/05/20 23:39:55 schwarze Exp $	*/
/*	$NetBSD: stringlist.c,v 1.2 1997/01/17 07:26:20 lukem Exp $	*/

/*
 * Copyright (c) 1994 Christos Zoulas
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef SMALL

#include <stdio.h>
#include <string.h>
#include <err.h>
#include <stdlib.h>

#include "stringlist.h"

#define _SL_CHUNKSIZE	20

/*
 * sl_init(): Initialize a string list
 */
StringList *
sl_init(void)
{
	StringList *sl = malloc(sizeof(StringList));
	if (sl == NULL)
		err(1, "stringlist");

	sl->sl_cur = 0;
	sl->sl_max = _SL_CHUNKSIZE;
	sl->sl_str = calloc(sl->sl_max, sizeof(char *));
	if (sl->sl_str == NULL)
		err(1, "stringlist");
	return sl;
}


/*
 * sl_add(): Add an item to the string list
 */
void
sl_add(StringList *sl, char *name)
{
	if (sl->sl_cur == sl->sl_max - 1) {
		sl->sl_max += _SL_CHUNKSIZE;
		sl->sl_str = reallocarray(sl->sl_str, sl->sl_max,
		    sizeof(char *));
		if (sl->sl_str == NULL)
			err(1, "stringlist");
	}
	sl->sl_str[sl->sl_cur++] = name;
}


/*
 * sl_free(): Free a stringlist
 */
void
sl_free(StringList *sl, int all)
{
	size_t i;

	if (sl == NULL)
		return;
	if (sl->sl_str) {
		if (all)
			for (i = 0; i < sl->sl_cur; i++)
				free(sl->sl_str[i]);
		free(sl->sl_str);
	}
	free(sl);
}

#endif /* !SMALL */

