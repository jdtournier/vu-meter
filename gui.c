#define  _POSIX_C_SOURCE  200809L
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <gtk/gtk.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include "vu.h"

#ifndef  MAX_CHANNELS
#define  MAX_CHANNELS  32
#endif

#ifndef  MAX_RATE
#define  MAX_RATE      250000
#endif

static volatile sig_atomic_t  done = 0;

static void handle_done(int signum)
{
    if (!done)
        done = signum;
}

static int install_done(int signum)
{
    struct sigaction  act;
    memset(&act, 0, sizeof act);
    sigemptyset(&act.sa_mask);
    act.sa_handler = handle_done;
    act.sa_flags = SA_RESTART;
    if (sigaction(signum, &act, NULL) == -1)
        return errno;
    return 0;
}

struct tickmark {
    float       amplitude;
    float       red;
    float       green;
    float       blue;
};

enum placement {
    PLACEMENT_LEFT = 1,
    PLACEMENT_RIGHT = 2,
    PLACEMENT_TOP = 3,
    PLACEMENT_BOTTOM = 4
};

static const char      *server = NULL;
static const char      *device = NULL;
static int              channels = 2;
static int              rate = 48000;
static int              updates = 60;
static int              bar_size = 4;
static int              bar_space = 3;
static int              display_monitor = -1;
static enum placement   display_placement = PLACEMENT_RIGHT;

static float           *peak_line    = NULL;
static float           *peak         = NULL;

static float            decay_line   = 0.99f;
static float            decay        = 0.95f;
static float            red_limit    = 0.891251f;   /* Amplitude within 1 dB of clipping */
static float            green_limit  = 0.707946f;   /* Amplitude within 3 dB of clipping */

static struct tickmark  tickmarks[] = {
    { .amplitude = 0.891251f, .red = 1.0f, .green = 0.00f, .blue = 0.0f },   /*  1 dB */
    { .amplitude = 0.794328f, .red = 1.0f, .green = 1.00f, .blue = 0.0f },   /*  2 dB */
    { .amplitude = 0.707946f, .red = 0.0f, .green = 1.00f, .blue = 0.0f },   /*  3 dB */
    { .amplitude = 0.630957f, .red = 0.0f, .green = 0.95f, .blue = 0.0f },   /*  4 dB */
    { .amplitude = 0.562341f, .red = 0.0f, .green = 0.90f, .blue = 0.0f },   /*  5 dB */
    { .amplitude = 0.501187f, .red = 0.0f, .green = 0.85f, .blue = 0.0f },   /*  6 dB */
    { .amplitude = 0.446684f, .red = 0.0f, .green = 0.80f, .blue = 0.0f },   /*  7 dB */
    { .amplitude = 0.398107f, .red = 0.0f, .green = 0.75f, .blue = 0.0f },   /*  8 dB */
    { .amplitude = 0.354813f, .red = 0.0f, .green = 0.70f, .blue = 0.0f },   /*  9 dB */
    { .amplitude = 0.316228f, .red = 0.0f, .green = 0.65f, .blue = 0.0f },   /* 10 dB */
    { .amplitude = -1.0f }
};

static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    (void)user_data; /* Silence unused parameter warning; generates no code */

    GdkRectangle  area;
    gtk_widget_get_clip(widget, &area);

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.0,0.0,0.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);

    for (int i = 0; i < channels; i++) {
        if (peak[i] >= red_limit)
            cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
        else
        if (peak[i] <= green_limit)
            cairo_set_source_rgb(cr, 0.0, 0.5 + 0.5*peak[i]/green_limit, 0.0);
        else {
            const double c = (peak[i] - green_limit) / (red_limit - green_limit);
            cairo_set_source_rgb(cr, c, 1.0-c, 0.0);
        }

        const double c = (peak[i] < 0.0f) ? 0.0 : (peak[i] < 1.0f) ? peak[i] : 1.0;

        if (display_placement == PLACEMENT_LEFT || display_placement == PLACEMENT_RIGHT) {
            cairo_rectangle(cr, area.x + bar_space + i * (bar_space + bar_size),
                                area.y + bar_space + (1.0 - c)*(area.height - 2*bar_space),
                                bar_size, c*area.height);
            cairo_fill(cr);
        } else
        if (display_placement == PLACEMENT_TOP || display_placement == PLACEMENT_BOTTOM) {
            cairo_rectangle(cr, area.x + bar_space,
                                area.y + bar_space + i * (bar_space + bar_size),
                                c * (area.width - 2*bar_space), bar_size);
            cairo_fill(cr);
        }
    }

    cairo_set_line_width(cr, 1.0);
    if (display_placement == PLACEMENT_LEFT || display_placement == PLACEMENT_RIGHT) {
        for (int i = 0; tickmarks[i].amplitude >= 0.0f; i++) {
            const int  y = area.y + bar_space + (1.0f - tickmarks[i].amplitude)*(area.height - 2*bar_space);
            cairo_set_source_rgb(cr, tickmarks[i].red, tickmarks[i].green, tickmarks[i].blue);
            cairo_move_to(cr, area.x + 2, y);
            cairo_line_to(cr, area.x + area.width - 2, y);
            cairo_stroke(cr);
        }
    } else
    if (display_placement == PLACEMENT_TOP || display_placement == PLACEMENT_BOTTOM) {
        for (int i = 0; tickmarks[i].amplitude >= 0.0f; i++) {
            const int  x = area.x + bar_space + tickmarks[i].amplitude*(area.width - 2*bar_space);
            cairo_set_source_rgb(cr, tickmarks[i].red, tickmarks[i].green, tickmarks[i].blue);
            cairo_move_to(cr, x, area.y + 2);
            cairo_line_to(cr, x, area.y + area.width - 2);
            cairo_stroke(cr);
        }
    }

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 2.0);
    for (int i = 0; i < channels; i++) {
        const double c = (peak_line[i] < 0.0f) ? 0.0 : (peak_line[i] < 1.0f) ? peak_line[i] : 1.0;
        if (display_placement == PLACEMENT_LEFT || display_placement == PLACEMENT_RIGHT) {
            const int  y = area.y + bar_space + (1.0f - c)*(area.height - 2*bar_space);
            cairo_move_to(cr, area.x +  i   *(bar_space + bar_size), y);
            cairo_line_to(cr, area.x + (i+1)*(bar_space + bar_size), y);
            cairo_stroke(cr);
        } else
        if (display_placement == PLACEMENT_TOP || display_placement == PLACEMENT_BOTTOM) {
            const int  x = area.x + bar_space + c*(area.width - 2*bar_space);
            cairo_move_to(cr, x, area.y +  i   *(bar_space + bar_size));
            cairo_line_to(cr, x, area.y + (i+1)*(bar_space + bar_size));
            cairo_stroke(cr);
        }
    }

    cairo_restore(cr);
    return TRUE;
}

static gboolean tick(GtkWidget *widget, GdkFrameClock *fclk, gpointer user_data)
{
    (void)user_data; /* Silence unused parameter warning; generates no code */
    (void)fclk;

    if (done) {
        gtk_window_close(GTK_WINDOW(widget));
        return G_SOURCE_REMOVE;
    }

    float  new_peak[channels];
    if (vu_peak(new_peak, channels) == channels) {
        for (int c = 0; c < channels; c++) {
            peak[c] *= decay;
            peak[c]  = (new_peak[c] > peak[c]) ? new_peak[c] : peak[c];

            peak_line[c] *= decay_line;
            peak_line[c]  = (new_peak[c] > peak_line[c]) ? new_peak[c] : peak_line[c];
        }
        gtk_widget_queue_draw(widget);
    }


    return G_SOURCE_CONTINUE;
}

static void screen_changed(GtkWidget *widget, GdkScreen *old_screen, gpointer user_data)
{
    (void)user_data; (void)old_screen; /* Silence unused parameter warning; generates no code */
    gtk_widget_set_visual(widget, gdk_screen_get_system_visual(gtk_widget_get_screen(widget)));
}

static void place(GdkRectangle *to, gulong *reserve)
{
    if (!to || !reserve)
        return;

    to->x = 0;
    to->y = 0;
    to->width = bar_space + (bar_size + bar_space) * channels;
    to->height = bar_space + (bar_size + bar_space) * channels;

    reserve[ 0] = 0;  /* left */
    reserve[ 1] = 0;  /* right */
    reserve[ 2] = 0;  /* top */
    reserve[ 3] = 0;  /* bottom */
    reserve[ 4] = 0;  /* left_start_y */
    reserve[ 5] = 0;  /* left_end_y */
    reserve[ 6] = 0;  /* right_start_y */
    reserve[ 7] = 0;  /* right_end_y */
    reserve[ 8] = 0;  /* top_start_x */
    reserve[ 9] = 0;  /* top_end_x */
    reserve[10] = 0;  /* bottom_start_x */
    reserve[11] = 0;  /* bottom_end_x */

    GdkDisplay  *d = gdk_display_get_default();
    GdkMonitor  *m = gdk_display_get_monitor(d, display_monitor);
    GdkRectangle w;
    if (!m)
        m = gdk_display_get_primary_monitor(d);
    if (!m)
        return;
    gdk_monitor_get_workarea(m, &w);

    switch (display_placement) {

    case PLACEMENT_LEFT:
        to->x = w.x;
        to->height = w.height;
        reserve[0] = to->width;
        reserve[4] = w.y;
        reserve[5] = w.y + w.height - 1;
        return;

    case PLACEMENT_RIGHT:
        to->x = w.x + w.width - to->width;
        to->height = w.height;
        reserve[1] = to->width;
        reserve[6] = w.y;
        reserve[7] = w.y + w.height - 1;
        return;

    case PLACEMENT_TOP:
        to->y = w.y;
        to->width = w.width;
        reserve[2] = to->height;
        reserve[8] = w.x;
        reserve[9] = w.x + w.width - 1;
        return;

    case PLACEMENT_BOTTOM:
        to->y = w.y + w.height - to->height;
        to->width = w.width;
        reserve[3] = to->height;
        reserve[10] = w.x;
        reserve[11] = w.x + w.width - 1;
        return;
    }
}

static void activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data; /* Silence unused parameter warning; generates no code */

    gulong reserve[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    /* Compute where to place the window */
    GdkRectangle pos;
    place(&pos, reserve);

    /* Create window */
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "VU-Bar");
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_icon_name(GTK_WINDOW(window), "multimedia-volume-control");
    gtk_window_set_default_size(GTK_WINDOW(window), pos.width, pos.height);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    gtk_window_move(GTK_WINDOW(window), pos.x, pos.y);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_widget_add_tick_callback(window, tick, NULL, NULL);
    gtk_widget_set_app_paintable(window, TRUE);
    g_signal_connect(window, "draw", G_CALLBACK(draw), NULL);
    g_signal_connect(window, "screen-changed", G_CALLBACK(screen_changed), NULL);
    gtk_application_add_window(app, GTK_WINDOW(window));
    gtk_widget_show_all(window);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);

    GdkWindow *w = gtk_widget_get_window(window);
    if (w) {
        GdkAtom strut = gdk_atom_intern_static_string("_NET_WM_STRUT");
        GdkAtom partial = gdk_atom_intern_static_string("_NET_WM_STRUT_PARTIAL");
        GdkAtom cardinal = gdk_atom_intern_static_string("CARDINAL");
        gdk_property_change(w, strut, cardinal, 32, GDK_PROP_MODE_REPLACE, (guchar *)reserve, 4);
        gdk_property_change(w, partial, cardinal, 32, GDK_PROP_MODE_REPLACE, (guchar *)reserve, 12);
    }
}


static const char *skip_lws(const char *from)
{
    if (!from)
        return NULL;
    while (isspace((unsigned char)(*from)))
        from++;
    return from;
}

static const char *parse_int(const char *from, int *to)
{
    const char  *next = from;
    long         val;

    if (!from || *from == '\0') {
        errno = EINVAL;
        return NULL;
    }

    errno = 0;
    val = strtol(from, (char **)(&next), 0);
    if (errno)
        return NULL;
    if (next == from) {
        errno = EINVAL;
        return NULL;
    }
    if ((long)(int)(val) != val) {
        errno = ERANGE;
        return NULL;
    }

    if (to)
        *to = val;

    errno = 0;
    return next;
}


int usage(const char *arg0)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: %s -h | --help\n", arg0);
    fprintf(stderr, "       %s [ OPTIONS ]\n", arg0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "       -s SERVER    PulseAudio server\n");
    fprintf(stderr, "       -d DEVICE    Source to monitor\n");
    fprintf(stderr, "       -c CHANNELS  Number of channels\n");
    fprintf(stderr, "       -r RATE      Samples per second\n");
    fprintf(stderr, "       -u COUNT     Peak calculations per second\n");
    fprintf(stderr, "       -m MONITOR   Display monitor number\n");
    fprintf(stderr, "       -p WHERE     Meter placement on display\n");
    fprintf(stderr, "       -B PIXELS    Bar thickness in pixels\n");
    fprintf(stderr, "       -S PIXELS    Bar spacing in pixels\n");
    fprintf(stderr, "Placement:\n");
    fprintf(stderr, "       -p left      Left edge of monitor\n");
    fprintf(stderr, "       -p right     Right edge of monitor\n");
    fprintf(stderr, "       -p top       Top edge of monitor\n");
    fprintf(stderr, "       -p bottom    Bottom edge of monitor\n");
    fprintf(stderr, "\n");
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    const char *arg0 = (argc > 0 && argv && argv[0] && argv[0][0]) ? argv[0] : "(this)";
    const char *p;
    int         opt, val;

    setlocale(LC_ALL, "");

    if (argc > 1 && !strcmp(argv[1], "--help"))
        return usage(arg0);

    gtk_init(&argc, &argv);

    while ((opt = getopt(argc, argv, "hs:d:c:r:u:m:p:B:S:")) != -1) {
        switch (opt) {

        case 'h':
            return usage(arg0);

        case 's':
            if (!optarg || optarg[0] == '\0' || !strcmp(optarg, "default"))
                server = NULL;
            else
                server = optarg;
            break;

        case 'd':
            if (!optarg || optarg[0] == '\0' || !strcmp(optarg, "default"))
                device = NULL;
            else
                device = optarg;
            break;

        case 'c':
            p = skip_lws(parse_int(optarg, &val));
            if (!p || *p != '\0' || val < 1 || val > MAX_CHANNELS) {
                fprintf(stderr, "%s: Invalid number of channels.\n", optarg);
                return EXIT_FAILURE;
            }
            channels = val;
            break;

        case 'r':
            p = skip_lws(parse_int(optarg, &val));
            if (!p || *p != '\0' || val < 128 || val > MAX_RATE) {
                fprintf(stderr, "%s: Invalid sample rate.\n", optarg);
                return EXIT_FAILURE;
            }
            rate = val;
            break;

        case 'u':
            p = skip_lws(parse_int(optarg, &val));
            if (!p || *p != '\0' || val < 1 || val > 200) {
                fprintf(stderr, "%s: Invalid number of peak updates per second.\n", optarg);
                return EXIT_FAILURE;
            }
            updates = val;
            break;

        case 'm':
            p = skip_lws(parse_int(optarg, &val));
            if (!p || *p != '\0' || val < -1) {
                fprintf(stderr, "%s: Invalid monitor number.\n", optarg);
                return EXIT_FAILURE;
            }
            display_monitor = val;
            break;

        case 'p':
            if (!strcasecmp(optarg, "left"))
                display_placement = PLACEMENT_LEFT;
            else
            if (!strcasecmp(optarg, "right"))
                display_placement = PLACEMENT_RIGHT;
            else
            if (!strcasecmp(optarg, "top"))
                display_placement = PLACEMENT_TOP;
            else
            if (!strcasecmp(optarg, "bottom"))
                display_placement = PLACEMENT_BOTTOM;
            else {
                fprintf(stderr, "%s: Unsupported placement.\n", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'B':
            p = skip_lws(parse_int(optarg, &val));
            if (!p || *p != '\0' || val < 1) {
                fprintf(stderr, "%s: Invalid bar thickness in pixels.\n", optarg);
                return EXIT_FAILURE;
            }
            bar_size = val;
            break;

        case 'S':
            p = skip_lws(parse_int(optarg, &val));
            if (!p || *p != '\0' || val < 0) {
                fprintf(stderr, "%s: Invalid bar spacing in pixels.\n", optarg);
                return EXIT_FAILURE;
            }
            bar_space = val;
            break;

        case '?':
            /* getopt() has already printed an error message. */
            return EXIT_FAILURE;

        default:
            /* Bug catcher: This should never occur. */
            fprintf(stderr, "getopt() returned %d ('%c')!\n", opt, opt);
            return EXIT_FAILURE;
        }
    }

    if (install_done(SIGINT) ||
        install_done(SIGHUP) ||
        install_done(SIGTERM) ||
        install_done(SIGQUIT)) {
        fprintf(stderr, "Cannot install signal handlers: %s.\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (optind < argc) {
        fprintf(stderr, "%s: Unsupported parameter.\n", argv[optind]);
        return EXIT_FAILURE;
    }

    GtkApplication *app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    if (!app) {
        fprintf(stderr, "Cannot start GTK+ application.\n");
        return EXIT_FAILURE;
    }
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    size_t  samples = rate / updates;
    if (samples < 1)
        samples = 1;

    val = vu_start(server, "vu-bar", device, "VU monitor", channels, rate, samples);
    if (val) {
        fprintf(stderr, "Cannot monitor audio source: %s.\n", vu_error(val));
        g_object_unref(app);
        return EXIT_FAILURE;
    }

    peak = calloc((size_t)channels * sizeof (float), samples);
    peak_line = calloc((size_t)channels * sizeof (float), samples);
    if (!peak) {
        fprintf(stderr, "Out of memory.\n");
        g_object_unref(app);
        vu_stop();
        return EXIT_FAILURE;
    }

    val = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    vu_stop();
    return val;
}
