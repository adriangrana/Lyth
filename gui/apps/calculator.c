/* ============================================================
 *  calculator.c  —  Lyth Calc
 *
 *  Basic 4-function calculator with keyboard and mouse input.
 *  Uses the Widget Kit for buttons.
 * ============================================================ */

#include "calculator.h"
#include "compositor.h"
#include "window.h"
#include "widgets.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "input.h"

/* ---- Colours (from theme.h) ---- */
#define COL_CALC_BG       THEME_COL_BASE
#define COL_CALC_DISPLAY  THEME_COL_CRUST
#define COL_CALC_TEXT     THEME_COL_TEXT
#define COL_CALC_DIM      THEME_COL_DIM
#define COL_CALC_BTN      THEME_COL_SURFACE0
#define COL_CALC_BTN_HI   THEME_COL_SURFACE1
#define COL_CALC_OP       THEME_COL_ACCENT
#define COL_CALC_EQ       THEME_COL_SUCCESS
#define COL_CALC_CLR      THEME_COL_ERROR
#define COL_CALC_BORDER   THEME_COL_SURFACE1
#define COL_CALC_PANEL    THEME_COL_MANTLE

/* ---- Layout ---- */
#define CALC_WIN_W    240
#define CALC_WIN_H    340
#define CALC_DISP_H   64
#define CALC_BTN_W    50
#define CALC_BTN_H    40
#define CALC_BTN_PAD  6
#define CALC_GRID_X   9   /* content-relative */
#define CALC_GRID_Y   (CALC_DISP_H + 7)  /* content-relative */
#define CALC_MAX_DIG  15

/* ---- Button grid (label, key-for-calc_press) ---- */
static const struct { const char *label; char key; uint32_t bg; } btn_defs[5][4] = {
    { {"C",'C',COL_CALC_CLR}, {"(",'(',COL_CALC_DIM}, {")",')',COL_CALC_DIM}, {"/",'/',COL_CALC_OP} },
    { {"7",'7',COL_CALC_BTN}, {"8",'8',COL_CALC_BTN}, {"9",'9',COL_CALC_BTN}, {"*",'*',COL_CALC_OP} },
    { {"4",'4',COL_CALC_BTN}, {"5",'5',COL_CALC_BTN}, {"6",'6',COL_CALC_BTN}, {"-",'-',COL_CALC_OP} },
    { {"1",'1',COL_CALC_BTN}, {"2",'2',COL_CALC_BTN}, {"3",'3',COL_CALC_BTN}, {"+",'+',COL_CALC_OP} },
    { {"0",'0',COL_CALC_BTN}, {".",'.',COL_CALC_BTN}, {"+/-",'N',COL_CALC_BTN}, {"=",'=',COL_CALC_EQ} },
};

/* ---- State ---- */
static gui_window_t* calc_window;
static int calc_is_open;

static char calc_display[CALC_MAX_DIG + 1];
static int  calc_display_len;

static long calc_accumulator;
static char calc_operator;     /* '+', '-', '*', '/' or '\0' */
static int  calc_new_input;    /* next digit starts fresh */
static int  calc_has_dot;
static int  calc_decimal_places;
static char calc_expr[48];     /* expression display (e.g. "12 + 5") */

/* ---- Helpers ---- */

static void calc_reset(void) {
    calc_display[0] = '0';
    calc_display[1] = '\0';
    calc_display_len = 1;
    calc_accumulator = 0;
    calc_operator = '\0';
    calc_new_input = 1;
    calc_has_dot = 0;
    calc_decimal_places = 0;
    calc_expr[0] = '\0';
}

/* Parse display to integer (scaled by 1000 for 3 decimal places) */
static long calc_parse_display(void) {
    long result = 0;
    int neg = 0;
    int i = 0;
    int dec = 0, in_dec = 0;

    if (calc_display[0] == '-') { neg = 1; i = 1; }

    while (i < calc_display_len) {
        if (calc_display[i] == '.') {
            in_dec = 1;
            i++;
            continue;
        }
        if (calc_display[i] >= '0' && calc_display[i] <= '9') {
            result = result * 10 + (calc_display[i] - '0');
            if (in_dec) dec++;
        }
        i++;
    }
    /* Scale to 3 decimal places */
    while (dec < 3) { result *= 10; dec++; }
    while (dec > 3) { result /= 10; dec--; }

    return neg ? -result : result;
}

/* Format a scaled-by-1000 integer into display string */
static void calc_format_result(long val) {
    int neg = 0;
    unsigned long uval;
    char tmp[20];
    int ti = 0;
    int i;

    if (val < 0) { neg = 1; val = -val; }
    uval = (unsigned long)val;

    /* Check if there's a fractional part */
    if (uval % 1000 == 0) {
        /* Integer result */
        uval /= 1000;
        if (uval == 0) {
            tmp[ti++] = '0';
        } else {
            while (uval > 0) {
                tmp[ti++] = '0' + (char)(uval % 10);
                uval /= 10;
            }
        }
        calc_display_len = 0;
        if (neg) calc_display[calc_display_len++] = '-';
        for (i = ti - 1; i >= 0; i--)
            calc_display[calc_display_len++] = tmp[i];
        calc_display[calc_display_len] = '\0';
        calc_has_dot = 0;
    } else {
        /* Has decimal part */
        unsigned long integer_part = uval / 1000;
        unsigned long frac_part = uval % 1000;

        /* Strip trailing zeros from fraction */
        while (frac_part > 0 && frac_part % 10 == 0) frac_part /= 10;

        calc_display_len = 0;
        if (neg) calc_display[calc_display_len++] = '-';

        /* Integer part */
        if (integer_part == 0) {
            calc_display[calc_display_len++] = '0';
        } else {
            ti = 0;
            while (integer_part > 0) {
                tmp[ti++] = '0' + (char)(integer_part % 10);
                integer_part /= 10;
            }
            for (i = ti - 1; i >= 0; i--)
                calc_display[calc_display_len++] = tmp[i];
        }

        calc_display[calc_display_len++] = '.';

        /* Fractional part */
        ti = 0;
        if (frac_part == 0) {
            calc_display[calc_display_len++] = '0';
        } else {
            while (frac_part > 0) {
                tmp[ti++] = '0' + (char)(frac_part % 10);
                frac_part /= 10;
            }
            for (i = ti - 1; i >= 0; i--)
                calc_display[calc_display_len++] = tmp[i];
        }
        calc_display[calc_display_len] = '\0';
        calc_has_dot = 1;
    }
}

static void calc_evaluate(void) {
    long b = calc_parse_display();
    long result = calc_accumulator;

    switch (calc_operator) {
    case '+': result = calc_accumulator + b; break;
    case '-': result = calc_accumulator - b; break;
    case '*': result = (calc_accumulator * b) / 1000; break;
    case '/':
        if (b == 0) {
            str_copy(calc_display, "Error", CALC_MAX_DIG);
            calc_display_len = 5;
            calc_operator = '\0';
            calc_new_input = 1;
            return;
        }
        result = (calc_accumulator * 1000) / b;
        break;
    default:
        result = b;
        break;
    }

    calc_format_result(result);
    calc_accumulator = result;
    calc_operator = '\0';
    calc_new_input = 1;
}

static void calc_press(char key) {
    if (key >= '0' && key <= '9') {
        if (calc_new_input) {
            calc_display[0] = key;
            calc_display[1] = '\0';
            calc_display_len = 1;
            calc_new_input = 0;
            calc_has_dot = 0;
            calc_decimal_places = 0;
        } else {
            if (calc_display_len < CALC_MAX_DIG) {
                calc_display[calc_display_len++] = key;
                calc_display[calc_display_len] = '\0';
                if (calc_has_dot) calc_decimal_places++;
            }
        }
    } else if (key == '.') {
        if (calc_new_input) {
            calc_display[0] = '0';
            calc_display[1] = '.';
            calc_display[2] = '\0';
            calc_display_len = 2;
            calc_new_input = 0;
            calc_has_dot = 1;
            calc_decimal_places = 0;
        } else if (!calc_has_dot) {
            calc_display[calc_display_len++] = '.';
            calc_display[calc_display_len] = '\0';
            calc_has_dot = 1;
            calc_decimal_places = 0;
        }
    } else if (key == '+' || key == '-' || key == '*' || key == '/') {
        if (calc_operator && !calc_new_input) {
            calc_evaluate();
        } else {
            calc_accumulator = calc_parse_display();
        }
        /* Build expression string */
        str_copy(calc_expr, calc_display, sizeof(calc_expr));
        {
            char op[4] = {' ', key, ' ', '\0'};
            str_append(calc_expr, op, sizeof(calc_expr));
        }
        calc_operator = key;
        calc_new_input = 1;
    } else if (key == '=' || key == '\n') {
        if (calc_operator) {
            /* Append to expression */
            str_append(calc_expr, calc_display, sizeof(calc_expr));
            str_append(calc_expr, " =", sizeof(calc_expr));
            calc_evaluate();
        }
    } else if (key == 'C' || key == 'c') {
        calc_reset();
    } else if (key == 'N') {
        /* Toggle sign */
        if (calc_display[0] == '-') {
            int i;
            for (i = 0; i < calc_display_len; i++)
                calc_display[i] = calc_display[i + 1];
            calc_display_len--;
        } else if (!(calc_display[0] == '0' && calc_display_len == 1)) {
            int i;
            for (i = calc_display_len; i >= 0; i--)
                calc_display[i + 1] = calc_display[i];
            calc_display[0] = '-';
            calc_display_len++;
        }
    }
}

/* ---- Drawing (display only — buttons are widgets) ---- */

static void calc_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int cw = gui_window_content_w(win);
    int ox = GUI_BORDER_WIDTH;
    int oy = GUI_TITLEBAR_HEIGHT + GUI_BORDER_WIDTH;

    if (!s->pixels) return;

    gui_surface_clear(s, COL_CALC_BG);
    gui_window_draw_decorations(win);

    /* Display panel */
    {
        int dx = ox + 9, dy = oy + 3;
        int dw = cw - 18;
        gui_surface_fill(s, dx, dy, dw, CALC_DISP_H, COL_CALC_DISPLAY);

        /* Expression (small, top) */
        if (calc_expr[0]) {
            gui_surface_draw_string(s, dx + 8, dy + 4, calc_expr, COL_CALC_DIM, 0, 0);
        }

        /* Current number (right-aligned) */
        {
            int tw = calc_display_len * GUI_FONT_W;
            gui_surface_draw_string(s, dx + dw - tw - 8,
                                    dy + CALC_DISP_H - GUI_FONT_H - 8,
                                    calc_display, COL_CALC_TEXT, 0, 0);
        }
    }
    /* Buttons drawn automatically by wid_draw_all */
}

/* ---- Key handling ---- */

static void calc_on_key(gui_window_t* win, int event_type, char key) {
    (void)win;

    if (event_type == INPUT_EVENT_CHAR) {
        calc_press(key);
    } else if (event_type == INPUT_EVENT_ENTER) {
        calc_press('=');
    } else if (event_type == INPUT_EVENT_BACKSPACE) {
        /* Delete last digit */
        if (calc_display_len > 1) {
            calc_display_len--;
            if (calc_display[calc_display_len] == '.') calc_has_dot = 0;
            calc_display[calc_display_len] = '\0';
        } else {
            calc_display[0] = '0';
            calc_display[1] = '\0';
        }
    } else if (event_type == INPUT_EVENT_DELETE) {
        calc_reset();
    }

    gui_window_invalidate(calc_window);
}

/* ---- Widget button callback ---- */

static void calc_btn_click(wid_t *w) {
    calc_press((char)w->id);
    gui_window_invalidate(calc_window);
}

/* ---- Mouse click (display area only — buttons handled by widgets) ---- */

static void calc_on_click(gui_window_t* win, int mx, int my, int button) {
    (void)win; (void)mx; (void)my; (void)button;
}

/* ---- Close ---- */

static void calc_on_close(gui_window_t* win) {
    calc_is_open = 0;
    calc_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

/* ---- Public API ---- */

void calculator_app_open(void) {
    if (calc_is_open && calc_window) {
        gui_window_focus(calc_window);
        gui_dirty_add(calc_window->x, calc_window->y,
                      calc_window->width, calc_window->height);
        return;
    }

    calc_window = gui_window_create("Calc", 350, 80, CALC_WIN_W, CALC_WIN_H,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!calc_window) return;

    calc_window->on_paint = calc_paint;
    calc_window->on_key = calc_on_key;
    calc_window->on_click = calc_on_click;
    calc_window->on_close = calc_on_close;

    /* Create button widgets (5 rows × 4 cols) */
    {
        int r, c;
        for (r = 0; r < 5; r++) {
            for (c = 0; c < 4; c++) {
                int bx = CALC_GRID_X + c * (CALC_BTN_W + CALC_BTN_PAD);
                int by = CALC_GRID_Y + r * (CALC_BTN_H + CALC_BTN_PAD);
                wid_t *b = wid_button(calc_window, bx, by,
                                      CALC_BTN_W, CALC_BTN_H,
                                      btn_defs[r][c].label, calc_btn_click);
                if (b) {
                    b->id = (int16_t)btn_defs[r][c].key;
                    b->bg = btn_defs[r][c].bg;
                }
            }
        }
    }

    calc_is_open = 1;
    calc_reset();
}
