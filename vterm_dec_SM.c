
#include "vterm.h"
#include "vterm_private.h"
#include "vterm_csi.h"
#include "vterm_buffer.h"
#include "vterm_cursor.h"
#include "mouse_driver.h"

/*
    Info on bracketed paste can be found here:

    ESC [ ? x h

    https://cirw.in/blog/bracketed-paste
*/

/* Interpret DEC SM (set mode) */
void
interpret_dec_SM(vterm_t *vterm, int param[], int pcount)
{
    vterm_desc_t    *v_desc = NULL;
    int             i;
    int             idx;

    if(pcount == 0) return;

    // set the vterm description buffer selector
    idx = vterm_buffer_get_active(vterm);
    v_desc = &vterm->vterm_desc[idx];

    for(i = 0; i < pcount; i++)
    {
        // enable auto-wrap mode (default is auto-wrap)
        if(param[i] == 7)
        {
            v_desc->buffer_state &= ~STATE_NO_WRAP;
            continue;
        }

        /* civis is actually "normal" for rxvt */
        if(param[i] == 25)
        {
            v_desc->buffer_state &= ~STATE_CURSOR_INVIS;
            continue;
        }

        // start alt buffer
        // CSI handler:  ESC [ ? 47 h
        if(param[i] == 47)
        {
            // check to see if we're already using the ALT buffer
            if(idx == VTERM_BUFFER_ALT) continue;

            vterm_buffer_set_active(vterm, VTERM_BUFFER_ALT);
            continue;
        }

        /*
            set mouse mode VT200 and installs the generic mouse driver
            this is a varation of the X10 protocol with tracking
        */
        if(param[i] == 1000)
        {
            mouse_driver_init(vterm);
            mouse_driver_start(vterm);

            vterm->mouse |= VTERM_MOUSE_VT200;
            vterm->mouse_driver = mouse_driver_default;
            continue;
        }

        if(param[i] == 1001)
        {
            vterm->mouse |= VTERM_MOUSE_HIGHLIGHT;
            continue;
        }

        /*
            set mouse mode SGR and installs the generic mouse driver
            the most ubiquitous mouse mode handling > 223 x 223 coord
        */
        if(param[i] == 1006)
        {
            mouse_driver_init(vterm);
            mouse_driver_start(vterm);

            vterm->mouse |= VTERM_MOUSE_SGR;
            vterm->mouse_driver = mouse_driver_default;
            continue;
        }

        /*
            turn on alternate scroll mode.  this causes cursor up and
            cursor down to be emitted instead of the actual button
            values when the wheel mouse is used.
        */
        if(param[i] == 1007)
        {
            vterm->mouse |= VTERM_MOUSE_ALTSCROLL;
            continue;
        }

        /*
            not reall well documented but this code turns on the
            meta key.  see terminfo(5).  we just pass this along
            to ncurses via the meta() function.
        */
        if(param[i] == 1034)
        {
            /*
                according to man pages for meta() "The window
                argument, win, is always ignored."
            */
            meta(NULL, TRUE);

            return;
        }

        /*
            nearly identical to ESC [ ? 47 h except it calls for
            saving the cursor first then switching to alt buffer.
        */
        if(param[i] == 1049)
        {
            vterm_cursor_save(vterm);

            // check to see if we're already using the ALT buffer
            if(idx != VTERM_BUFFER_ALT)
                vterm_buffer_set_active(vterm, VTERM_BUFFER_ALT);

            continue;
        }

        // stub for enabling bracketed paste
        if(param[i] == 2004)
        {
            // todo

            continue;
        }
    }
}
