/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// common.c -- misc functions used in client and server
#include "qcommon.h"
#include "../client/ref.h"

#define	MAXPRINTMSG	4096

int		realtime;

extern	refExport_t	re;		// interface to refresh .dll


cvar_t	*com_speeds;
cvar_t	*developer;
cvar_t	*timescale;
cvar_t	*timedemo;
cvar_t	*sv_cheats;

#ifndef DEDICATED_ONLY
static cvar_t dummyDedicated;
cvar_t	*dedicated = &dummyDedicated;
	// dummy cvar; may be used before initialized - when error happens before
	// QCommon_Init()::Cvar_GetVars(); after this, will point to actual cvar
#else
cvar_t	*dedicated;
#endif

static cvar_t	*logfile_active;		// 1 = buffer log, 2 = flush after each print

static FILE	*logfile;

static server_state_t server_state;

// com_speeds times
int		time_before_game, time_after_game, time_before_ref, time_after_ref;

int		linewidth = 80;

/*
============================================================================

CLIENT / SERVER interactions

============================================================================
*/

static char	*rd_buffer;
static int	rd_buffersize;
static void	(*rd_flush)(char *buffer);


void Com_BeginRedirect (char *buffer, int buffersize, void (*flush)(char*))
{
	if (!buffer || !buffersize || !flush)
		return;
	rd_buffer = buffer;
	rd_buffersize = buffersize;
	rd_flush = flush;

	*rd_buffer = 0;
}

void Com_EndRedirect (void)
{
	rd_flush (rd_buffer);

	rd_buffer = NULL;
	rd_buffersize = 0;
	rd_flush = NULL;
}


/*
=============
Com_Printf

Both client and server can use this, and it will output
to the apropriate place.
=============
*/

static bool console_logged = false;
static char log_name[MAX_OSPATH];

// from new engine strings.cpp
static void appUncolorizeString (char *dst, const char *src)
{
	char	c, c1;

	if (!src) src = dst;
	do
	{
		c = *src++;
		if (c == COLOR_ESCAPE)
		{
			c1 = *src;
			if (c1 < '0' || c1 > '7')
				*dst++ = c;
			else
				src++;
		}
		else
			*dst++ = c;
	} while (c);
}

void Com_Printf (const char *fmt, ...)
{
	va_list	argptr;
	char	msg[MAXPRINTMSG];

	guard(Com_Printf);

	va_start (argptr,fmt);
	vsnprintf (ARRAY_ARG(msg),fmt,argptr);
	va_end (argptr);

	if (rd_buffer)
	{
		if ((strlen (msg) + strlen(rd_buffer)) > rd_buffersize - 1)
		{
			rd_flush (rd_buffer);
			*rd_buffer = 0;
		}
		strcat (rd_buffer, msg);
		return;
	}

	if (!DEDICATED)
		Con_Print (msg);

	// also echo to debugging console (in dedicated server mode only, when win32)
	Sys_ConsoleOutput (msg);

	// logfile
	if (logfile && logfile_active->integer)
	{
		const char *s = strchr (msg, '\r');		// find line-feed
		if (s)	s++;
		else	s = msg;
		if (!s[0]) return;						// nothing to print
		if (!logfile)
			logfile = fopen ("./console.log", logfile_active->integer > 2 || console_logged ? "a" : "w");

		if (logfile)
		{
			appUncolorizeString (msg, msg);
			fprintf (logfile, "%s", msg);
			if (logfile_active->integer > 1)	// force to save every message
			{
				fclose (logfile);
				logfile = NULL;
			}
		}
		console_logged = true;
	}

	unguard;
}


/*
================
Com_DPrintf

A Com_Printf that only shows up if the "developer" cvar is set
(prints on a screen with a different color)
When developer is set to 2, logging the message.
When developer is set to 256, do not colorize message.
================
*/
void Com_DPrintf (const char *fmt, ...)
{
	va_list	argptr;
	char	msg[MAXPRINTMSG];

	if (!developer->integer) return;

	va_start (argptr,fmt);
	vsnprintf (ARRAY_ARG(msg),fmt,argptr);
	va_end (argptr);
	Com_Printf (S_BLUE"%s", msg);
	if (developer->integer == 2)
	{
		appUncolorizeString (msg, msg);
		DebugPrintf ("%s", msg);
	}
}


bool debugLogged = false;

void DebugPrintf (const char *fmt, ...)
{
	va_list	argptr;
	char	msg[1024], ctime[256];
	FILE	*log;
	time_t	itime;

	va_start (argptr,fmt);
	vsnprintf (ARRAY_ARG(msg),fmt,argptr);
	va_end (argptr);

	log = fopen ("debug.log", "a+");
	if (!debugLogged)
	{
		time (&itime);
		strftime (ARRAY_ARG(ctime), "%a %b %d, %Y (%H:%M:%S)", localtime (&itime));
		fprintf (log, "\n\n----- " APPNAME " debug log on %s -----\n", ctime);
		debugLogged = true;
	}
	fprintf (log, "%s", msg);
	fclose (log);
}


/*
================
Com_WPrintf

Printf non-fatal error message (warning) using a different color
When developer is set to 2, logging the message
================
*/

void Com_WPrintf (const char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	vsnprintf (ARRAY_ARG(msg),fmt,argptr);
	va_end (argptr);

	Com_Printf (S_YELLOW"%s", msg);
	if (developer->integer == 2) DebugPrintf ("%s", msg);
}


/*-----------------------------------------------------------------------------
	Error handling
-----------------------------------------------------------------------------*/

static char errMsg[MAXPRINTMSG];
static bool errRecurse = false;


void Com_FatalError (const char *fmt, ...)
{
	va_list argptr;

	GErr.isFatalError = true;
	GErr.isSoftError = true;

	if (errRecurse)
		Sys_Error ("recursive error after: %s", errMsg);

	va_start (argptr, fmt);
	vsnprintf (ARRAY_ARG(errMsg), fmt, argptr);
	va_end (argptr);

	errRecurse = true;
	SV_Shutdown (va("Server fatal crashed: %s\n", errMsg), false);
	CL_Shutdown (true);

	if (logfile)
	{
		fclose (logfile);
		logfile = NULL;
	}

	if (debugLogged) DebugPrintf ("FATAL ERROR: %s\n", errMsg);
	Sys_Error ("%s", errMsg);
}


void Com_DropError (const char *fmt, ...)
{
	va_list argptr;

	GErr.isSoftError = true;

	if (errRecurse)
		Sys_Error ("recursive error after: %s", errMsg);

	if (fmt)
	{
		va_start (argptr, fmt);
		vsnprintf (ARRAY_ARG(errMsg), fmt, argptr);
		va_end (argptr);
		Com_Printf (S_RED"ERROR: %s\n", errMsg);
		SV_Shutdown (va("Server crashed: %s\n", errMsg), false);
	}
	else
		SV_Shutdown ("", false);
	if (!DEDICATED) CL_Drop (true);
	throw 1;
}


errorState_t GErr;

void appUnwindPrefix (const char *fmt)
{
	char	buf[512];

	if (GErr.wasError)
		appSprintf (buf, sizeof(buf), " <- %s:", fmt);
	else
		appSprintf (buf, sizeof(buf), "%s:", fmt);

	GErr.wasError = false;		// will not insert "<-" next appUnwindThrow()
	strncat (GErr.history, buf, sizeof(GErr.history));
}


void appUnwindThrow (const char *fmt, ...)
{
	char	buf[512];
	va_list	argptr;

	va_start (argptr, fmt);
	if (GErr.wasError)
	{
		buf[0] = ' '; buf[1] = '<'; buf[2] = '-'; buf[3] = ' ';
		vsnprintf (buf+4, sizeof(buf)-4, fmt, argptr);
	}
	else
		vsnprintf (buf, sizeof(buf), fmt, argptr);
	va_end (argptr);
	GErr.wasError = true;

	strncat (GErr.history, buf, sizeof(GErr.history));

#if 1
	throw;
#else
	*((int*)NULL) &= 123;
#endif
}


void Com_ResetErrorState (void)
{
	memset (&GErr, 0, sizeof(GErr));
}


/*
=============
Com_Quit

Both client and server can use this, and it will
do the apropriate things.
=============
*/
void Com_Quit (void)
{
	SV_Shutdown ("Server quit\n", false);
	CL_Shutdown (false);
	QCommon_Shutdown ();
	Sys_Quit ();
}


// Mask variants:
// 1) *      - any file
// 2) *.*    - any file
// 3) *rest  - name ends with "rest" (for example, ".ext")
// 4) start* - name starts with "start"
// 4) text   - name equals "text"
// Comparision is case-sensitive.
bool MatchWildcard (const char *name, const char *mask, bool ignoreCase)
{
	int		masklen, namelen;
//	char	maskCopy[MAX_QPATH], *next;
	char	maskCopy[MAX_OSPATH], nameCopy[MAX_OSPATH], *next;

	if (ignoreCase)
	{
		Q_strncpylower (nameCopy, name, sizeof(nameCopy));
		name = nameCopy;
		Q_strncpylower (maskCopy, mask, sizeof(maskCopy));
	}
	else
		appStrncpyz (maskCopy, mask, sizeof(maskCopy));
	namelen = strlen (name);

	for (mask = maskCopy; mask; mask = next)
	{
		// find next wildcard (comma-separated)
		next = strchr (mask, ',');
		if (next)
		{
			masklen = next - mask;
			next++;		// skip ','
		}
		else
			masklen = strlen (mask);

		if (!masklen)
		{
			// used something like "mask1,,mask2" (2nd mask is empty)
			Com_DPrintf ("CheckMask(): skip empty mask in \"%s\"\n", mask);
			continue;
		}

		// check for a trivial wildcard
		if (mask[0] == '*')
		{
			if (masklen == 1 || (masklen == 3 && mask[1] == '.' && mask[2] == '*'))
				return true;			// "*" or "*.*" -- any name valid
		}

		// "*text*" mask
		if (masklen >= 3 && mask[0] == '*' && mask[masklen-1] == '*')
		{
			int		i;

			mask++;
			masklen -= 2;
			for (i = 0; i <= namelen - masklen; i++)
				if (!memcmp (&name[i], mask, masklen)) return true;
		}
		else
		{
			char	*suff;

			// "*text" or "text*" mask
			suff = strchr (mask, '*');
			if (next && suff >= next) suff = NULL;		// suff can be in next wildcard
			if (suff)
			{
				int		preflen, sufflen;

				preflen = suff - mask;
				sufflen = masklen - preflen - 1;
				suff++;

				if (namelen < preflen + sufflen)
					continue;			// name is not long enough
				if (preflen && memcmp (name, mask, preflen))
					continue;			// different prefix
				if (sufflen && memcmp (name + namelen - sufflen, suff, sufflen))
					continue;			// different suffix

				return true;
			}
			// exact match ("text")
			if (namelen == masklen && !memcmp (name, mask, namelen))
				return true;
		}
	}

	return false;
}


/*
============
va

does a varargs printf into a temp buffer, so I don't need to have
varargs versions of all text functions.
============
*/

#define VA_GOODSIZE		1024
#define VA_BUFSIZE		2048

char *va (const char *format, ...)
{
	va_list argptr;
	static char buf[VA_BUFSIZE];
	static int bufPos = 0;
	int		len;
	char	*str;

	str = &buf[bufPos];
	va_start (argptr, format);
	len = vsnprintf (str, VA_GOODSIZE, format, argptr);
	va_end (argptr);

	if (len < 0)
		return NULL;	// error (may be, overflow)

	bufPos += len + 1;
	if (bufPos > VA_BUFSIZE - VA_GOODSIZE)
		bufPos = 0;		// cycle buffer

	return str;
}


int appSprintf (char *dest, int size, const char *fmt, ...)
{
	int		len;
	va_list	argptr;
#if 1

	va_start (argptr, fmt);
	len = vsnprintf (dest, size, fmt, argptr);
	va_end (argptr);
	if (len < 0 || len > size - 1)
		Com_WPrintf ("appSprintf: overflow %d > %d" RETADDR_STR "\n", len, size, GET_RETADDR(dest));
#else
	char	bigbuffer[0x10000];

	va_start (argptr, fmt);
	len = vsprintf (bigbuffer, fmt, argptr);
	va_end (argptr);
	if (len >= size)
		Com_WPrintf ("appSprintf: overflow %d > %d\n", len, size);
	strncpy (dest, bigbuffer, size-1);
#endif

	return len;
}


static unsigned GetInt (char **str)
{
	unsigned r;
	char	c, *s;

	r = 0;
	s = *str;
	while (1)
	{
		c = *s;
		if (c < '0' || c > '9') break;
		r = r * 10 + c - '0';
		s++;
	}
	*str = s;
	return r;
}

bool IPWildcard (netadr_t *a, char *mask)
{
	int		i, n;
	char	*m;

	if (a->type != NA_IP) return false;
	m = mask;

	for (i = 0; i < 4; i++)
	{
		if (m[0] == '*')
			m++;			// skip '*'
		else if (m[0] >= '0' && m[0] <= '9')
		{
			n = GetInt (&m);
			if (m[0] == '.' || m[0] == 0)
			{
				if (a->ip[i] != n) return false;
			}
			else if (m[0] == '-')
			{
				if (a->ip[i] < n) return false;
				m++;
				n = GetInt (&m);
				if (a->ip[i] > n) return false;
			}
			else
			{
				Com_DPrintf ("IPWildcard: bad char in \"%s\"\n", mask);
				return false;
			}
		}
		else
		{
			Com_DPrintf ("IPWildcard: bad char in \"%s\"\n", mask);
			return false;
		}
		if (m[0] == 0 && i < 3)
		{
			Com_DPrintf ("IPWildcard: short mask \"%s\"\n", mask);
			return true;
		}
		m++;
	}
	return true;
}


/*
==================
Com_ServerState
==================
*/
server_state_t Com_ServerState (void)
{
	return server_state;
}

/*
==================
Com_SetServerState
==================
*/
void Com_SetServerState (server_state_t state)
{
	server_state = state;
}


#if 1
// 0 to 1
float frand (void)
{
	return rand() * (1.0f/RAND_MAX);
}

// -1 to 1
float crand (void)
{
	return rand() * (2.0f/RAND_MAX) - 1;
}
#endif

/*
==============================================================================

			MESSAGE IO FUNCTIONS

Handles byte ordering and avoids alignment errors
==============================================================================
*/

vec3_t	bytedirs[NUMVERTEXNORMALS] =
{
#include "anorms.h"
};

//
// writing functions
//

void MSG_WriteChar (sizebuf_t *sb, int c)
{
	byte	*buf;

#ifdef PARANOID
	if (c < -128 || c > 127)
		Com_FatalError ("MSG_WriteChar: range error");
#endif

	buf = (byte*)SZ_GetSpace (sb, 1);
	buf[0] = c;
}

void MSG_WriteByte (sizebuf_t *sb, int c)
{
	byte	*buf;

#ifdef PARANOID
	if (c < 0 || c > 255)
		Com_FatalError ("MSG_WriteByte: range error");
#endif

	buf = (byte*)SZ_GetSpace (sb, 1);
	buf[0] = c;
}

void MSG_WriteShort (sizebuf_t *sb, int c)
{
	byte	*buf;

#ifdef PARANOID
	if (c < ((short)0x8000) || c > (short)0x7fff)
		Com_FatalError ("MSG_WriteShort: range error");
#endif

	buf = (byte*)SZ_GetSpace (sb, 2);
	//!! optimize for little-endian machines
	buf[0] = c&0xff;
	buf[1] = c>>8;
}

void MSG_WriteLong (sizebuf_t *sb, int c)
{
	byte	*buf;

	buf = (byte*)SZ_GetSpace (sb, 4);
	//!! optimize
	*buf++ = c & 0xff;
	*buf++ = (c>>8) & 0xff;
	*buf++ = (c>>16) & 0xff;
	*buf = c>>24;
}

void MSG_WriteFloat (sizebuf_t *sb, float f)
{
	union {
		float	f;
		int		l;
	} dat;

	dat.f = f;
	dat.l = LittleLong (dat.l);

	SZ_Write (sb, &dat.l, 4);
}

void MSG_WriteString (sizebuf_t *sb, char *s)
{
	if (!s)	SZ_Write (sb, "", 1);
	else	SZ_Write (sb, s, strlen(s)+1);
}

void MSG_WriteCoord (sizebuf_t *sb, float f)
{
	MSG_WriteShort (sb, appRound (f*8));
}

void MSG_WritePos (sizebuf_t *sb, vec3_t pos)
{
	MSG_WriteShort (sb, appRound (pos[0]*8));
	MSG_WriteShort (sb, appRound (pos[1]*8));
	MSG_WriteShort (sb, appRound (pos[2]*8));
}

void MSG_WriteAngle (sizebuf_t *sb, float f)
{
	MSG_WriteByte (sb, appRound (f*256.0f/360) & 255);
}

void MSG_WriteAngle16 (sizebuf_t *sb, float f)
{
	MSG_WriteShort (sb, ANGLE2SHORT(f));
}


void MSG_WriteDeltaUsercmd (sizebuf_t *buf, usercmd_t *from, usercmd_t *cmd)
{
	int		bits;

	// send the movement message
	bits = 0;
	if (cmd->angles[0] != from->angles[0])		bits |= CM_ANGLE1;
	if (cmd->angles[1] != from->angles[1])		bits |= CM_ANGLE2;
	if (cmd->angles[2] != from->angles[2])		bits |= CM_ANGLE3;
	if (cmd->forwardmove != from->forwardmove)	bits |= CM_FORWARD;
	if (cmd->sidemove != from->sidemove)		bits |= CM_SIDE;
	if (cmd->upmove != from->upmove)			bits |= CM_UP;
	if (cmd->buttons != from->buttons)			bits |= CM_BUTTONS;
	if (cmd->impulse != from->impulse)			bits |= CM_IMPULSE;

	MSG_WriteByte (buf, bits);

	if (bits & CM_ANGLE1)	MSG_WriteShort (buf, cmd->angles[0]);
	if (bits & CM_ANGLE2)	MSG_WriteShort (buf, cmd->angles[1]);
	if (bits & CM_ANGLE3)	MSG_WriteShort (buf, cmd->angles[2]);

	if (bits & CM_FORWARD)	MSG_WriteShort (buf, cmd->forwardmove);
	if (bits & CM_SIDE)	  	MSG_WriteShort (buf, cmd->sidemove);
	if (bits & CM_UP)		MSG_WriteShort (buf, cmd->upmove);

	if (bits & CM_BUTTONS) 	MSG_WriteByte (buf, cmd->buttons);
	if (bits & CM_IMPULSE)	MSG_WriteByte (buf, cmd->impulse);

	MSG_WriteByte (buf, cmd->msec);
	MSG_WriteByte (buf, cmd->lightlevel);
}


#if 0

static byte dirTable[3*8*8*8];		// value=0 -- uninitialized, else idx+1
	// 3*8 - cube: 6 planes, each subdivided to 4 squares (x<0,x>0,y<0,y>0)
	// 8*8 - coordinates inside square

static int GetDirCell (vec3_t dir)
{
	vec3_t	adir;
	float	m;
	int		sign, base, base2;

	adir[0] = fabs (dir[0]);
	adir[1] = fabs (dir[1]);
	adir[2] = fabs (dir[2]);
	sign = 0;
	if (dir[0] < 0) sign |= 1;
	if (dir[1] < 0) sign |= 2;
	if (dir[2] < 0) sign |= 4;

	if (adir[0] < adir[1])
	{
		base = 1;
		m = adir[1];
	}
	else
	{
		base = 0;
		m = adir[0];
	}
	if (m < adir[2])
	{
		base = 2;
		m = adir[2];
	}

	m = 8.0f / m;
	switch (base)
	{
	case 0:
		base2 = (appFloor (adir[1] * m) << 3) + appFloor (adir[2] * m);
		break;
	case 1:
		base2 = (appFloor (adir[0] * m) << 3) + appFloor (adir[2] * m);
		break;
	case 2:
		base2 = (appFloor (adir[0] * m) << 3) + appFloor (adir[1] * m);
		break;
	}
	return (base << 9) + (sign << 6) + base2;
}

void MSG_WriteDir (sizebuf_t *sb, vec3_t dir)
{
	int		cell, i, best;
	float	d, bestd;

	if (!dir)
	{
		MSG_WriteByte (sb, 0);
		return;
	}
	// find cache cell
	cell = GetDirCell (dir);
	if (dirTable[cell])	// already computed
	{
		MSG_WriteByte (sb, dirTable[cell] - 1);
		Com_Printf("*");
		return;
	}
	// compute index
	bestd = best = 0;
	for (i = 0; i < NUMVERTEXNORMALS; i++)
	{
		d = DotProduct (dir, bytedirs[i]);
		if (d > bestd)
		{
			bestd = d;
			best = i;
		}
	}
	// cache index
	dirTable[cell] = best+1;
	// write
	MSG_WriteByte (sb, best);
}

#else

void MSG_WriteDir (sizebuf_t *sb, vec3_t dir)
{
	int		i, best;
	float	d, bestd;

	if (!dir)
	{
		MSG_WriteByte (sb, 0);
		return;
	}

	bestd = 0;
	best = 0;
	for (i = 0; i < NUMVERTEXNORMALS; i++)
	{
		d = DotProduct (dir, bytedirs[i]);
		if (d > bestd)
		{
			bestd = d;
			best = i;
		}
	}
	MSG_WriteByte (sb, best);
}

#endif


void MSG_ReadDir (sizebuf_t *sb, vec3_t dir)
{
	int		b;

	b = MSG_ReadByte (sb);
	if (b >= NUMVERTEXNORMALS)
		Com_DropError ("MSG_ReadDir: out of range");
	VectorCopy (bytedirs[b], dir);
}


/*
==================
MSG_WriteDeltaEntity

Writes part of a packetentities message.
Can delta from either a baseline or a previous packet_entity
==================
*/
void MSG_WriteDeltaEntity (entity_state_t *from, entity_state_t *to, sizebuf_t *msg, bool force, bool newentity)
{
	int		bits;

	if (!to->number)
		Com_FatalError ("Unset entity number");
	if (to->number >= MAX_EDICTS)
		Com_FatalError ("Entity number >= MAX_EDICTS");

	// send an update
	bits = 0;

	if (to->number >= 256)
		bits |= U_NUMBER16;		// number8 is implicit otherwise

	if (to->origin[0] != from->origin[0])	bits |= U_ORIGIN1;
	if (to->origin[1] != from->origin[1])	bits |= U_ORIGIN2;
	if (to->origin[2] != from->origin[2])	bits |= U_ORIGIN3;

	if (to->angles[0] != from->angles[0])	bits |= U_ANGLE1;
	if (to->angles[1] != from->angles[1])	bits |= U_ANGLE2;
	if (to->angles[2] != from->angles[2])	bits |= U_ANGLE3;

	if (to->skinnum != from->skinnum)
	{
		if ((unsigned)to->skinnum < 256)
			bits |= U_SKIN8;
		else if ((unsigned)to->skinnum < 0x10000)
			bits |= U_SKIN16;
		else
			bits |= (U_SKIN8|U_SKIN16);
	}

	if (to->frame != from->frame)
	{
		if (to->frame < 256)
			bits |= U_FRAME8;
		else
			bits |= U_FRAME16;
	}

	if (to->effects != from->effects)
	{
		if (to->effects < 256)
			bits |= U_EFFECTS8;
		else if (to->effects < 0x8000)
			bits |= U_EFFECTS16;
		else
			bits |= U_EFFECTS8|U_EFFECTS16;
	}

	if (to->renderfx != from->renderfx)
	{
		if (to->renderfx < 256)
			bits |= U_RENDERFX8;
		else if (to->renderfx < 0x8000)
			bits |= U_RENDERFX16;
		else
			bits |= U_RENDERFX8|U_RENDERFX16;
	}

	if (to->solid != from->solid)		bits |= U_SOLID;

	// event is not delta compressed, just 0 compressed
	if (to->event)						bits |= U_EVENT;

	if (to->modelindex != from->modelindex)		bits |= U_MODEL;
	if (to->modelindex2 != from->modelindex2)	bits |= U_MODEL2;
	if (to->modelindex3 != from->modelindex3)	bits |= U_MODEL3;
	if (to->modelindex4 != from->modelindex4)	bits |= U_MODEL4;

	if (to->sound != from->sound)		bits |= U_SOUND;

	if (newentity || (to->renderfx & RF_BEAM))	bits |= U_OLDORIGIN;

	// write the message
	if (!bits && !force)
		return;		// nothing to send!

	//----------

	if (bits & 0xFF000000)
		bits |= U_MOREBITS3 | U_MOREBITS2 | U_MOREBITS1;
	else if (bits & 0x00FF0000)
		bits |= U_MOREBITS2 | U_MOREBITS1;
	else if (bits & 0x0000FF00)
		bits |= U_MOREBITS1;

	MSG_WriteByte (msg,	bits & 0xFF);

	if (bits & 0xFF000000)
	{
		MSG_WriteByte (msg,	(bits>>8)  & 255);
		MSG_WriteByte (msg,	(bits>>16) & 255);
		MSG_WriteByte (msg,	(bits>>24) & 255);
	}
	else if (bits & 0x00FF0000)
	{
		MSG_WriteByte (msg,	(bits>>8)  & 255);
		MSG_WriteByte (msg,	(bits>>16) & 255);
	}
	else if (bits & 0x0000FF00)
	{
		MSG_WriteByte (msg,	(bits>>8)  & 255);
	}

	//----------

	if (bits & U_NUMBER16)
		MSG_WriteShort (msg, to->number);
	else
		MSG_WriteByte (msg,	to->number);

	if (bits & U_MODEL)		MSG_WriteByte (msg,	to->modelindex);
	if (bits & U_MODEL2)	MSG_WriteByte (msg,	to->modelindex2);
	if (bits & U_MODEL3)	MSG_WriteByte (msg,	to->modelindex3);
	if (bits & U_MODEL4)	MSG_WriteByte (msg,	to->modelindex4);

	if (bits & U_FRAME8)	MSG_WriteByte (msg, to->frame);
	if (bits & U_FRAME16)	MSG_WriteShort (msg, to->frame);

	if ((bits & U_SKIN8) && (bits & U_SKIN16))		//used for laser colors
		MSG_WriteLong (msg, to->skinnum);
	else if (bits & U_SKIN8)
		MSG_WriteByte (msg, to->skinnum);
	else if (bits & U_SKIN16)
		MSG_WriteShort (msg, to->skinnum);


	if ( (bits & (U_EFFECTS8|U_EFFECTS16)) == (U_EFFECTS8|U_EFFECTS16) )
		MSG_WriteLong (msg, to->effects);
	else if (bits & U_EFFECTS8)
		MSG_WriteByte (msg, to->effects);
	else if (bits & U_EFFECTS16)
		MSG_WriteShort (msg, to->effects);

	if ( (bits & (U_RENDERFX8|U_RENDERFX16)) == (U_RENDERFX8|U_RENDERFX16) )
		MSG_WriteLong (msg, to->renderfx);
	else if (bits & U_RENDERFX8)
		MSG_WriteByte (msg, to->renderfx);
	else if (bits & U_RENDERFX16)
		MSG_WriteShort (msg, to->renderfx);

	if (bits & U_ORIGIN1)	MSG_WriteCoord (msg, to->origin[0]);
	if (bits & U_ORIGIN2)	MSG_WriteCoord (msg, to->origin[1]);
	if (bits & U_ORIGIN3)	MSG_WriteCoord (msg, to->origin[2]);

	if (bits & U_ANGLE1) 	MSG_WriteAngle(msg, to->angles[0]);
	if (bits & U_ANGLE2) 	MSG_WriteAngle(msg, to->angles[1]);
	if (bits & U_ANGLE3) 	MSG_WriteAngle(msg, to->angles[2]);

	if (bits & U_OLDORIGIN)
	{
		MSG_WriteCoord (msg, to->old_origin[0]);
		MSG_WriteCoord (msg, to->old_origin[1]);
		MSG_WriteCoord (msg, to->old_origin[2]);
	}

	if (bits & U_SOUND)  	MSG_WriteByte (msg, to->sound);
	if (bits & U_EVENT)  	MSG_WriteByte (msg, to->event);
	if (bits & U_SOLID)  	MSG_WriteShort (msg, to->solid);
}


//============================================================

//
// reading functions
//

void MSG_BeginReading (sizebuf_t *msg)
{
	msg->readcount = 0;
}

// returns -1 if no more characters are available
int MSG_ReadChar (sizebuf_t *msg_read)
{
	int	c;

	if (msg_read->readcount+1 > msg_read->cursize)
		c = -1;
	else
		c = (signed char)msg_read->data[msg_read->readcount];
	msg_read->readcount++;

	return c;
}

int MSG_ReadByte (sizebuf_t *msg_read)
{
	int	c;

	if (msg_read->readcount+1 > msg_read->cursize)
		c = -1;
	else
		c = (unsigned char)msg_read->data[msg_read->readcount];
	msg_read->readcount++;

	return c;
}

int MSG_ReadShort (sizebuf_t *msg_read)
{
	int	c;

	//!! optimize
	if (msg_read->readcount+2 > msg_read->cursize)
		c = -1;
	else
		c = (short)(msg_read->data[msg_read->readcount] + (msg_read->data[msg_read->readcount+1]<<8));

	msg_read->readcount += 2;

	return c;
}

int MSG_ReadLong (sizebuf_t *msg_read)
{
	int	c;

	//!! optimize
	if (msg_read->readcount+4 > msg_read->cursize)
		c = -1;
	else
		c = msg_read->data[msg_read->readcount]         +
			(msg_read->data[msg_read->readcount+1]<<8)  +
			(msg_read->data[msg_read->readcount+2]<<16) +
			(msg_read->data[msg_read->readcount+3]<<24);

	msg_read->readcount += 4;

	return c;
}

float MSG_ReadFloat (sizebuf_t *msg_read)
{
	union
	{
		byte	b[4];
		float	f;
		int	l;
	} dat;

	if (msg_read->readcount+4 > msg_read->cursize)
		dat.f = -1;
	else
	{
		//!! optimize
		dat.b[0] =	msg_read->data[msg_read->readcount];
		dat.b[1] =	msg_read->data[msg_read->readcount+1];
		dat.b[2] =	msg_read->data[msg_read->readcount+2];
		dat.b[3] =	msg_read->data[msg_read->readcount+3];
	}
	msg_read->readcount += 4;

	dat.l = LittleLong (dat.l);

	return dat.f;
}

char *MSG_ReadString (sizebuf_t *msg_read)
{
	static char	string[2048];
	int		l,c;

	l = 0;
	do
	{
		c = MSG_ReadChar (msg_read);
		if (c == -1 || c == 0)
			break;
		string[l] = c;
		l++;
	} while (l < sizeof(string)-1);

	string[l] = 0;

	return string;
}

float MSG_ReadCoord (sizebuf_t *msg_read)
{
	return MSG_ReadShort(msg_read) * (1.0f/8);
}

void MSG_ReadPos (sizebuf_t *msg_read, vec3_t pos)
{
	pos[0] = MSG_ReadShort(msg_read) * (1.0f/8);
	pos[1] = MSG_ReadShort(msg_read) * (1.0f/8);
	pos[2] = MSG_ReadShort(msg_read) * (1.0f/8);
}

float MSG_ReadAngle (sizebuf_t *msg_read)
{
	return MSG_ReadChar(msg_read) * (360.0f/256);
}

float MSG_ReadAngle16 (sizebuf_t *msg_read)
{
	return SHORT2ANGLE(MSG_ReadShort(msg_read));
}

void MSG_ReadDeltaUsercmd (sizebuf_t *msg_read, usercmd_t *from, usercmd_t *move)
{
	int bits;

	memcpy (move, from, sizeof(*move));

	bits = MSG_ReadByte (msg_read);

	// read current angles
	if (bits & CM_ANGLE1)	move->angles[0] = MSG_ReadShort (msg_read);
	if (bits & CM_ANGLE2)	move->angles[1] = MSG_ReadShort (msg_read);
	if (bits & CM_ANGLE3)	move->angles[2] = MSG_ReadShort (msg_read);

	// read movement
	if (bits & CM_FORWARD)	move->forwardmove = MSG_ReadShort (msg_read);
	if (bits & CM_SIDE)   	move->sidemove = MSG_ReadShort (msg_read);
	if (bits & CM_UP)     	move->upmove = MSG_ReadShort (msg_read);

	// read buttons
	if (bits & CM_BUTTONS)	move->buttons = MSG_ReadByte (msg_read);

	if (bits & CM_IMPULSE)	move->impulse = MSG_ReadByte (msg_read);

	// read time to run command
	move->msec = MSG_ReadByte (msg_read);

	// read the light level
	move->lightlevel = MSG_ReadByte (msg_read);
}


void MSG_ReadData (sizebuf_t *msg_read, void *data, int len)
{
	int		i;

	for (i = 0; i < len; i++)
		((byte *)data)[i] = MSG_ReadByte (msg_read);
}


//===========================================================================

void SZ_Init (sizebuf_t *buf, void *data, int length)
{
	memset (buf, 0, sizeof(sizebuf_t));
	buf->data = (byte*)data;
	buf->maxsize = length;
}

void SZ_Clear (sizebuf_t *buf)
{
	buf->cursize = 0;
	buf->overflowed = false;
}

void *SZ_GetSpace (sizebuf_t *buf, int length)
{
	void	*data;
	int		need;

	need = buf->cursize + length;
	if (need > buf->maxsize)
	{
		if (!buf->allowoverflow)
			Com_FatalError ("SZ_GetSpace: overflow without allowoverflow (size is %d)", buf->maxsize);

		if (length > buf->maxsize)
			Com_FatalError ("SZ_GetSpace: %d is > full buffer size", length);

		Com_Printf ("SZ_GetSpace: overflow (max=%d, need=%d)\n", buf->maxsize, need);
		SZ_Clear (buf);
		buf->overflowed = true;
	}

	data = buf->data + buf->cursize;
	buf->cursize = need;

	return data;
}

void SZ_Write (sizebuf_t *buf, void *data, int length)
{
	memcpy (SZ_GetSpace (buf,length), data, length);
}

void SZ_Insert (sizebuf_t *buf, void *data, int length, int pos)
{
	int		len;
	byte	*from, *to;

	if (pos > buf->cursize) pos = buf->cursize;
	SZ_GetSpace (buf, length);
	// shift old data
	from = buf->data + pos;
	to = from + length;
	len = buf->cursize - pos - length;
	if (len > 0) memmove (to, from, len);
	// insert new data
	memcpy (from, data, length);
}

void SZ_Print (sizebuf_t *buf, char *data)
{
	int		len;

	len = strlen(data)+1;

	if (buf->cursize)
	{
		if (buf->data[buf->cursize-1])
			memcpy ((byte *)SZ_GetSpace(buf, len),data,len);		// no trailing 0
		else
			memcpy ((byte *)SZ_GetSpace(buf, len-1)-1,data,len);	// write over trailing 0
	}
	else
		memcpy ((byte *)SZ_GetSpace(buf, len),data,len);
}


/*
=====================================================================

  INFO STRINGS

=====================================================================
*/

/*
===============
Info_ValueForKey

Searches the string for the given
key and returns the associated value, or an empty string.
===============
*/
char *Info_ValueForKey (char *s, char *key)
{
	char	pkey[512];
	static	char value[2][512];	// use two buffers so compares
								// work without stomping on each other
	static	int	valueindex;
	char	*o;

	valueindex ^= 1;
	if (*s == '\\')
		s++;
	while (1)
	{
		o = pkey;
		while (*s != '\\')
		{
			if (!*s)
				return "";
			*o++ = *s++;
		}
		*o = 0;
		s++;

		o = value[valueindex];

		while (*s != '\\' && *s)
		{
			if (!*s)
				return "";
			*o++ = *s++;
		}
		*o = 0;

		if (!strcmp (key, pkey) )
			return value[valueindex];

		if (!*s)
			return "";
		s++;
	}
}

void Info_RemoveKey (char *s, char *key)
{
	char	*start;
	char	pkey[512];
	char	value[512];
	char	*o;

	if (strchr (key, '\\'))
	{
//		Com_Printf ("Can't use a key with a '\\'\n");
		return;
	}

	while (1)
	{
		start = s;
		if (*s == '\\')
			s++;
		o = pkey;
		while (*s != '\\')
		{
			if (!*s)
				return;
			*o++ = *s++;
		}
		*o = 0;
		s++;

		o = value;
		while (*s != '\\' && *s)
		{
			if (!*s)
				return;
			*o++ = *s++;
		}
		*o = 0;

		if (!strcmp (key, pkey) )
		{
			strcpy (start, s);	// remove this part
			return;
		}

		if (!*s)
			return;
	}

}


void Info_SetValueForKey (char *s, char *key, char *value)
{
	char	newi[MAX_INFO_STRING], *v;
	int		c;
	int		maxsize = MAX_INFO_STRING;

	if (strchr (key, '\\') || strchr (value, '\\'))
	{
		Com_WPrintf ("Can't use keys or values with a '\\'\n");
		return;
	}

	if (strchr (key, '\"') || strchr (value, '\"'))
	{
		Com_WPrintf ("Can't use keys or values with a '\"'\n");
		return;
	}

	if (strchr (key, ';'))
	{
		Com_WPrintf ("Can't use keys or values with a semicolon\n");
		return;
	}

	if (strlen (key) > MAX_INFO_KEY - 1 || strlen (value) > MAX_INFO_KEY - 1)
	{
		Com_WPrintf ("Keys and values must be < 64 characters\n");
		return;
	}
	Info_RemoveKey (s, key);
	if (!value || !value[0])
		return;

	appSprintf (ARRAY_ARG(newi), "\\%s\\%s", key, value);

	if (strlen (newi) + strlen (s) > maxsize)
	{
		Com_WPrintf ("Info string length exceeded\n");
		return;
	}

	// only copy ascii values
	s += strlen (s);
	v = newi;
	while (*v)
	{
		c = *v++;
		c &= 127;		// strip high bits
		if (c >= 32 && c < 127)
			*s++ = c;
	}
	*s = 0;
}


void Info_Print (char *s)
{
	char	key[512];
	char	value[512];
	char	*o;
	int		l;

	if (*s == '\\')
		s++;
	while (*s)
	{
		o = key;
		while (*s && *s != '\\')
			*o++ = *s++;

		l = o - key;
		if (l < 20)
		{
			memset (o, ' ', 20-l);
			key[20] = 0;
		}
		else
			*o = 0;
		Com_Printf (S_GREEN"%s", key);

		if (!*s)
		{
			Com_WPrintf ("Info_Print(): missing value\n");
			return;
		}

		o = value;
		s++;
		while (*s && *s != '\\')
			*o++ = *s++;
		*o = 0;

		if (*s)
			s++;
		Com_Printf ("%s\n", value);
	}
}


//------------------------------------------------------------

static byte chktbl[1024] = {
0x84, 0x47, 0x51, 0xc1, 0x93, 0x22, 0x21, 0x24, 0x2f, 0x66, 0x60, 0x4d, 0xb0, 0x7c, 0xda,
0x88, 0x54, 0x15, 0x2b, 0xc6, 0x6c, 0x89, 0xc5, 0x9d, 0x48, 0xee, 0xe6, 0x8a, 0xb5, 0xf4,
0xcb, 0xfb, 0xf1, 0x0c, 0x2e, 0xa0, 0xd7, 0xc9, 0x1f, 0xd6, 0x06, 0x9a, 0x09, 0x41, 0x54,
0x67, 0x46, 0xc7, 0x74, 0xe3, 0xc8, 0xb6, 0x5d, 0xa6, 0x36, 0xc4, 0xab, 0x2c, 0x7e, 0x85,
0xa8, 0xa4, 0xa6, 0x4d, 0x96, 0x19, 0x19, 0x9a, 0xcc, 0xd8, 0xac, 0x39, 0x5e, 0x3c, 0xf2,
0xf5, 0x5a, 0x72, 0xe5, 0xa9, 0xd1, 0xb3, 0x23, 0x82, 0x6f, 0x29, 0xcb, 0xd1, 0xcc, 0x71,
0xfb, 0xea, 0x92, 0xeb, 0x1c, 0xca, 0x4c, 0x70, 0xfe, 0x4d, 0xc9, 0x67, 0x43, 0x47, 0x94,
0xb9, 0x47, 0xbc, 0x3f, 0x01, 0xab, 0x7b, 0xa6, 0xe2, 0x76, 0xef, 0x5a, 0x7a, 0x29, 0x0b,
0x51, 0x54, 0x67, 0xd8, 0x1c, 0x14, 0x3e, 0x29, 0xec, 0xe9, 0x2d, 0x48, 0x67, 0xff, 0xed,
0x54, 0x4f, 0x48, 0xc0, 0xaa, 0x61, 0xf7, 0x78, 0x12, 0x03, 0x7a, 0x9e, 0x8b, 0xcf, 0x83,
0x7b, 0xae, 0xca, 0x7b, 0xd9, 0xe9, 0x53, 0x2a, 0xeb, 0xd2, 0xd8, 0xcd, 0xa3, 0x10, 0x25,
0x78, 0x5a, 0xb5, 0x23, 0x06, 0x93, 0xb7, 0x84, 0xd2, 0xbd, 0x96, 0x75, 0xa5, 0x5e, 0xcf,
0x4e, 0xe9, 0x50, 0xa1, 0xe6, 0x9d, 0xb1, 0xe3, 0x85, 0x66, 0x28, 0x4e, 0x43, 0xdc, 0x6e,
0xbb, 0x33, 0x9e, 0xf3, 0x0d, 0x00, 0xc1, 0xcf, 0x67, 0x34, 0x06, 0x7c, 0x71, 0xe3, 0x63,
0xb7, 0xb7, 0xdf, 0x92, 0xc4, 0xc2, 0x25, 0x5c, 0xff, 0xc3, 0x6e, 0xfc, 0xaa, 0x1e, 0x2a,
0x48, 0x11, 0x1c, 0x36, 0x68, 0x78, 0x86, 0x79, 0x30, 0xc3, 0xd6, 0xde, 0xbc, 0x3a, 0x2a,
0x6d, 0x1e, 0x46, 0xdd, 0xe0, 0x80, 0x1e, 0x44, 0x3b, 0x6f, 0xaf, 0x31, 0xda, 0xa2, 0xbd,
0x77, 0x06, 0x56, 0xc0, 0xb7, 0x92, 0x4b, 0x37, 0xc0, 0xfc, 0xc2, 0xd5, 0xfb, 0xa8, 0xda,
0xf5, 0x57, 0xa8, 0x18, 0xc0, 0xdf, 0xe7, 0xaa, 0x2a, 0xe0, 0x7c, 0x6f, 0x77, 0xb1, 0x26,
0xba, 0xf9, 0x2e, 0x1d, 0x16, 0xcb, 0xb8, 0xa2, 0x44, 0xd5, 0x2f, 0x1a, 0x79, 0x74, 0x87,
0x4b, 0x00, 0xc9, 0x4a, 0x3a, 0x65, 0x8f, 0xe6, 0x5d, 0xe5, 0x0a, 0x77, 0xd8, 0x1a, 0x14,
0x41, 0x75, 0xb1, 0xe2, 0x50, 0x2c, 0x93, 0x38, 0x2b, 0x6d, 0xf3, 0xf6, 0xdb, 0x1f, 0xcd,
0xff, 0x14, 0x70, 0xe7, 0x16, 0xe8, 0x3d, 0xf0, 0xe3, 0xbc, 0x5e, 0xb6, 0x3f, 0xcc, 0x81,
0x24, 0x67, 0xf3, 0x97, 0x3b, 0xfe, 0x3a, 0x96, 0x85, 0xdf, 0xe4, 0x6e, 0x3c, 0x85, 0x05,
0x0e, 0xa3, 0x2b, 0x07, 0xc8, 0xbf, 0xe5, 0x13, 0x82, 0x62, 0x08, 0x61, 0x69, 0x4b, 0x47,
0x62, 0x73, 0x44, 0x64, 0x8e, 0xe2, 0x91, 0xa6, 0x9a, 0xb7, 0xe9, 0x04, 0xb6, 0x54, 0x0c,
0xc5, 0xa9, 0x47, 0xa6, 0xc9, 0x08, 0xfe, 0x4e, 0xa6, 0xcc, 0x8a, 0x5b, 0x90, 0x6f, 0x2b,
0x3f, 0xb6, 0x0a, 0x96, 0xc0, 0x78, 0x58, 0x3c, 0x76, 0x6d, 0x94, 0x1a, 0xe4, 0x4e, 0xb8,
0x38, 0xbb, 0xf5, 0xeb, 0x29, 0xd8, 0xb0, 0xf3, 0x15, 0x1e, 0x99, 0x96, 0x3c, 0x5d, 0x63,
0xd5, 0xb1, 0xad, 0x52, 0xb8, 0x55, 0x70, 0x75, 0x3e, 0x1a, 0xd5, 0xda, 0xf6, 0x7a, 0x48,
0x7d, 0x44, 0x41, 0xf9, 0x11, 0xce, 0xd7, 0xca, 0xa5, 0x3d, 0x7a, 0x79, 0x7e, 0x7d, 0x25,
0x1b, 0x77, 0xbc, 0xf7, 0xc7, 0x0f, 0x84, 0x95, 0x10, 0x92, 0x67, 0x15, 0x11, 0x5a, 0x5e,
0x41, 0x66, 0x0f, 0x38, 0x03, 0xb2, 0xf1, 0x5d, 0xf8, 0xab, 0xc0, 0x02, 0x76, 0x84, 0x28,
0xf4, 0x9d, 0x56, 0x46, 0x60, 0x20, 0xdb, 0x68, 0xa7, 0xbb, 0xee, 0xac, 0x15, 0x01, 0x2f,
0x20, 0x09, 0xdb, 0xc0, 0x16, 0xa1, 0x89, 0xf9, 0x94, 0x59, 0x00, 0xc1, 0x76, 0xbf, 0xc1,
0x4d, 0x5d, 0x2d, 0xa9, 0x85, 0x2c, 0xd6, 0xd3, 0x14, 0xcc, 0x02, 0xc3, 0xc2, 0xfa, 0x6b,
0xb7, 0xa6, 0xef, 0xdd, 0x12, 0x26, 0xa4, 0x63, 0xe3, 0x62, 0xbd, 0x56, 0x8a, 0x52, 0x2b,
0xb9, 0xdf, 0x09, 0xbc, 0x0e, 0x97, 0xa9, 0xb0, 0x82, 0x46, 0x08, 0xd5, 0x1a, 0x8e, 0x1b,
0xa7, 0x90, 0x98, 0xb9, 0xbb, 0x3c, 0x17, 0x9a, 0xf2, 0x82, 0xba, 0x64, 0x0a, 0x7f, 0xca,
0x5a, 0x8c, 0x7c, 0xd3, 0x79, 0x09, 0x5b, 0x26, 0xbb, 0xbd, 0x25, 0xdf, 0x3d, 0x6f, 0x9a,
0x8f, 0xee, 0x21, 0x66, 0xb0, 0x8d, 0x84, 0x4c, 0x91, 0x45, 0xd4, 0x77, 0x4f, 0xb3, 0x8c,
0xbc, 0xa8, 0x99, 0xaa, 0x19, 0x53, 0x7c, 0x02, 0x87, 0xbb, 0x0b, 0x7c, 0x1a, 0x2d, 0xdf,
0x48, 0x44, 0x06, 0xd6, 0x7d, 0x0c, 0x2d, 0x35, 0x76, 0xae, 0xc4, 0x5f, 0x71, 0x85, 0x97,
0xc4, 0x3d, 0xef, 0x52, 0xbe, 0x00, 0xe4, 0xcd, 0x49, 0xd1, 0xd1, 0x1c, 0x3c, 0xd0, 0x1c,
0x42, 0xaf, 0xd4, 0xbd, 0x58, 0x34, 0x07, 0x32, 0xee, 0xb9, 0xb5, 0xea, 0xff, 0xd7, 0x8c,
0x0d, 0x2e, 0x2f, 0xaf, 0x87, 0xbb, 0xe6, 0x52, 0x71, 0x22, 0xf5, 0x25, 0x17, 0xa1, 0x82,
0x04, 0xc2, 0x4a, 0xbd, 0x57, 0xc6, 0xab, 0xc8, 0x35, 0x0c, 0x3c, 0xd9, 0xc2, 0x43, 0xdb,
0x27, 0x92, 0xcf, 0xb8, 0x25, 0x60, 0xfa, 0x21, 0x3b, 0x04, 0x52, 0xc8, 0x96, 0xba, 0x74,
0xe3, 0x67, 0x3e, 0x8e, 0x8d, 0x61, 0x90, 0x92, 0x59, 0xb6, 0x1a, 0x1c, 0x5e, 0x21, 0xc1,
0x65, 0xe5, 0xa6, 0x34, 0x05, 0x6f, 0xc5, 0x60, 0xb1, 0x83, 0xc1, 0xd5, 0xd5, 0xed, 0xd9,
0xc7, 0x11, 0x7b, 0x49, 0x7a, 0xf9, 0xf9, 0x84, 0x47, 0x9b, 0xe2, 0xa5, 0x82, 0xe0, 0xc2,
0x88, 0xd0, 0xb2, 0x58, 0x88, 0x7f, 0x45, 0x09, 0x67, 0x74, 0x61, 0xbf, 0xe6, 0x40, 0xe2,
0x9d, 0xc2, 0x47, 0x05, 0x89, 0xed, 0xcb, 0xbb, 0xb7, 0x27, 0xe7, 0xdc, 0x7a, 0xfd, 0xbf,
0xa8, 0xd0, 0xaa, 0x10, 0x39, 0x3c, 0x20, 0xf0, 0xd3, 0x6e, 0xb1, 0x72, 0xf8, 0xe6, 0x0f,
0xef, 0x37, 0xe5, 0x09, 0x33, 0x5a, 0x83, 0x43, 0x80, 0x4f, 0x65, 0x2f, 0x7c, 0x8c, 0x6a,
0xa0, 0x82, 0x0c, 0xd4, 0xd4, 0xfa, 0x81, 0x60, 0x3d, 0xdf, 0x06, 0xf1, 0x5f, 0x08, 0x0d,
0x6d, 0x43, 0xf2, 0xe3, 0x11, 0x7d, 0x80, 0x32, 0xc5, 0xfb, 0xc5, 0xd9, 0x27, 0xec, 0xc6,
0x4e, 0x65, 0x27, 0x76, 0x87, 0xa6, 0xee, 0xee, 0xd7, 0x8b, 0xd1, 0xa0, 0x5c, 0xb0, 0x42,
0x13, 0x0e, 0x95, 0x4a, 0xf2, 0x06, 0xc6, 0x43, 0x33, 0xf4, 0xc7, 0xf8, 0xe7, 0x1f, 0xdd,
0xe4, 0x46, 0x4a, 0x70, 0x39, 0x6c, 0xd0, 0xed, 0xca, 0xbe, 0x60, 0x3b, 0xd1, 0x7b, 0x57,
0x48, 0xe5, 0x3a, 0x79, 0xc1, 0x69, 0x33, 0x53, 0x1b, 0x80, 0xb8, 0x91, 0x7d, 0xb4, 0xf6,
0x17, 0x1a, 0x1d, 0x5a, 0x32, 0xd6, 0xcc, 0x71, 0x29, 0x3f, 0x28, 0xbb, 0xf3, 0x5e, 0x71,
0xb8, 0x43, 0xaf, 0xf8, 0xb9, 0x64, 0xef, 0xc4, 0xa5, 0x6c, 0x08, 0x53, 0xc7, 0x00, 0x10,
0x39, 0x4f, 0xdd, 0xe4, 0xb6, 0x19, 0x27, 0xfb, 0xb8, 0xf5, 0x32, 0x73, 0xe5, 0xcb, 0x32
};

/*
====================
COM_BlockSequenceCRCByte

For proxy protecting
====================
*/
byte COM_BlockSequenceCRCByte (byte *base, int length, int sequence)
{
	int		n;
	byte	*p;
	int		x;
	byte chkb[60 + 4];
	unsigned short crc;


	if (sequence < 0)
		Sys_Error ("BlockCRC: sequence < 0");

	p = chktbl + (sequence % (sizeof(chktbl) - 4));

	if (length > 60)
		length = 60;
	memcpy (chkb, base, length);

	chkb[length] = p[0];
	chkb[length+1] = p[1];
	chkb[length+2] = p[2];
	chkb[length+3] = p[3];

	length += 4;

	crc = CRC_Block (chkb, length);

	for (x = 0, n = 0; n < length; n++)
		x += chkb[n];

	crc = (crc ^ x) & 0xff;

	return crc;
}

//========================================================

void Key_Init (void);

/*
=============
Com_Error_f

Just throw a fatal error to
test error shutdown procedures
=============
*/
static void Com_Error_f (int argc, char **argv)
{
	if (!stricmp (argv[1], "-gpf"))
		*((int*)NULL) = 0;		// this is not "throw" command, this is GPF
	else if (!stricmp (argv[1], "-drop"))
		Com_DropError ("testing drop error");
	Com_FatalError ("%s", argv[1]);
}


/*-----------------------------------------------------------------------------
	Commandline parsing
-----------------------------------------------------------------------------*/


#define MAX_CMDLINE_PARTS	64

static char *cmdlineParts[MAX_CMDLINE_PARTS];
static int cmdlineNumParts;

static void ParseCmdline (const char *cmdline)
{
	char	c, *dst;
	static char buf[512];
	int		i, quotes;

	guard(ParseCmdline);

	// parse command line; fill cmdlineParts[] array
	buf[0] = 0;						// starts with 0 (needs for trimming trailing spaces)
	dst = &buf[1];
	while (true)
	{
		while ((c = *cmdline) == ' ') cmdline++;	// skip spaces
		if (!c) break;				// end of commandline

		if (c != '-')
		{
			// bad argument
			Com_Printf ("ParseCmdline: bad argument \"");
			do
			{
				Com_Printf ("%c", c);
				c = *++cmdline;
			} while (c != ' ' && c != 0);
			Com_Printf ("\"\n");
			continue;				// try next argument
		}

		// cmdline points to start of possible command
		if (cmdlineNumParts == MAX_CMDLINE_PARTS)
		{
			Com_WPrintf ("ParseCmdline: overflow\n");
			break;
		}
		cmdlineParts[cmdlineNumParts++] = dst;
		quotes = 0;
		while (c = *++cmdline)
		{
			if (c == '\"')
				quotes ^= 1;
			else if (c == '-' && !quotes)
			{
				char prev = *(dst-1);
				if (prev == 0 || prev == ' ')
					break;
			}
			*dst++ = c;
		}
		while (*(dst-1) == ' ') dst--;

		*dst++ = 0;
//		Com_Printf("arg[%d] = <%s>\n", cmdlineNumParts-1, cmdlineParts[cmdlineNumParts-1]);
	}

	// immediately exec strings of type "var=value"
	for (i = 0; i < cmdlineNumParts; i++)
	{
		char	*cmd, *s1, *s2;

		cmd = cmdlineParts[i];
		s1 = strchr (cmd, '=');
		s2 = strchr (cmd, '\"');
		if (s1 && (!s2 || s2 > s1))		// a=b, but '=' not inside quotes
		{
			char	varName[64], varValue[256];
			char	*value;
			int		len;
			cvar_t	*var;

			// convert to "set a b"
			appStrncpyz (varName, cmd, s1 - cmd + 1);	// copy "a"
			appStrncpyz (varValue, s1 + 1, sizeof(varValue));
			len = strlen (varValue);
			if (varValue[0] == '\"' && varValue[len-1] == '\"')
			{
				// remove quotes
				value = varValue + 1;
				varValue[len-1] = 0;
			}
			else
				value = varValue;
			var = Cvar_Set (varName, value);
			if (var)
				var->flags |= CVAR_CMDLINE;
			else
				Com_WPrintf ("ParseCmdline: unable to set \"%s\"\n", varName);

			cmdlineParts[i] = NULL;		// remove command
		}
	}

	unguard;
}


bool COM_CheckCmdlineVar (const char *name)
{
	int		i;
	char	*cmd;

	for (i = 0; i < cmdlineNumParts; i++)
	{
		cmd = cmdlineParts[i];
		if (!cmd || strchr (cmd, ' ')) continue;	// already removed or contains arguments
		if (!stricmp (name, cmd))
		{
			cmdlineParts[i] = NULL;
			return true;
		}
	}
	return false;
}


static void PushCmdline (void)
{
	int		i;
	char	*cmd;

	for (i = 0; i < cmdlineNumParts; i++)
	{
		cmd = cmdlineParts[i];
		if (!cmd) continue;				// already removed
		Cbuf_AddText (va("%s\n", cmd));
		cmdlineParts[i] = NULL;			// just in case
	}
}


/*-----------------------------------------------------------------------------
	Initialization
-----------------------------------------------------------------------------*/

static cvar_t	*nointro;


void QCommon_Init (char *cmdline)
{
CVAR_BEGIN(vars)
	CVAR_VAR(com_speeds, 0, 0),
	CVAR_VAR(developer, 0, 0),
	CVAR_VAR(timescale, 1, CVAR_CHEAT),
	CVAR_VAR(timedemo, 0, CVAR_CHEAT),
	CVAR_VAR(nointro, 0, CVAR_NOSET),
	CVAR_FULL(&logfile_active, "logfile", "0", 0),
	CVAR_FULL(&sv_cheats, "cheats", "0", CVAR_SERVERINFO|CVAR_LATCH),
#ifdef DEDICATED_ONLY
	CVAR_VAR(dedicated, 1, CVAR_NOSET)
#else
	CVAR_VAR(dedicated, 0, CVAR_NOSET)
#endif
CVAR_END

	guard(QCommon_Init);

	Com_ResetErrorState ();
	Swap_Init ();
	Mem_Init ();
	Cvar_Init ();

	ParseCmdline (cmdline);			// should be executed before any cvar creation
	Cvar_GetVars (ARRAY_ARG(vars));
	Cvar_Get ("version", STR(VERSION) " " CPUSTRING " " __DATE__ " " BUILDSTRING, CVAR_SERVERINFO|CVAR_NOSET);

	Cbuf_Init ();
	Cmd_Init ();
	Key_Init ();


	Sys_Init ();

	FS_InitFilesystem ();

	FS_LoadGameConfig ();
	Cbuf_Execute ();
	cvar_initialized = 1;			// config executed -- allow cmdline cvars to be modified

	// init commands and vars
	RegisterCommand ("error", Com_Error_f);

	if (DEDICATED)
	{
		RegisterCommand ("quit", Com_Quit);
		linewidth = 80;
	}

	NET_Init ();
	Netchan_Init ();

	SV_Init ();
	if (!DEDICATED) CL_Init ();

	// initialize rand() functions
	srand (Sys_Milliseconds ());

	if (nointro->integer == 0)
	{	// if the user didn't give any commands, run default action
		if (!DEDICATED)
		{
			Cbuf_AddText ("d1\n");
			Cbuf_Execute ();
			if (!Com_ServerState ())		// this alias not exists or not starts demo
				Cvar_ForceSet ("nointro", "1");
		}
		else
		{
			Cbuf_AddText ("dedicated_start\n");
			Cbuf_Execute ();
		}
	}

	cvar_initialized = 2;
	PushCmdline ();
	Com_Printf (S_GREEN"\n====== " APPNAME " Initialized ======\n\n");

	unguard;
}


extern	int	c_traces, c_pointcontents;

//??
#define SV_PROFILE

#ifdef SV_PROFILE

#pragma warning (push)
#pragma warning (disable : 4035)
#pragma warning (disable : 4715)
inline unsigned cycles (void)	  // taken from UT
{
	__asm
	{
#if 0
		xor   eax,eax	          // Required so that VC++ realizes EAX is modified.
		_emit 0x0F		          // RDTSC  -  Pentium+ time stamp register to EDX:EAX.
		_emit 0x31		          // Use only 32 bits in EAX - even a Ghz cpu would have a 4+ sec period.
		xor   edx,edx	          // Required so that VC++ realizes EDX is modified.
#else
		rdtsc
#endif
	}
}
#pragma warning (pop)

#endif


void QCommon_Frame (int msec)
{
	char	*s;
	int		time_before, time_between, time_after;
	int		realMsec;
	float	msecf;

	guard(QCommon_Frame);

	realMsec = msec;
	//?? ignore timescale in multiplayer and timedemo in non-demo mode
	if (timedemo->integer)
		msecf = 100.0f / timedemo->integer;		// sv_fps ?
	else // if (timescale->value)
		msecf = msec * timescale->value;
	if (msecf < 0) msecf = 0;					// no reverse time

	c_traces = 0;
	c_pointcontents = 0;

	if (DEDICATED)
		do
		{
			s = Sys_ConsoleInput ();
			if (s) Cbuf_AddText (va("%s\n",s));
		} while (s);
	Cbuf_Execute ();

	if (com_speeds->integer)
	{
		time_before = Sys_Milliseconds ();
		time_before_game = -1;
	}

//if (!Cvar_VariableInt("sv_block"))
	SV_Frame (msecf);

	if (com_speeds->integer)
		time_between = Sys_Milliseconds ();

	if (!DEDICATED)
	{
		CL_Frame (msecf, realMsec);

		if (com_speeds->integer)
		{
			int		all, sv, gm, cl, rf;
			static int old_gm, old_tr, old_pc;

			time_after = Sys_Milliseconds ();
			all = time_after - time_before;
			sv = time_between - time_before;
			cl = time_after - time_between;
			gm = time_after_game - time_before_game;
			if (time_before_game > 0)	// have a valid game frame
			{
				old_gm = gm;
				old_tr = c_traces;
				old_pc = c_pointcontents;
			}
			rf = time_after_ref - time_before_ref;
			sv -= gm;
			cl -= rf;
			re.DrawTextRight (va("sv:%2d gm:%2d (%2d) cl:%2d rf:%2d all:%2d",
					sv, gm, old_gm, cl, rf, all), RGB(1, 0.8, 0.3));
			re.DrawTextRight (va("tr: %4d (%4d) pt: %4d (%4d)",
					c_traces, old_tr, c_pointcontents, old_pc), RGB(1, 0.8, 0.3));

#ifdef SV_PROFILE
			if (1)	//?? cvar
			{
				extern int prof_times[256];
				extern int prof_counts[256];
				static char *names[] = {"LinkEdict", "UnlinkEdict", "AreaEdicts", "Trace",
					"PtContents", "Pmove", "ModIndex", "ImgIndex", "SndIndex",
					"Malloc", "Free", "FreeTags"};
				static unsigned counts[12], times[12];
				static unsigned cyc, lastCycles, lastMsec;
				int		i, msec;		// NOTE: msec overrided
				float	scale;

				cyc = cycles (); msec = Sys_Milliseconds ();
				if (msec != lastMsec)
					scale = (cyc - lastCycles) / (msec - lastMsec);
				else
					scale = 1e10;
				if (!scale) scale = 1;
				for (i = 0; i < ARRAY_COUNT(names); i++)
				{
					re.DrawTextLeft (va("%s: %3d %4f", names[i], counts[i], times[i]/scale), RGB(1, 0.8, 0.3));
					if (time_before_game > 0)
					{
						counts[i] = prof_counts[i];
						times[i] = prof_times[i];
						prof_counts[i] = prof_times[i] = 0;
					}
				}
				lastCycles = cyc; lastMsec = msec;
			}
#endif
		}
	}

	unguard;
}


void QCommon_Shutdown (void)
{
	if (logfile)
	{
		fclose (logfile);
		logfile = NULL;
	}
}
