/*	$OpenBSD: dumpgame.c,v 1.12 2016/01/07 14:37:51 mestre Exp $	*/
/*	$NetBSD: dumpgame.c,v 1.4 1995/04/24 12:25:54 cgd Exp $	*/

/*
 * Copyright (c) 1980, 1993
 *	The Regents of the University of California.  All rights reserved.
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

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "trek.h"

/***  THIS CONSTANT MUST CHANGE AS THE DATA SPACES CHANGE ***/
# define	VERSION		3

struct dump
{
	char	*area;
	int	count;
};


struct dump	Dump_template[] =
{
	{ (char *)&Ship,	sizeof (Ship) },
	{ (char *)&Now,		sizeof (Now) },
	{ (char *)&Param,	sizeof (Param) },
	{ (char *)&Etc,		sizeof (Etc) },
	{ (char *)&Game,	sizeof (Game) },
	{ (char *)Sect,		sizeof (Sect) },
	{ (char *)Quad,		sizeof (Quad) },
	{ (char *)&Move,	sizeof (Move) },
	{ (char *)Event,	sizeof (Event) },
	{ NULL,			0 }
};

static int readdump(int);

/*
**  DUMP GAME
**
**	This routine dumps the game onto the file "trek.dump".  The
**	first two bytes of the file are a version number, which
**	reflects whether this image may be used.  Obviously, it must
**	change as the size, content, or order of the data structures
**	output change.
*/

void
dumpgame(int v)
{
	int		version;
	int		fd;
	struct dump	*d;
	int		i;

	if ((fd = open("trek.dump", O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0)
	{
		warn("cannot open `trek.dump'");
		return;
	}
	version = VERSION;
	write(fd, &version, sizeof version);

	/* output the main data areas */
	for (d = Dump_template; d->area; d++)
	{
		write(fd, &d->area, sizeof d->area);
		i = d->count;
		write(fd, d->area, i);
	}

	close(fd);
}


/*
**  RESTORE GAME
**
**	The game is restored from the file "trek.dump".  In order for
**	this to succeed, the file must exist and be readable, must
**	have the correct version number, and must have all the appro-
**	priate data areas.
**
**	Return value is zero for success, one for failure.
*/

int
restartgame(void)
{
	int	fd, version;

	if ((fd = open("trek.dump", O_RDONLY)) == -1 ||
	    read(fd, &version, sizeof version) != sizeof version ||
	    version != VERSION ||
	    readdump(fd))
	{
		printf("cannot restart\n");
		if (fd != -1)
			close(fd);
		return (1);
	}

	close(fd);
	return (0);
}


/*
**  READ DUMP
**
**	This is the business end of restartgame().  It reads in the
**	areas.
**
**	Returns zero for success, one for failure.
*/

static int
readdump(int fd1)
{
	int		fd, i;
	struct dump	*d;
	long		junk;

	fd = fd1;

	for (d = Dump_template; d->area; d++)
	{
		if (read(fd, &junk, sizeof junk) != (sizeof junk))
			return (1);
		if ((char *)junk != d->area)
			return (1);
		i = d->count;
		if (read(fd, d->area, i) != i)
			return (1);
	}

	/* make quite certain we are at EOF */
	return (read(fd, &junk, 1));
}