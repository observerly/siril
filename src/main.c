/*
 * This file is part of Siril, an astronomy image processor.
 * Copyright (C) 2005-2011 Francois Meyer (dulle at free.fr)
 * Copyright (C) 2012-2019 team free-astro (see more in AUTHORS file)
 * Reference site is https://free-astro.org/index.php/Siril
 *
 * Siril is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Siril is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Siril. If not, see <http://www.gnu.org/licenses/>.
*/

#define MAIN
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gtk/gtk.h>
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <tchar.h>
#include <io.h>
#include <fcntl.h>
#endif
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <locale.h>
#if (defined(__APPLE__) && defined(__MACH__))
#include <stdlib.h>
#include <libproc.h>
#endif
#include <getopt.h>

#include "core/siril.h"
#include "core/proto.h"
#include "core/initfile.h"
#include "core/command.h"
#include "core/pipe.h"
#include "io/sequence.h"
#include "io/conversion.h"
#include "gui/callbacks.h"
#include "gui/script_menu.h"
#include "gui/progress_and_log.h"
#include "registration/registration.h"
#include "stacking/stacking.h"
#include "core/undo.h"
#include "io/single_image.h"
#include "algos/star_finder.h"
#include "algos/photometry.h"

#define GLADE_FILE "siril3.glade"

/* the global variables of the whole project */
cominfo com;	// the main data struct
fits gfit;	// currently loaded image
GtkBuilder *builder;	// get widget references anywhere

#ifdef MAC_INTEGRATION

static gboolean osx_open_file(GtkosxApplication *osx_app, gchar *path, gpointer data){
	if (path != NULL) {
		open_single_image(path);
		return FALSE;
	}
	return TRUE;
}

static void gui_add_osx_to_app_menu(GtkosxApplication *osx_app, const gchar *item_name, gint index) {
	GtkWidget *item;

	item = lookup_widget(item_name);
	if (GTK_IS_MENU_ITEM(item))
		gtkosx_application_insert_app_menu_item(osx_app, GTK_WIDGET(item), index);
}

static void set_osx_integration(GtkosxApplication *osx_app, gchar *siril_path) {
	GtkWidget *menubar = lookup_widget("menubar1");
	GtkWidget *file_quit_menu_item = lookup_widget("exit");
	GtkWidget *help_menu = lookup_widget("help1");
	GtkWidget *window_menu = lookup_widget("menuitemWindows");
	GtkWidget *sep;
	GdkPixbuf *icon;
	gchar *icon_path;
	
	g_signal_connect(osx_app, "NSApplicationOpenFile", G_CALLBACK(osx_open_file), NULL);

	gtk_widget_hide(menubar);

	gtkosx_application_set_menu_bar(osx_app, GTK_MENU_SHELL(menubar));
	gtkosx_application_set_window_menu(osx_app, GTK_MENU_ITEM(window_menu));

	gui_add_osx_to_app_menu(osx_app, "help_item1", 0);
	gui_add_osx_to_app_menu(osx_app, "help_get_scripts", 1);
	gui_add_osx_to_app_menu(osx_app, "help_update", 2);
	sep = gtk_separator_menu_item_new();
	gtkosx_application_insert_app_menu_item(osx_app, sep, 2);
	gui_add_osx_to_app_menu(osx_app, "settings", 3);
	sep = gtk_separator_menu_item_new();
	gtkosx_application_insert_app_menu_item(osx_app, sep, 4);

	gtk_widget_hide(file_quit_menu_item);
	gtk_widget_hide(help_menu);
	
	icon_path = g_build_filename(siril_path, "pixmaps/siril.svg", NULL);
	icon = gdk_pixbuf_new_from_file(icon_path, NULL);
	gtkosx_application_set_dock_icon_pixbuf(osx_app, icon);
		
	gtkosx_application_ready(osx_app);
	g_free(icon_path);
}

#endif
#ifdef _WIN32
/* origine du source: https://stackoverflow.com/questions/24171017/win32-console-application-that-can-open-windows */
int ReconnectIO(int OpenNewConsole)
{
    int    hConHandle;
    HANDLE lStdHandle;
    FILE  *fp;
    int    MadeConsole;

    MadeConsole=0;
    if(!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        if(!OpenNewConsole)
            return 0;

        MadeConsole=1;
        if(!AllocConsole())
            return 0;  
    }

    // STDOUT to the console
    lStdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    hConHandle = _open_osfhandle((intptr_t)lStdHandle, _O_TEXT);
    fp = _fdopen( hConHandle, "w" );
    *stdout = *fp;
    setvbuf( stdout, NULL, _IONBF, 0 );

     // STDIN to the console
    lStdHandle = GetStdHandle(STD_INPUT_HANDLE);
    hConHandle = _open_osfhandle((intptr_t)lStdHandle, _O_TEXT);
    fp = _fdopen( hConHandle, "r" );
    *stdin = *fp;
    setvbuf( stdin, NULL, _IONBF, 0 );

    // STDERR to the console
    lStdHandle = GetStdHandle(STD_ERROR_HANDLE);
    hConHandle = _open_osfhandle((intptr_t)lStdHandle, _O_TEXT);
    fp = _fdopen( hConHandle, "w" );
    *stderr = *fp;
    setvbuf( stderr, NULL, _IONBF, 0 );

    return MadeConsole;
}	
#endif

char *siril_sources[] = {
#ifdef _WIN32
    "../share/siril",
#elif (defined(__APPLE__) && defined(__MACH__))
	"/tmp/siril/Contents/Resources/share/siril/",
#endif
	PACKAGE_DATA_DIR"/",
	"/usr/share/siril/",
	"/usr/local/share/siril/",
	""
};

void usage(const char *command) {
    printf("\nUsage:  %s [OPTIONS] [IMAGE_FILE_TO_OPEN]\n\n", command);
    puts("    -d, --directory CWD        changing the current working directory as the argument");
    puts("    -s, --script    SCRIPTFILE run the siril commands script in console mode");
    puts("    -i              INITFILE   load configuration from file name instead of the default configuration file");
    puts("    -p                         run in console mode with command and log stream through named pipes");
    puts("    -f, --format               print all supported image file formats (depending on installed libraries)");
    puts("    -v, --version              print program name and version and exit");
    puts("    -h, --help                 show this message");
}

void signal_handled(int s) {
	// printf("Caught signal %d\n", s);
	gtk_main_quit();
}

static void initialize_scrollbars() {
	int i;
	char *vport_names[] = { "r", "g", "b", "rgb" };
	char window_name[32];

	for (i = 0; i < sizeof(vport_names) / sizeof(char *); i++) {
		sprintf(window_name, "scrolledwindow%s", vport_names[i]);
		GtkScrolledWindow *win = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, window_name));
		com.hadj[i] = gtk_scrolled_window_get_hadjustment(win);
		g_signal_connect(com.hadj[i], "value-changed",
				G_CALLBACK(scrollbars_hadjustment_changed_handler), NULL);
		com.vadj[i] = gtk_scrolled_window_get_vadjustment(win);
		g_signal_connect(com.vadj[i], "value-changed",
				G_CALLBACK(scrollbars_vadjustment_changed_handler), NULL);
	}
}

static void initialize_path_directory() {
	GtkFileChooser *swap_dir;

	swap_dir = GTK_FILE_CHOOSER(lookup_widget("filechooser_swap"));
	if (com.swap_dir && com.swap_dir[0] != '\0')
		gtk_file_chooser_set_filename (swap_dir, com.swap_dir);
	else
		gtk_file_chooser_set_filename (swap_dir, g_get_tmp_dir());
}

struct option long_opts[] = {
		{"version", no_argument, 0, 'v'},
		{"help", no_argument, 0, 'h'},
		{"format", no_argument, 0, 'f'},
		{"directory", required_argument, 0, 'd'},
		{"script",    required_argument, 0, 's'},
		{0, 0, 0, 0}
	};

static GtkTargetEntry drop_types[] = {
  {"text/uri-list", 0, 0}
};


int main(int argc, char *argv[]) {
	int i, c;
	extern char *optarg;
	extern int opterr;
	gchar *siril_path = NULL;
	gchar *current_cwd = NULL;
	gboolean forcecwd = FALSE;
	char *cwd_forced = NULL, *start_script = NULL;

	g_setenv ("LC_NUMERIC", "C", TRUE); // avoid possible bugs using french separator ","

	/* for translation */
#ifdef _WIN32
	setlocale(LC_ALL, "");

	gchar *localedir = g_build_filename(_getcwd(0, 0), "\\..\\share\\locale", NULL);
	gchar *localefilename = g_win32_locale_filename_from_utf8(localedir);
	bindtextdomain(PACKAGE, localefilename);
	bind_textdomain_codeset(PACKAGE, "UTF-8");
	g_free(localefilename);
	g_free(localedir);
#else
	bindtextdomain(PACKAGE, LOCALEDIR);
#endif
	textdomain(PACKAGE);

	opterr = 0;
	memset(&com, 0, sizeof(struct cominf));	// needed?
	com.initfile = NULL;

	/* Caught signals */
	signal(SIGINT, signal_handled);

	while ((c = getopt_long(argc, argv, "i:phfvd:s:", long_opts, NULL)) != -1) {
		switch (c) {
			case 'i':
				com.initfile = g_strdup(optarg);
				break;
			case 'v':
				fprintf(stdout, "%s %s\n", PACKAGE, VERSION);
				exit(EXIT_SUCCESS);
				break;
			case 'f':
				list_format_available();
				exit(EXIT_SUCCESS);
				break;
			case 'd':
				cwd_forced = optarg;
				forcecwd = TRUE;
				break;
			case 's':
			case 'p':
				com.script = TRUE;
				com.headless = TRUE;
				/* need to force cwd to the current dir if no option -d */
				if (!forcecwd) {
					cwd_forced = g_get_current_dir();
					forcecwd = TRUE;
				}
				if (c == 's')
					start_script = optarg;
				break;
			default:
				fprintf(stderr, _("unknown command line parameter '%c'\n"), argv[argc - 1][1]);
				/* no break */
			case 'h':
				usage(argv[0]);
				exit(EXIT_SUCCESS);
		}
	}
	com.cvport = RED_VPORT;
	com.show_excluded = TRUE;
	com.selected_star = -1;
	com.star_is_seqdata = FALSE;
	com.stars = NULL;
	com.uniq = NULL;
	com.color = NORMAL_COLOR;
	for (i = 0; i < MAXVPORT; i++)
		com.buf_is_dirty[i] = TRUE;
	memset(&com.selection, 0, sizeof(rectangle));
	memset(com.layers_hist, 0, sizeof(com.layers_hist));
	/* initialize the com struct and zoom level */
	com.sliders = MINMAX;
	com.zoom_value = ZOOM_DEFAULT;

	/* set default CWD */
	com.wd = siril_get_startup_dir();
	current_cwd = g_get_current_dir();

	/* load init file */
	if (checkinitfile()) {
		siril_log_message(_("Could not load or create settings file, exiting.\n"));
		exit(1);
	}

	if (!com.headless) {
		gtk_init(&argc, &argv);

		/* load prefered theme */
		load_prefered_theme(com.combo_theme);

		/* try to load the glade file, from the sources defined above */
		builder = gtk_builder_new();

		i = 0;
		do {
			GError *err = NULL;
			gchar *gladefile;

			gladefile = g_build_filename (siril_sources[i], GLADE_FILE, NULL);
			if (gtk_builder_add_from_file (builder, gladefile, &err)) {
				fprintf(stdout, _("Successfully loaded '%s'\n"), gladefile);
				g_free(gladefile);
				break;
			}
			fprintf (stderr, _("%s. Looking into another directory...\n"), err->message);
			g_error_free(err);
			g_free(gladefile);
			i++;
		} while (i < G_N_ELEMENTS(siril_sources));
		if (i == G_N_ELEMENTS(siril_sources)) {
			fprintf(stderr, _("%s was not found or contains errors, cannot render GUI. Exiting.\n"), GLADE_FILE);
			exit(EXIT_FAILURE);
		}
		siril_path = siril_sources[i];

		gtk_builder_connect_signals (builder, NULL);
	}

	siril_log_color_message(_("Welcome to %s v%s\n"), "bold", PACKAGE, VERSION);

	/* initialize converters (utilities used for different image types importing) */
	initialize_converters();

	/* initialize photometric variables */
	initialize_photometric_param();

	/* initialize sequence-related stuff */
	initialize_sequence(&com.seq, TRUE);

	/* initializing internal structures with widgets (drawing areas) */
	if (!com.headless) {
		com.vport[RED_VPORT] = lookup_widget("drawingarear");
		com.vport[GREEN_VPORT] = lookup_widget("drawingareag");
		com.vport[BLUE_VPORT] = lookup_widget("drawingareab");
		com.vport[RGB_VPORT] = lookup_widget("drawingareargb");
		com.preview_area[0] = lookup_widget("drawingarea_preview1");
		com.preview_area[1] = lookup_widget("drawingarea_preview2");
		initialize_remap();
		initialize_scrollbars();
		init_mouse();

		/* Keybord Shortcuts */
		initialize_shortcuts();

		/* Select combo boxes that trigger some text display or other things */
		gtk_combo_box_set_active(GTK_COMBO_BOX(gtk_builder_get_object(builder, "comboboxstack_methods")), 0);
		gtk_combo_box_set_active(GTK_COMBO_BOX(gtk_builder_get_object(builder, "comboboxstacksel")), 0);
		zoomcombo_update_display_for_zoom();

		adjust_sellabel();

		/* load the css sheet for general style */
		load_css_style_sheet(siril_path);

		/* initialize theme */
		initialize_theme_GUI();

		/* initialize menu gui */
		update_MenuItem();
		initialize_script_menu();

		/* initialize command completion */
		init_completion_command();

		/* initialize preprocessing */
		initialize_preprocessing();

		/* initialize registration methods */
		initialize_registration_methods();

		/* initialize stacking methods */
		initialize_stacking_methods();

		/* register some callbacks */
		register_selection_update_callback(update_export_crop_label);

		/* initialization of the binning parameters */
		GtkComboBox *binning = GTK_COMBO_BOX(gtk_builder_get_object(builder, "combobinning"));
		gtk_combo_box_set_active(binning, 0);

		/* initialization of some paths */
		initialize_path_directory();

		/* initialization of default FITS extension */
		GtkComboBox *box = GTK_COMBO_BOX(lookup_widget("combobox_ext"));
		gtk_combo_box_set_active_id(box, com.ext);
		initialize_FITS_name_entries();

		initialize_log_tags();

		/* support for converting files by dragging onto the GtkTreeView */
		gtk_drag_dest_set(lookup_widget("treeview_convert"),
				GTK_DEST_DEFAULT_MOTION, drop_types, G_N_ELEMENTS(drop_types),
				GDK_ACTION_COPY);

		set_GUI_CWD();

#ifdef HAVE_LIBRAW
		set_GUI_LIBRAW();
#endif
		set_GUI_photometry();

		init_peaker_GUI();

		g_object_ref(G_OBJECT(lookup_widget("main_window"))); // don't destroy it on removal
		g_object_ref(G_OBJECT(lookup_widget("rgb_window")));  // don't destroy it on removal

		update_used_memory();
	}
	else {
		init_peaker_default();
	}

	/* Get CPU number and set the number of threads */
	siril_log_message(_("Parallel processing %s: Using %d logical processor(s).\n"),
#ifdef _OPENMP
			_("enabled"), com.max_thread = omp_get_num_procs()
#else
			_("disabled"), com.max_thread = 1
#endif
			);
	if (!com.headless) {
		update_spinCPU(com.max_thread);
	}

	/* handling OS-X integration */
#ifdef MAC_INTEGRATION
	GtkosxApplication *osx_app = gtkosx_application_get();
	if (!com.headless) {
		set_osx_integration(osx_app, siril_path);
	}
#endif //MAC_INTEGRATION

	/* start Siril */
	if (argv[optind] != NULL) {
		if (current_cwd) {
			changedir(current_cwd, NULL);
			g_free(current_cwd);
		}
		open_single_image(argv[optind]);
		if (!forcecwd) {
			gchar *newpath = g_path_get_dirname(argv[optind]);
			changedir(newpath, NULL);
			g_free(newpath);
		}
	}

	if (forcecwd && cwd_forced) {
		changedir(cwd_forced, NULL);
	}

	if (!com.script) {
		set_GUI_CWD();
	}

	if (com.headless) {
		if (start_script) {
			FILE* fp = g_fopen(start_script, "r");
			if (fp == NULL) {
				siril_log_message(_("File [%s] does not exist\n"), start_script);
				exit(1);
			}
#ifdef _WIN32			
			ReconnectIO(1);
#endif
			execute_script(fp);
		}
		else {
			pipe_start();
			read_pipe(NULL);
		}
	}
	else gtk_main();

	/* quit Siril */
	close_sequence(FALSE);	// closing a sequence if loaded
	close_single_image();	// close the previous image and free resources
	pipe_stop();		// close the pipes and their threads
#ifdef MAC_INTEGRATION
	g_object_unref(osx_app);
#endif //MAC_INTEGRATION
	return 0;
}
