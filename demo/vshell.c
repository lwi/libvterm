#define _XOPEN_SOURCE 500               // needed for wcswidth()
#define _XOPEN_SOURCE_EXTNEDED
#define _GNU_SOURCE

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <wchar.h>
#include <termios.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

// #include <sys/ioctl.h>

#ifdef __linux__
#include <ncursesw/curses.h>
#endif

#ifdef __FreeBSD__
#include <stdarg.h>
#include <ncurses/ncurses.h>
#endif

#if defined(__APPLE__) && defined(__MACH__)
#include <ncursesw/ncurses.h>
#endif

#include "ctimer.h"
#include "../vterm.h"
#include "../stringv.h"
#include "../macros.h"
#include "../utlist.h"

// this is necessary for FreeBSD in some compilation scenarios
#ifndef CCHARW_MAX
#define CCHARW_MAX  5
#endif

#define WBUF_MAX    128         // maximum size of wchar string buffer

#define WC_SYMS_X(i, s)    i,
enum
{
#include "wc_syms.def"
};
#undef WC_SYMS_X

#define WC_SYMS_X(i, s)    s,
static wchar_t wc_syms[] = {
#include "wc_syms.def"
};
#undef WC_SYMS_X

static cchar_t *cc_syms;

#define TERM_STATE_ALT          (1 << 0)
#define TERM_STATE_HISTORY      (1 << 4)
#define TERM_STATE_EXECV        (1 << 6)
#define TERM_STATE_DIRTY        (1 << 7)

#define FLAG_PAINT_ALL          1

#define VSHELL_FLAG_HELP        (1 << 0)

#define HELP_WIDTH              40
#define HELP_HEIGHT             21

short   color_table[] =
            {
                COLOR_BLACK, COLOR_RED, COLOR_GREEN,
                COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA,
                COLOR_CYAN, COLOR_WHITE
            };

int      color_count = ARRAY_SZ(color_table);


struct _color_mtx_s
{
    int     fg;
    int     bg;
};

typedef struct _color_mtx_s     color_mtx_t;

typedef struct _vshell_s        vshell_t;

typedef struct _vpane_s         vpane_t;

struct _vpane_s
{
    int             cursor_pos;
    vterm_t         *vterm;
    WINDOW          *term_wnd;
    int             x;              // x coordinate of top left
    int             y;              // y coordinate of top left
    int             width;          // width of pane
    int             height;         // height of pane
    unsigned int    state;
    int             scroll_amount;  // tmp value indicating lines to scroll

    ctimer_t        *fps_timer;
    struct timeval  fps_interval;
    ssize_t         bytes;
    ssize_t         bytes_buffered;

    vpane_t         *next;
    vpane_t         *prev;

    void            (*render)       (vpane_t *, int);
    void            (*kinput)       (vshell_t *, int32_t);
};

// global struct
struct _vshell_s
{
    int             argc;
    char            **argv;

    char            *exec_path;
    char            **exec_argv;

    unsigned int    vshell_flags;

    WINDOW          *canvas;
    WINDOW          *help;
    int             screen_w;
    int             screen_h;
    color_mtx_t     *color_mtx;

    int             active_pane;        //  1s-based index of the active pane
    int             pane_count;         //  total number of panes

    // term specific data stores
    uint32_t        vterm_flags;

    vpane_t         *vpane_list;
};

// prototypes
void        vshell_parse_cmdline(vshell_t *vshell);
void        vshell_print_help(void);
void        vshell_color_init(vshell_t *vshell);
void        vshell_help_init(vshell_t *vshell);

void        vshell_create_pane(vshell_t *vshell, unsigned int state);
void        vshell_destroy_pane(vshell_t *vshell, int pane_id);
vpane_t*    vshell_get_pane(vshell_t *vshell, int pane_id);

void        vshell_update_canvas(vshell_t *vshell, int flags);
int32_t     vshell_get_key(vshell_t *vshell);
int         vshell_resize(vshell_t *vshell);

void        vshell_render_normal(vpane_t *vpane, int flags);
void        vshell_render_history(vpane_t *vpane, int flags);
void        vshell_kinput_normal(vshell_t *vshell, int32_t keystroke);
void        vshell_kinput_history(vshell_t *vshell, int32_t keystroke);

void        vshell_hook(vterm_t *vterm, int event, void *anything);
short       vshell_pair_selector(vterm_t *vterm, short fg, short bg);

cchar_t*    cchar_alloc(wchar_t *wcs, attr_t attrs, short color_pair);
void        mvwadd_wchars(WINDOW *win, int row_y, int col_x, wchar_t *wchstr);
void        wchar_box(WINDOW *win, int colors, int x, int y,
                int width, int height);

volatile sig_atomic_t   sio_count = 0;

int main(int argc, char **argv)
{
    vshell_t            *vshell;
    vpane_t             *vpane;
    vpane_t             *tmp1;
    vpane_t             *tmp2;
    int32_t             keystroke;
    char                *locale;
    struct pollfd       fds[1];
    int                 pty_fd;
    int                 signum;
    int                 sig_fd;
    int                 retval;
    int                 fds_ready;


    vshell = (vshell_t *)calloc(1, sizeof(vshell_t));
    cc_syms = (cchar_t *)calloc(ARRAY_SZ(wc_syms), sizeof(cchar_t));

    locale = getenv("LANG");
    if(locale == NULL) locale = "en_US.UTF-8";

	setlocale(LC_ALL, locale);

    initscr();
    noecho();
    raw();
    curs_set(0);                        // hide cursor
    keypad(stdscr, TRUE);
    scrollok(stdscr, FALSE);
    getmaxyx(stdscr, vshell->screen_h, vshell->screen_w);

    vshell->argc = argc;
    vshell->argv = argv;

    vshell_parse_cmdline(vshell);

    vshell_color_init(vshell);

    /*
        this is the "master window" which everything gets drawn on.  it's a
        fullscreen window which resizes with the host terminal.  this is
        also where the "window frame", decorations, and status information
        are drawn.
    */
    vshell->canvas = newwin(vshell->screen_h, vshell->screen_w, 0, 0);
    scrollok(vshell->canvas, FALSE);
    // nodelay(vshell->canvas, TRUE);
    nodelay(vshell->canvas, FALSE);
    keypad(vshell->canvas, TRUE);

    // create the first pane and vterm instance
    if(vshell->exec_path != NULL)
        vshell_create_pane(vshell, TERM_STATE_DIRTY | TERM_STATE_EXECV);
    else
        vshell_create_pane(vshell, TERM_STATE_DIRTY);


    vshell->active_pane = 1;

    vshell_help_init(vshell);

    // render frame
    vshell_update_canvas(vshell, FLAG_PAINT_ALL);

    vpane = vshell_get_pane(vshell, vshell->active_pane);
    sig_fd = vterm_set_async(vpane->vterm);
    fds[0].fd = sig_fd;
    fds[0].events = POLLIN;

    /*
        keep reading keypresses from the user and passing them to
        the terminal;  also, redraw the terminal to the window at each
        iteration
    */
    keystroke = '\0';
    while(TRUE)
    {
        // all of the terminals have exited
        if(vshell->pane_count == 0) break;

        fds_ready = poll(fds, 1, 10);

        if(fds_ready == -1 && errno == EINTR) continue;

        while(fds_ready > 0)
        {
            retval = poll(fds, 1, 10);

            if(retval == -1 && errno == EINTR) continue;
            if(retval > 0) fds_ready += retval;

            retval = read(sig_fd, &signum, sizeof(signum));

            if(retval == -1)
            {
                if(errno == EINTR) continue;
                if(errno == EAGAIN) break;
            }

            // no more signals in the pipe
            if(retval == 0)
            {
                fds_ready--;
                if(fds_ready < 0)
                {
                    fds_ready = 0;
                    continue;
                }
            }

            CDL_FOREACH_SAFE(vshell->vpane_list, vpane, tmp1, tmp2)
            {
                do
                {
                    retval = poll(fds, 1, 10);

                    if(retval == -1 && errno == EINTR) continue;
                    if(retval > 0) fds_ready += retval;

                    for(;;)
                    {
                        retval = read(sig_fd, &signum, sizeof(signum));

                        if(retval == -1)
                        {
                            if(errno == EINTR) continue;
                            break;
                        }

                        if(retval > 0) continue;

                        break;
                    }

                    vpane->bytes = vterm_read_pipe(vpane->vterm, 10);
                    vpane->bytes_buffered += vpane->bytes;
                }
                while(vpane->bytes > 0);

                // if(ctimer_compare(vpane->fps_timer, &vpane->fps_interval) > 0)
                // {
                //    if(vpane->bytes_buffered > 0)
                //    {
                        vpane->render(vpane, 0);
                        vpane->bytes_buffered = 0;
                //     }

                //    ctimer_reset(vpane->fps_timer);
                //}

                if(vpane->bytes == -1)
                {
                    vshell_destroy_pane(vshell, vshell->active_pane);
                    if(vshell->pane_count == 0) break;

                    vshell_resize(vshell);
                }
            }
        }

        if(vshell->pane_count == 0) break;

        keystroke = vshell_get_key(vshell);

        // interrupted by a signal
        if(keystroke == -1 && errno == EINTR)
        {
            continue;
        }

        // handle special events like resize first
        if(keystroke == KEY_RESIZE)
        {
            vshell_resize(vshell);
            continue;
        }

        // alt H
        if(keystroke == 0x1b48)
        {
            if(vshell->vshell_flags & VSHELL_FLAG_HELP)
                vshell->vshell_flags &= ~VSHELL_FLAG_HELP;
            else
                vshell->vshell_flags |= VSHELL_FLAG_HELP;

            vshell_update_canvas(vshell, FLAG_PAINT_ALL);
            // raise(SIGIO);

            continue;
        }

        // alt w
        if(keystroke == 0x1b77)
        {
            vshell->active_pane++;
            if(vshell->active_pane > vshell->pane_count)
            {
                vshell->active_pane = 1;
            }
            vshell_update_canvas(vshell, FLAG_PAINT_ALL);

            continue;
        }

        // alt +
        // create a new pane
        if(keystroke == 0x1b2b)
        {
            vshell_create_pane(vshell, 0);
            vshell->active_pane = vshell->pane_count;
            vpane = vshell_get_pane(vshell, vshell->active_pane);
            vterm_set_async(vpane->vterm);
            vshell_resize(vshell);

            continue;
        }

        // alt -
        // terminate and close active pane
        if(keystroke == 0x1b2d)
        {
            vshell_destroy_pane(vshell, vshell->active_pane);
            vpane = vshell_get_pane(vshell, vshell->active_pane);
            vpane->state |= TERM_STATE_DIRTY;
            vshell_resize(vshell);

            continue;
        }

        // pass the keystroke into the active keyboard handler
        vpane = vshell_get_pane(vshell, vshell->active_pane);

        if(vpane != NULL)
        {
            vpane->kinput(vshell, keystroke);
        }
    }

    endwin();

    CDL_FOREACH_SAFE(vshell->vpane_list, vpane, tmp1, tmp2)
    {
        vshell_destroy_pane(vshell, 1);
    }

    // print some debug info
    printf("%s\n\r", locale);
    printf("ncurses = %d.%d (%d)\n\r", NCURSES_VERSION_MAJOR,
        NCURSES_VERSION_MINOR, NCURSES_VERSION_PATCH);
    fflush(NULL);

    exit(0);
}


void
vshell_create_pane(vshell_t *vshell, unsigned int state)
{
    static int      first_init = 1;
    vpane_t         *vpane;

    if(vshell == NULL) return;

    vpane = CALLOC_PTR(vpane);
    vpane->fps_timer = ctimer_create();

    // create pane in the dirty state so it will alwasy get rendered initially
    vpane->state = state;

    // 1,000,000 usec = 1 sec
    // effective 30000 usec = 30 fps
    vpane->fps_interval.tv_sec = 0;
    vpane->fps_interval.tv_usec = 30000;

    vpane->render = vshell_render_normal;
    vpane->kinput = vshell_kinput_normal;

    CDL_APPEND(vshell->vpane_list, vpane);
    vshell->pane_count++;

    vpane->width = (vshell->screen_w / vshell->pane_count);
    vpane->height = vshell->screen_h;
    vpane->x = vpane->width * (vshell->pane_count - 1);
    vpane->y = 0;

    vpane->term_wnd = newwin(vpane->height - 2, vpane->width - 2,
        vpane->y + 1, vpane->x + 1);
    nodelay(vpane->term_wnd, TRUE);
    keypad(vpane->term_wnd, TRUE);
    // cbreak();

    // set term window colors to black on white
    wattrset(vpane->term_wnd, COLOR_PAIR(7 * 8 + 7 - 0));

    vpane->vterm = vterm_alloc();

    // set the exec path if specified
    if(first_init == 1)
    {
        if((vshell->exec_path != NULL) && (vpane->state & TERM_STATE_EXECV))
        {
            vterm_set_exec(vpane->vterm,
                vshell->exec_path, vshell->exec_argv);
        }

        first_init = 0;
    }

    vterm_init(vpane->vterm, vpane->width - 2, vpane->height - 2,
        vshell->vterm_flags);
    vterm_set_userptr(vpane->vterm, (void *)vshell);

    vterm_set_colors(vpane->vterm, COLOR_WHITE, COLOR_BLACK);
    vterm_wnd_set(vpane->vterm, vpane->term_wnd);

    // this illustrates how to install an event hook
    vterm_set_event_mask(vpane->vterm, VTERM_MASK_BUFFER_ACTIVATED);
    vterm_install_hook(vpane->vterm, vshell_hook);

    return;
}

vpane_t*
vshell_get_pane(vshell_t *vshell, int pane_id)
{
    vpane_t     *vpane = NULL;
    int         i = 1;

    if(vshell == NULL) return NULL;
    if(vshell->pane_count == 0) return NULL;
    if(pane_id > vshell->pane_count) return NULL;

    if(vshell->pane_count > 0)
    {
        CDL_FOREACH(vshell->vpane_list, vpane)
        {
            if(i == pane_id) break;

            i++;
        }
    }

    return vpane;
}

void
vshell_destroy_pane(vshell_t *vshell, int pane_id)
{
    vpane_t     *vpane;

    vpane = vshell_get_pane(vshell, pane_id);

    vterm_destroy(vpane->vterm);
    CDL_DELETE(vshell->vpane_list, vpane);
    vshell->pane_count--;

    if(vshell->active_pane == pane_id)
    {
        if(vshell->pane_count > 0)
            vshell->active_pane = 1;
        else
            vshell->active_pane = 0;
    }

    ctimer_destroy(vpane->fps_timer);

    free(vpane);

    return;
}


int32_t
vshell_get_key(vshell_t *vshell)
{
    int32_t     keystroke;
    int         ch;

    ch = wgetch(vshell->canvas);

    if(ch == -1)
    {
        if(errno == EINTR) return -1;
    }

    keystroke = ch;

    if(ch == 0x1b)
    {
        nodelay(vshell->canvas, TRUE);
        for(;;)
        {
            ch = wgetch(vshell->canvas);
            // ch = getch();
            if(ch == -1)
            {
                if(errno == EINTR) continue;

                break;
            }

            keystroke = keystroke << 8;
            keystroke |= ch;
        }
        nodelay(vshell->canvas, FALSE);
    }

    return keystroke;
}

void
vshell_update_canvas(vshell_t *vshell, int flags)
{
    vpane_t         *vpane;
    wchar_t         wbuf[WBUF_MAX];
    char            *mode;
    int             len;
    int             frame_colors;
    int             scroll_colors = 0;
    int             history_sz;
    float           total_height;
    float           pos;
    int             i = 0;
    short           fg;                     // fg color of border
    short           bg;                     // bg color of border 


    CDL_FOREACH(vshell->vpane_list, vpane)
    {
        i++;

        if(!(flags & FLAG_PAINT_ALL))
        {
            if(!(vpane->state & TERM_STATE_DIRTY)) continue;
        }

        if(vpane->state & TERM_STATE_ALT)
        {
            fg = (vshell->active_pane == i) ? COLOR_WHITE : COLOR_RED;
            bg = (vshell->active_pane == i) ? COLOR_RED : COLOR_BLACK;
            mode = "ALTERNATE";
        }
        else
        {
            fg = (vshell->active_pane == i) ? COLOR_WHITE : COLOR_BLUE;
            bg = (vshell->active_pane == i) ? COLOR_BLUE : COLOR_BLACK;
            mode = "NORMAL";
        }

        if(vpane->state & TERM_STATE_HISTORY)
        {
            fg = (vshell->active_pane == i) ? COLOR_WHITE : COLOR_MAGENTA;
            bg = (vshell->active_pane == i) ? COLOR_MAGENTA : COLOR_BLACK;
            mode = "HISTORY";
        }

        frame_colors = vshell_pair_selector(NULL, fg, bg);

        wattron(vshell->canvas, WA_BOLD);
        wchar_box(vshell->canvas, frame_colors, vpane->x, vpane->y,
            vpane->width, vpane->height);
        wattroff(vshell->canvas, WA_BOLD);

        if(vshell->active_pane == i)
        {
            if(vpane->state & TERM_STATE_HISTORY)
            {
                history_sz = vterm_get_history_size(vpane->vterm);

                len = swprintf(wbuf, WBUF_MAX,
                    L"[ Active | %s | %04d / %04d | Help = < Alt H > ]", mode,
                    vpane->cursor_pos + vpane->height, history_sz);
            }
            else
            {
                len = swprintf(wbuf, WBUF_MAX,
                    L"[ Active | %s | Help < Alt H > ]", mode);
            }

            wattron(vshell->canvas, WA_BOLD);
            mvwadd_wchars(vshell->canvas,
                0, vpane->x + (vpane->width >> 1) - (len >> 1), wbuf);
            wattroff(vshell->canvas, WA_BOLD);
        }
    }

    // configure cchar for cblock
    scroll_colors = vshell_pair_selector(NULL, COLOR_WHITE, COLOR_BLUE);
    setcchar(&cc_syms[WCS_CKBOARD_MEDIUM], &wc_syms[WCS_CKBOARD_MEDIUM],
        WA_NORMAL, scroll_colors, NULL);
    i = 0;

    CDL_FOREACH(vshell->vpane_list, vpane)
    {
        i++;

        if(!(flags & FLAG_PAINT_ALL))
        {
            if(!(vpane->state & TERM_STATE_DIRTY)) continue;
        }

        if(vpane->state & TERM_STATE_HISTORY)
        {
            // configure arrows
            setcchar(&cc_syms[WCS_UARROW], &wc_syms[WCS_UARROW],
                WA_NORMAL, scroll_colors, NULL);
            setcchar(&cc_syms[WCS_DARROW], &wc_syms[WCS_DARROW],
                WA_NORMAL, scroll_colors, NULL);

            mvwvline_set(vshell->canvas, 2, vpane->x + (vpane->width - 1),
                &cc_syms[WCS_CKBOARD_MEDIUM], vpane->height - 4);

            // draw arrows
            mvwadd_wch(vshell->canvas, 1, vpane->x + (vpane->width - 1),
                &cc_syms[WCS_UARROW]);
            mvwadd_wch(vshell->canvas, vpane->height - 2,
                vpane->x + (vpane->width - 1), &cc_syms[WCS_DARROW]);

            // draw block
            setcchar(&cc_syms[WCS_BLOCK], &wc_syms[WCS_BLOCK],
                WA_REVERSE, scroll_colors, NULL);

            history_sz = vterm_get_history_size(vpane->vterm);
            total_height = (float)history_sz - ((float)vpane->height - 2);
            pos = 1.0 - (float)vpane->cursor_pos / total_height;
            pos *= (vpane->height - 5);

            mvwadd_wch(vshell->canvas, 2 + (int)pos,
                vpane->x + (vpane->width) - 1, &cc_syms[WCS_BLOCK]);
        }
    }

    // blit each window to the canvas
    if(flags & FLAG_PAINT_ALL)
    {
        CDL_FOREACH(vshell->vpane_list, vpane)
        {
            overwrite(vpane->term_wnd, vshell->canvas);
            vpane->state &= ~TERM_STATE_DIRTY;
        }
    }
    else
    {
        CDL_FOREACH(vshell->vpane_list, vpane)
        {
            if(vpane->state & TERM_STATE_DIRTY)
            {
                overwrite(vpane->term_wnd, vshell->canvas);
                vpane->state &= ~TERM_STATE_DIRTY;
            }
        }
    }

    if(vshell->vshell_flags & VSHELL_FLAG_HELP)
    {
        mvwin(vshell->help,
            (vshell->screen_h - HELP_HEIGHT) >> 1,
            (vshell->screen_w - HELP_WIDTH) >> 1);
        overwrite(vshell->help, vshell->canvas);
    }

    // blit the window to the canvas
    wnoutrefresh(vshell->canvas);
    doupdate();

    return;
}

int
vshell_resize(vshell_t *vshell)
{
    vpane_t     *vpane;
    int         pane_id = 1;

    getmaxyx(stdscr, vshell->screen_h, vshell->screen_w);

    wresize(vshell->canvas, vshell->screen_h, vshell->screen_w);

    CDL_FOREACH(vshell->vpane_list, vpane)
    {
        vpane->width = (vshell->screen_w / vshell->pane_count);
        vpane->height = vshell->screen_h;
        vpane->x = vpane->width * (pane_id - 1);
        vpane->y = 0;

        wresize(vpane->term_wnd, vpane->height - 2, vpane->width - 2);
        mvwin(vpane->term_wnd, vpane->y + 1, vpane->x + 1);

        vterm_resize(vpane->vterm, vpane->width - 2, vpane->height - 2);

        pane_id++;
    }

    vshell_update_canvas(vshell, FLAG_PAINT_ALL);

    return 0;
}

/*
    this illustarates a callback "event hook".  a simple switch
    statement is all that is needed to "listen" for the event
    you want to trigger on.
*/
void
vshell_hook(vterm_t *vterm, int event, void *anything)
{
    vpane_t     *vpane;
    vshell_t    *vshell;
    int         idx;

    if(vterm == NULL) return;       // something went horribly wrong

    vshell = (vshell_t *)vterm_get_userptr(vterm);

    CDL_FOREACH(vshell->vpane_list, vpane)
    {
        if(vpane->vterm == vterm) break;
    }

    switch(event)
    {
        case VTERM_EVENT_BUFFER_ACTIVATED:
        {
            idx = *(int *)anything;

            if(idx == 0)
                vpane->state &= ~TERM_STATE_ALT;
            else
                vpane->state |= TERM_STATE_ALT;

            vshell_update_canvas(vshell, FLAG_PAINT_ALL);
            break;
        }
    }

    return;
}

void
vshell_color_init(vshell_t *vshell)
{
    extern short        color_table[];
    extern int          color_count;
    short               fg, bg;
    int                 i;
    int                 max_colors;
    int                 hard_pair = -1;

    start_color();

    /*
        calculate the size of the matrix.  this is necessary because
        some terminals support 256 colors and we don't want to deal
        with that.
    */
    max_colors = color_count * color_count;

    vshell->color_mtx  = (color_mtx_t *)calloc(1,
        max_colors * sizeof(color_mtx_t));

    for(i = 0;i < max_colors; i++)
    {
        fg = i / color_count;
        bg = color_count - (i % color_count) - 1;

        vshell->color_mtx[i].bg = fg;
        vshell->color_mtx[i].fg = bg;

        /*
            according to ncurses documentation, color pair 0 is assumed to
            be WHITE foreground on BLACK background.  when we discover this
            pair, we need to make sure it gets swapped into index 0 and
            whatever is in index 0 gets put into this location.
        */
        if( vshell->color_mtx[i].fg == COLOR_WHITE &&
            vshell->color_mtx[i].bg == COLOR_BLACK )
        {
            hard_pair = i;
        }
    }

    /*
        if hard_pair is no longer -1 then we found the "hard pair"
        during our enumeration process and we need to do the swap.
    */
    if(hard_pair != -1)
    {
        fg = vshell->color_mtx[0].fg;
        bg = vshell->color_mtx[0].bg;
        vshell->color_mtx[hard_pair].fg = fg;
        vshell->color_mtx[hard_pair].bg = bg;
    }

    for(i = 1; i < max_colors; i++)
    {
        init_pair(i, vshell->color_mtx[i].fg, vshell->color_mtx[i].bg);
    }

    return;
}

short
vshell_pair_selector(vterm_t *vterm, short fg, short bg)
{
    extern short        color_table[];
    extern int          color_count;
    int                 i = 0;

    (void)vterm;        // make compiler happy

    if(fg == COLOR_WHITE && bg == COLOR_BLACK) return 0;

    i = (bg * color_count) + ((color_count - 1) - fg);

    return i;
}

void
vshell_kinput_normal(vshell_t *vshell, int32_t keystroke)
{
    vpane_t     *vpane;

    vpane = vshell_get_pane(vshell, vshell->active_pane);

    // alt-z
    if(keystroke == 0x1b7a)
    {
        vpane->state |= TERM_STATE_HISTORY;
        vpane->render = vshell_render_history;
        vpane->kinput = vshell_kinput_history;

        vpane->render(vpane, 1);

        return;
    }

    if(keystroke != ERR)
    {
        vterm_write_pipe(vpane->vterm, keystroke);

        return;
    }
}


void
vshell_render_normal(vpane_t *vpane, int flags)
{
    vshell_t    *vshell;

    VAR_UNUSED(flags);

    vshell = (vshell_t *)vterm_get_userptr(vpane->vterm);

    vterm_wnd_update(vpane->vterm, -1, 0);
    vpane->state |= TERM_STATE_DIRTY;

    vshell_update_canvas(vshell, 0);

    return;
}

void
vshell_kinput_history(vshell_t *vshell, int32_t keystroke)
{
    vpane_t *vpane;
    int     history_sz;
    int     width;
    int     height;

    vpane = vshell_get_pane(vshell, vshell->active_pane);

    // alt-z
    if(keystroke == 0x1b7a)
    {
        vpane->state &= ~TERM_STATE_HISTORY;
        vpane->cursor_pos = 0;
        vpane->render = vshell_render_normal;
        vpane->kinput = vshell_kinput_normal;

        vpane->render(vpane, 1);

        return;
    }


    if(keystroke == '-')
    {
        history_sz = vterm_get_history_size(vpane->vterm);
        history_sz--;
        vterm_set_history_size(vpane->vterm, history_sz);

        vpane->render(vpane, 1);

        return;
    }

    if(keystroke == '+')
    {
        history_sz = vterm_get_history_size(vpane->vterm);
        history_sz++;
        vterm_set_history_size(vpane->vterm, history_sz);

        vpane->render(vpane, 1);

        return;
    }

    vterm_wnd_size(vpane->vterm, &width, &height);

    if(keystroke == KEY_PPAGE)
    {
        vpane->scroll_amount = height;
        vpane->render(vpane, 1);

        return;
    }

    if(keystroke == KEY_NPAGE)
    {
        height = 0 - height;
        vpane->scroll_amount = height;
        vpane->render(vpane, 1);

        return;
    }

    if(keystroke == KEY_UP)
    {
        vpane->scroll_amount = 1;
        vpane->render(vpane, 1);

        return;
    }

    if(keystroke == KEY_DOWN)
    {
        vpane->scroll_amount = -1;
        vpane->render(vpane, 1);

        return;
    }
}

void
vshell_render_history(vpane_t *vpane, int flags)
{
    vshell_t    *vshell;
    int         history_sz;
    int         height, width;
    int         offset;

    VAR_UNUSED(flags);

    vshell = (vshell_t *)vterm_get_userptr(vpane->vterm);

    history_sz = vterm_get_history_size(vpane->vterm);
    vterm_wnd_size(vpane->vterm, &width, &height);

    if((vpane->cursor_pos + vpane->scroll_amount) < 0)
    {
        vpane->cursor_pos = 0;
        vpane->scroll_amount = 0;
    }

    if((vpane->cursor_pos + vpane->scroll_amount) > history_sz - height)
    {
        vpane->cursor_pos = history_sz - height;
        vpane->scroll_amount = 0;
    }

    vpane->cursor_pos += vpane->scroll_amount;
    offset = history_sz - height - vpane->cursor_pos;

    vterm_wnd_update(vpane->vterm, VTERM_BUF_HISTORY, offset);
    vpane->state |= TERM_STATE_DIRTY;

    vshell_update_canvas(vshell, 0);

    return;
}



void
vshell_parse_cmdline(vshell_t *vshell)
{
    int     count = 1;
    int     i;

    if(vshell == NULL) return;

    if (vshell->argc > 1)
    {
        // interate through argv[] looking for params
        for (i = 1; i < vshell->argc; i++)
        {
            if (strncmp(vshell->argv[i], "--help", strlen("--help")) == 0)
            {
                endwin();
                vshell_print_help();
                exit(0);
            }

            if (strncmp(vshell->argv[i], "--version", strlen("--version")) == 0)
            {
                endwin();
                vshell_print_help();
                exit(0);
            }

            if (strncmp(vshell->argv[i], "--dump", strlen("--dump")) == 0)
            {
                vshell->vterm_flags |= VTERM_FLAG_DUMP;
                continue;
            }

            if (strncmp(vshell->argv[i], "--rxvt", strlen("--rxvt")) == 0)
            {
                vshell->vterm_flags |= VTERM_FLAG_RXVT;
                continue;
            }

            if (strncmp(vshell->argv[i], "--vt100", strlen("--vt100")) == 0)
            {
                vshell->vterm_flags |= VTERM_FLAG_VT100;
                continue;
            }

            if (strncmp(vshell->argv[i], "--linux", strlen("--linux")) == 0)
            {
                vshell->vterm_flags |= VTERM_FLAG_LINUX;
                continue;
            }

            if (strncmp(vshell->argv[i], "--c16", strlen("--c16")) == 0)
            {
                vshell->vterm_flags |= VTERM_FLAG_C16;
                continue;
            }

            if (strncmp(vshell->argv[i], "--exec", strlen("--exec")) == 0)
            {
                // must have at least exec path
                i++;
                if (i < vshell->argc)
                {
                    vshell->exec_path = strdup(vshell->argv[i]);

                    // first arg should be same as path
                    vshell->exec_argv = (char **)calloc(vshell->argc,
                        sizeof(char *));

                    vshell->exec_argv[0] = strdup(vshell->argv[i]);
                    i++;
                }

                count = 1;
                while(i < vshell->argc)
                {
                    vshell->exec_argv[count] = strdup(vshell->argv[i]);

                    count++;
                    i++;
                }

                // this will always be the last set of params we handle
                break;
            }
        }
    }

    return;
}

void
vshell_help_init(vshell_t *vshell)
{
    wchar_t         *help[] =
                        {   L"Alt +     Create new pane",
                            L"Alt -     Close pane",
                            L"Alt w     Switch pane",
                            L"",
                            L"",
                            L"",
                            L"Alt z     Toggle history mode",
                            L"+ / -     Grow / shrink buffer",
                            L"\u2191 / \u2193     Scroll up / down one line",
                            L"PgUp      Scroll up one page",
                            L"PgDn      Scroll down one page",
                            L"",
                            L"",
                            L"",
                            L"Alt H     Show / hide help"   };


    wchar_t         wbuf[WBUF_MAX];
    short           colors;
    int             len;
    unsigned int    i;


    vshell->help = newwin(HELP_HEIGHT, HELP_WIDTH, 0, 0);
    scrollok(vshell->help, FALSE);

    colors = vshell_pair_selector(NULL, COLOR_YELLOW, COLOR_BLACK);
    wattrset(vshell->help, COLOR_PAIR(colors) | WA_BOLD);
    wborder_set(vshell->help, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    len = wcslen(L"[ Help ]");
    mvwadd_wchars(vshell->help, 0, (HELP_WIDTH - len) >> 1, L"[ Help ]");

    for(i = 0; i < ARRAY_SZ(help); i++)
    {
        mvwadd_wchars(vshell->help, i + 4, 2, help[i]);
    }

    colors = vshell_pair_selector(NULL, COLOR_WHITE, COLOR_BLACK);
    wattrset(vshell->help, COLOR_PAIR(colors) | WA_BOLD);

    len = wcslen(L"Pane Controls");
    mvwadd_wchars(vshell->help, 2, (HELP_WIDTH - len) >> 1, L"Pane Controls");

    len = swprintf(wbuf, WBUF_MAX, L"History Controls");
    mvwadd_wchars(vshell->help, 8, (HELP_WIDTH - len) >> 1, wbuf);

    len = swprintf(wbuf, WBUF_MAX, L"General Controls");
    mvwadd_wchars(vshell->help, 16, (HELP_WIDTH - len) >> 1, wbuf);

    return;
}

void
vshell_print_help(void)
{
    char    *help_msg =
                "Usage:  vshell  <args...>\n\r"
                " \n\r"
                "[Args]         [Description]\n\r"
                "---------------------------------------------------------\n\r"
                "--help         Show usage information.\n\r"
                "--version      Show version information.\n\r"
                "--dump         Write escape sequences to a log file.\n\r"
                "--vt100        Set emulation mode to vt100.\n\r"
                "--rxvt         Set emulation mode to rxvt.\n\r"
                "--linux        Set emulation mode to linux.\n\r"
                "--xterm        Set emulation mode to xterm (default).\n\r"
                "               If the underlying terminal supports it,\n\r"
                "               vshell will attempt to use hi-color mode.\n\r"
                "--c16          When running in 16 color mode use the\n\r"
                "               libvterm palette instead.\n\r"
                "--exec <prg>   Launch program at start up.\n\r";

    printf("\n\rLibvterm version: %s\n\r\n\r%s\n\r",
        LIBVTERM_VERSION, help_msg);

    return;
}

void
mvwadd_wchars(WINDOW *win, int row_y, int col_x, wchar_t *wchstr)
{
    cchar_t         cch;
    wchar_t         wch[CCHARW_MAX]; 
    attr_t          attrs;
    short           cell_colors;
    size_t          len;
    unsigned int    i;

    len = wcslen(wchstr);

    for(i = 0; i < len; i++)
    {
        mvwin_wch(win, row_y, col_x, &cch);

        getcchar(&cch, wch, &attrs, &cell_colors, NULL);

        swprintf(wch, CCHARW_MAX - 1, L"%lc", wchstr[i]);

        setcchar(&cch, wch, attrs, cell_colors, NULL);

        wmove(win, row_y, col_x);
        wadd_wch(win, &cch);

        col_x++;
    }

    return;
}

void
wchar_box(WINDOW *win, int colors, int x, int y, int cols, int rows)
{
    setcchar(&cc_syms[WCS_VLINE_NORMAL], &wc_syms[WCS_VLINE_NORMAL],
        WA_NORMAL, colors, NULL);

    setcchar(&cc_syms[WCS_HLINE_NORMAL], &wc_syms[WCS_HLINE_NORMAL],
        WA_NORMAL, colors, NULL);

    setcchar(&cc_syms[WCS_TLCORNER_NORMAL], &wc_syms[WCS_TLCORNER_NORMAL],
        WA_NORMAL, colors, NULL);

    setcchar(&cc_syms[WCS_TRCORNER_NORMAL], &wc_syms[WCS_TRCORNER_NORMAL],
        WA_NORMAL, colors, NULL);

    setcchar(&cc_syms[WCS_BLCORNER_NORMAL], &wc_syms[WCS_BLCORNER_NORMAL],
        WA_NORMAL, colors, NULL);

    setcchar(&cc_syms[WCS_BRCORNER_NORMAL], &wc_syms[WCS_BRCORNER_NORMAL],
        WA_NORMAL, colors, NULL);

    mvwhline_set(win, y, x, &cc_syms[WCS_HLINE_NORMAL], cols);

    mvwhline_set(win, y + rows - 1, x, &cc_syms[WCS_HLINE_NORMAL], cols);

    mvwvline_set(win, y, x, &cc_syms[WCS_VLINE_NORMAL], rows);

    mvwvline_set(win, y, x + cols -1 , &cc_syms[WCS_VLINE_NORMAL], rows);

    mvwadd_wch(win, y, x, &cc_syms[WCS_TLCORNER_NORMAL]);

    mvwadd_wch(win, y, x + (cols - 1), &cc_syms[WCS_TRCORNER_NORMAL]);

    mvwadd_wch(win, y + (rows - 1), x, &cc_syms[WCS_BLCORNER_NORMAL]);

    mvwadd_wch(win, y + (rows - 1), x + (cols - 1),
        &cc_syms[WCS_BRCORNER_NORMAL]);

    return;
}

