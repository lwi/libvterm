/*
LICENSE INFORMATION:
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License (LGPL) as published by the Free Software Foundation.

Please refer to the COPYING file for more information.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

Copyright (c) 2004 Bruno T. C. de Oliveira
*/

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "vterm.h"
#include "vterm_private.h"
#include "vterm_csi.h"
#include "vterm_render.h"
#include "vterm_misc.h"

static int
vterm_interpret_esc_normal(vterm_t *vterm);

static int
vterm_interpret_esc_xterm_osc(vterm_t *vterm);

static int
vterm_interpret_esc_xterm_dsc(vterm_t *vterm);

static bool
validate_csi_escape_suffix(char c);

static bool
validate_xterm_escape_suffix(char c);

void
vterm_escape_start(vterm_t *vterm)
{
    vterm->state |= STATE_ESCAPE_MODE;

    // zero out the escape buffer just in case
    vterm->esbuf_len = 0;
    vterm->esbuf[0] = '\0';

    vterm->esc_handler = NULL;

    return;
}

void
vterm_escape_cancel(vterm_t *vterm)
{
    vterm->state &= ~STATE_ESCAPE_MODE;

    // zero out the escape buffer for the next run
    vterm->esbuf_len = 0;
    vterm->esbuf[0] = '\0';

    vterm->esc_handler = NULL;

    return;
}

void
vterm_interpret_escapes(vterm_t *vterm)
{
    char    firstchar;
    char    lastchar;
#ifdef _DEBUG
    FILE            *f_debug;
    char            debug_file[NAME_MAX];
#endif

    firstchar = vterm->esbuf[0];
    lastchar = vterm->esbuf[vterm->esbuf_len - 1];

#ifdef _DEBUG
    snprintf(debug_file,(sizeof(debug_file) - 1),
        "/tmp/libvterm-%d-esc-log",vterm->child_pid);

    f_debug = fopen(debug_file,"w");
    fwrite(vterm->esbuf,sizeof(char),vterm->esbuf_len,f_debug);
    fclose(f_debug);
#endif

    // too early to do anything
    if(!firstchar) return;

    // interpret ESC-M as reverse line-feed
    if(firstchar == 'M')
    {
        vterm_scroll_up(vterm);
        vterm_escape_cancel(vterm);

        return;
    }

    if(firstchar == '7' )
    {
        interpret_csi_SAVECUR(vterm, 0, 0);
        vterm_escape_cancel(vterm);
    }

    if(firstchar == '8' )
    {
        interpret_csi_RESTORECUR(vterm, 0, 0);
        vterm_escape_cancel(vterm);
    }

    // if it's not these, we don't have code to handle it.
    if(firstchar != '[' && firstchar != ']' && firstchar != 'P' )
    {
        vterm_escape_cancel(vterm);
        return;
    }

    // looks like an complete xterm Operating System Command
    if(firstchar == ']' && validate_xterm_escape_suffix(lastchar))
    {
        vterm->esc_handler = vterm_interpret_esc_xterm_osc;
    }

    // we have a complete csi escape sequence: interpret it
    if(firstchar == '[' && validate_csi_escape_suffix(lastchar))
       {
        vterm->esc_handler = vterm_interpret_esc_normal;
       }

    // DCS sequence - starts in P and ends in Esc backslash
    if( firstchar == 'P'
        && vterm->esbuf_len > 2
        && vterm->esbuf[vterm->esbuf_len - 2] == '\x1B'
        && lastchar=='\\' )
    {
        vterm->esc_handler = vterm_interpret_esc_xterm_dsc;
    }

    // if an escape handler has been configured, run it.
    if(vterm->esc_handler != NULL)
    {
        vterm->esc_handler(vterm);
        vterm_escape_cancel(vterm);

        return;
    }


    return;
}

int
vterm_interpret_esc_xterm_osc(vterm_t *vterm)
{
    /*
        TODO

        this function basically does nothing right now but it would be quite
        easy to parse the data and set the "window" title supplied by the
        Xterm OSC information.
    */

    const char  *p;

    p = vterm->esbuf + 1;

    // parse numeric parameters
    if(isdigit(*p))
    {
        switch(*p)
        {
            // Change Icon Name and Window Title
            case 0:
            {
                break;
            }

            // Change Icon Name
            case 1:

            // Change Window Title
            case 2:
            {
                /*
                    TODO:  for now we will simply return the number of
                    characters processed.  by not returning 0 or -1 then
                    calling function will keep us in ESCAPE MODE.

                    In the future we could actually store the string for
                    the user.
                */
                break;

                break;
            }

            default:
                break;
         }
    }

    return 0;
}

int
vterm_interpret_esc_xterm_dsc(vterm_t *vterm)
{
    /*
        TODO

        accept DSC (ESC-P) sequences from xterm but don't do anything
        with them - just toss them to /dev/null for now.
    */

    const char  *p;

    p = vterm->esbuf + 1;

    if( *p=='+'
        && *(p+1)=='q'
        && isxdigit( *(p+2) )
        && isxdigit( *(p+3) )
        && isxdigit( *(p+4) )
        && isxdigit( *(p+5) ) )
        {
        /* we have a valid code, and we'll promptly ignore it */
        }

    return 0;
}

int
vterm_interpret_esc_normal(vterm_t *vterm)
{
    static int  csiparam[MAX_CSI_ES_PARAMS];
    int         param_count = 0;
    const char  *p;
    char        verb;

    p = vterm->esbuf + 1;
    verb = vterm->esbuf[vterm->esbuf_len - 1];

    // parse numeric parameters
    while (isdigit(*p) || *p == ';' || *p == '?')
    {
        if(*p == '?')
        {
            p++;
            continue;
        }

        if(*p == ';')
        {
            if(param_count >= MAX_CSI_ES_PARAMS) return -1;    // too long!
            csiparam[param_count++] = 0;
        }
        else
        {
            if(param_count == 0) csiparam[param_count++] = 0;

            // increaase order of prev digit (10s column, 100s column, etc...)
            csiparam[param_count-1] *= 10;
            csiparam[param_count-1] += *p - '0';
        }

        p++;
    }

    // delegate handling depending on command character (verb)
    switch (verb)
    {
        case 'm':
        {
            interpret_csi_SGR(vterm,csiparam,param_count);
            break;
        }

        case 'l':
        {
            interpret_dec_RM(vterm,csiparam,param_count);
            break;
        }

        case 'h':
        {
            interpret_dec_SM(vterm,csiparam,param_count);
            break;
        }

        case 'J':
        {
            interpret_csi_ED(vterm,csiparam,param_count);
            break;
        }

        case 'H':
        case 'f':
        {
            interpret_csi_CUP(vterm,csiparam,param_count);
            break;
        }

        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'e':
        case 'a':
        case 'd':
        case '`':
        {
            interpret_csi_CUx(vterm,verb,csiparam,param_count);
            break;
        }

        case 'K':
        {
            interpret_csi_EL(vterm,csiparam,param_count);
            break;
        }

        case '@':
        {
            interpret_csi_ICH(vterm,csiparam,param_count);
            break;
        }

        case 'P':
        {
            interpret_csi_DCH(vterm,csiparam,param_count);
            break;
        }

        case 'L':
        {
            interpret_csi_IL(vterm,csiparam,param_count);
            break;
        }

        case 'M':
        {
            interpret_csi_DL(vterm,csiparam,param_count);
            break;
        }

        case 'X':
        {
            interpret_csi_ECH(vterm,csiparam,param_count);
            break;
        }

        case 'r':
        {
            interpret_csi_DECSTBM(vterm,csiparam,param_count);
            break;
        }

        case 's':
        {
            interpret_csi_SAVECUR(vterm,csiparam,param_count);
            break;
        }

        case 'u':
        {
            interpret_csi_RESTORECUR(vterm,csiparam,param_count);
            break;
        }

#ifdef _DEBUG
        default:
        {
            fprintf(stderr, "Unrecogized CSI: <%s>\n",
                vterm->esbuf);
            break;
        }
#endif
    }

    return 0;
}

bool
validate_csi_escape_suffix(char c)
{
   if(c >= 'a' && c <= 'z') return TRUE;
   if(c >= 'A' && c <= 'Z') return TRUE;
   if(c == '@') return TRUE;
   if(c == '`') return TRUE;

   return FALSE;
}

bool
validate_xterm_escape_suffix(char c)
{
    if(c == '\x07') return TRUE;
    if(c == '\x96') return TRUE;

    return FALSE;
}
