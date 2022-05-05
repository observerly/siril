/*
 * This file is part of Siril, an astronomy image processor.
 * Copyright (C) 2005-2011 Francois Meyer (dulle at free.fr)
 * Copyright (C) 2012-2022 team free-astro (see more in AUTHORS file)
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

#include <stdio.h>
#include <stdlib.h>
#include <gsl/gsl_statistics.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_version.h>

#include "core/siril.h"
#include "core/proto.h"
#include "core/processing.h"
#include "core/undo.h"
#include "core/OS_utils.h"
#include "io/single_image.h"
#include "io/image_format_fits.h"
#include "io/sequence.h"
#include "algos/statistics.h"
#include "algos/geometry.h"
#include "algos/sorting.h"
#include "gui/utils.h"
#include "gui/callbacks.h"
#include "gui/image_display.h"
#include "gui/image_interactions.h"
#include "gui/progress_and_log.h"
#include "gui/message_dialog.h"
#include "gui/dialogs.h"
#include "registration/registration.h"	// for mouse_status
#include "background_extraction.h"


#define NPARAM_POLY4 15		// Number of parameters used with 4rd order
#define NPARAM_POLY3 10		// Number of parameters used with 3rd order
#define NPARAM_POLY2 6		// Number of parameters used with 2nd order
#define NPARAM_POLY1 3		// Number of parameters used with 1nd order

#define SAMPLE_SIZE 25

struct sample {
	double median[3]; // median of each channel of the sample (if color)
	double mean; // mean of the 3 channel of the sample (if color)
	double min, max;
	size_t size;
	point position;
	gboolean valid;
};

//C contains background function
#define C(i) (gsl_vector_get(c,(i)))

static double poly_4(gsl_vector *c, double x, double y) {
	double value = C(0) + C(1) * x + C(2) * y + (x * x) * C(3) + (x * y) * C(4)
			+ (y * y) * C(5) + (x * x) * x * C(6) + (x * x) * y * C(7)
			+ x * (y * y) * C(8)+ (y * y) * y * C(9) + (x * x) * (x * x) * C(10)
			+ (x * x) * (x * y) * C(11) + (x * x) * (y * y) * C(12)
			+ (x * y) * (y * y) * C(13) + (y * y) * (y * y) * C(14);

	return (value);
}

static double poly_3(gsl_vector *c, double x, double y) {
	double value = C(0) + C(1) * x + C(2) * y + (x * x) * C(3) + (y * x) * C(4)
			+ (y * y) * C(5) + (x * x) * x * C(6) + (x * x) * y * C(7) + x * (y * y) * C(8)
			+ (y * y) * y * C(9);

	return (value);
}

static double poly_2(gsl_vector *c, double x, double y) {
	double value = C(0) + C(1) * x + C(2) * y + C(3) * (x * x) + C(4) * (y * x)
			+ C(5) * (y * y);

	return (value);
}

static double poly_1(gsl_vector *c, double x, double y) {
	double value = C(0) + C(1) * x + C(2) * y;

	return (value);
}

static gboolean computeBackground(GSList *list, double *background, int channel, unsigned int width, unsigned int height, poly_order order, gchar **err) {
	size_t k = 0;
	double chisq, pixel;
	gsl_matrix *J, *cov;
	gsl_vector *y, *w, *c;
	GSList *l;

	guint n = g_slist_length(list);

	int nbParam;
	switch (order) {
	case BACKGROUND_POLY_1:
		nbParam = NPARAM_POLY1;
		break;
	case BACKGROUND_POLY_2:
		nbParam = NPARAM_POLY2;
		break;
	case BACKGROUND_POLY_3:
		nbParam = NPARAM_POLY3;
		break;
	case BACKGROUND_POLY_4:
	default:
		nbParam = NPARAM_POLY4;
	}

	if (n < nbParam) {
		*err = siril_log_message(_("There are not enough background samples. "
				"The background to be extracted cannot be computed.\n"));
		return FALSE;
	}

	// J is the Jacobian
	// y contains data (pixel intensity)
	J = gsl_matrix_calloc(n, nbParam);
	y = gsl_vector_calloc(n);
	w = gsl_vector_calloc(n);
	c = gsl_vector_calloc(nbParam);
	cov = gsl_matrix_calloc(nbParam, nbParam);

	for (l = list; l; l = l->next) {
		background_sample *sample = (background_sample *) l->data;

		double col = sample->position.x;
		double row = sample->position.y;
		pixel = sample->median[channel];
		// here, it is a bit sketchy in the sense that if there is no
		// value to report in a box (because the threshold is too low
		// for example), then I just skip the initialization of J and y.
		// gsl automatically discards the unassigned values during the
		// minimization. I tested it with Matlab and it works fine. The
		// results agree.
		if (pixel < 0)
			continue;

		gsl_matrix_set(J, k, 0, 1.0);
		gsl_matrix_set(J, k, 1, col);
		gsl_matrix_set(J, k, 2, row);

		if (order != BACKGROUND_POLY_1) {
			gsl_matrix_set(J, k, 3, col * col);
			gsl_matrix_set(J, k, 4, col * row);
			gsl_matrix_set(J, k, 5, row * row);
		}

		if (order == BACKGROUND_POLY_3 || order == BACKGROUND_POLY_4) {
			gsl_matrix_set(J, k, 6, col * col * col);
			gsl_matrix_set(J, k, 7, col * col * row);
			gsl_matrix_set(J, k, 8, col * row * row);
			gsl_matrix_set(J, k, 9, row * row * row);
		}

		if (order == BACKGROUND_POLY_4) {
			gsl_matrix_set(J, k, 10, col * col * col * col);
			gsl_matrix_set(J, k, 11, col * col * col * row);
			gsl_matrix_set(J, k, 12, col * col * row * row);
			gsl_matrix_set(J, k, 13, col * row * row * row);
			gsl_matrix_set(J, k, 14, row * row * row * row);
		}

		gsl_vector_set(y, k, pixel);
		gsl_vector_set(w, k, 1.0);

		k++;
	}

	// Must turn off error handler or it aborts on error
	gsl_set_error_handler_off();

	gsl_multifit_linear_workspace *work = gsl_multifit_linear_alloc(n, nbParam);
	int status = gsl_multifit_wlinear(J, w, y, c, cov, &chisq, work);
	if (status != GSL_SUCCESS) {
		*err = siril_log_message("GSL multifit error: %s\n", gsl_strerror(status));
		gsl_matrix_free(J);
		gsl_vector_free(y);
		gsl_vector_free(w);
		gsl_vector_free(c);
		gsl_matrix_free(cov);
		return FALSE;
	}

	// Calculation of the background with the same dimension that the input matrix.
	for (unsigned int i = 0; i < height; i++) {
		for (unsigned int j = 0; j < width; j++) {
			switch (order) {
			case BACKGROUND_POLY_1:
				pixel = poly_1(c, (double) j, (double) i);
				break;
			case BACKGROUND_POLY_2:
				pixel = poly_2(c, (double) j, (double) i);
				break;
			case BACKGROUND_POLY_3:
				pixel = poly_3(c, (double) j, (double) i);
				break;
			default:
			case BACKGROUND_POLY_4:
				pixel = poly_4(c, (double) j, (double) i);
			}
			background[j + i * width] = pixel;
		}
	}

	/* free memory */
	gsl_multifit_linear_free(work);
	gsl_matrix_free(J);
	gsl_vector_free(y);
	gsl_vector_free(w);
	gsl_vector_free(c);
	gsl_matrix_free(cov);

	return TRUE;
}

static background_sample *get_sample(float *buf, const int xx,
		const int yy, const int w, const int h) {
	size_t size = SAMPLE_SIZE * SAMPLE_SIZE;
	int radius = SAMPLE_SIZE / 2;
	background_sample *sample = (background_sample *) g_malloc(sizeof(background_sample));
	if (!sample) {
		PRINT_ALLOC_ERR;
		return NULL;
	}

	int n = 0;
	double *data = calloc(size, sizeof(double));
	if (!data) {
		free(sample);
		PRINT_ALLOC_ERR;
		return NULL;
	}
	for (int y = yy - radius; y <= yy + radius; y ++) {
		for (int x = xx - radius; x <= xx + radius; x ++) {
			if (y >= 0 && y < h) {
				if (x >= 0 && x < w) {
					data[n++] = (double)buf[x + y * w];
				}
			}
		}
	}
	gsl_stats_minmax(&sample->min, &sample->max, data, 1, size);
	sample->mean = gsl_stats_mean(data, 1, size);
	sample->median[RLAYER] = quickmedian_double(data, size);
	sample->median[GLAYER] = sample->median[BLAYER] = sample->median[RLAYER];
	sample->position.x = xx;
	sample->position.y = yy;
	sample->size = SAMPLE_SIZE;
	sample->valid = TRUE;

	free(data);
	return sample;
}

static double get_sample_median(const double *buf, const int xx,
		const int yy, const int w, const int h) {
	size_t size = SAMPLE_SIZE * SAMPLE_SIZE;
	int radius = SAMPLE_SIZE / 2;

	int n = 0;
	double *data = calloc(size, sizeof(double));
	if (!data) {
		PRINT_ALLOC_ERR;
		return -1.0;
	}
	for (int y = yy - radius; y <= yy + radius; y ++) {
		for (int x = xx - radius; x <= xx + radius; x ++) {
			if (y >= 0 && y < h) {
				if (x >= 0 && x < w) {
					data[n++] = buf[x + y * w];
				}
			}
		}
	}
	double median = quickmedian_double(data, size);

	free(data);
	return median;
}

static unsigned int _rand(guint64 *const p_rng) {
	*p_rng = *p_rng * 1103515245 + 12345U;
	return (unsigned int) *p_rng;
}

static gboolean convert_fits_to_img(fits *fit, double *image, int channel, gboolean add_dither) {
	guint64 seed = time(NULL);

	float invnorm = (float)(1.0 / USHRT_MAX);
	const int height = fit->ry;
	const int width = fit->rx;
	if (fit->type == DATA_USHORT) {
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				image[y * width + x] = fit->pdata[channel][(height - y - 1) * width + x] * invnorm;
				if (add_dither) {
					/* add dithering in order to avoid colour banding */
					image[y * width + x] += (_rand(&seed) % 1048576) * 0.000000000095367431640625f;
				}
			}
		}
	} else {
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				image[y * width + x] = fit->fpdata[channel][(height - y - 1) * width + x];
				if (add_dither) {
					/* add dithering in order to avoid colour banding */
					image[y * width + x] += (_rand(&seed) % 1048576) * 0.000000000095367431640625f;
				}
			}
		}
	}
	return TRUE;
}

static float* convert_fits_to_luminance(fits *fit, threading_type threads) {
	g_assert(fit->type == DATA_USHORT || fit->type == DATA_FLOAT);
	const size_t n = fit->naxes[0] * fit->naxes[1];
	float invnorm = (float)(1.0 / USHRT_MAX);
	/* allocating memory to image */
	float *image = malloc(n * sizeof(float));
	if (!image) {
		PRINT_ALLOC_ERR;
		return NULL;
	}

	const int height = fit->ry;
	const int width = fit->rx;

#ifdef _OPENMP
	limit_threading(&threads, 200000/width, height);
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			size_t in_idx = (height - y - 1) * width + x;
			size_t out_idx = y * width + x;
			if (fit->naxes[2] > 1) {
				float r, g, b;
				if (fit->type == DATA_USHORT) {
					r = fit->pdata[RLAYER][in_idx] * invnorm;
					g = fit->pdata[GLAYER][in_idx] * invnorm;
					b = fit->pdata[BLAYER][in_idx] * invnorm;
				} else if (fit->type == DATA_FLOAT) {
					r = fit->fpdata[RLAYER][in_idx];
					g = fit->fpdata[GLAYER][in_idx];
					b = fit->fpdata[BLAYER][in_idx];
				}
				image[out_idx] = 0.3333f * r + 0.3333f * g + 0.3333f * b;
			} else {
				if (fit->type == DATA_USHORT) {
					image[out_idx] = fit->pdata[RLAYER][in_idx] * invnorm;
				} else if (fit->type == DATA_FLOAT) {
					image[out_idx] = fit->fpdata[RLAYER][in_idx];
				}
			}
		}
	}

	return image;
}

static void convert_img_to_fits(double *image, fits *fit, int channel) {
	const int height = fit->ry;
	const int width = fit->rx;
	if (fit->type == DATA_USHORT) {
		WORD *buf = fit->pdata[channel];
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				size_t in_idx = (height - y - 1) * width + x;
				size_t out_idx = y * width + x;
				buf[out_idx] = round_to_WORD(image[in_idx] * USHRT_MAX);
			}
		}
	} else if (fit->type == DATA_FLOAT) {
		float *buf = fit->fpdata[channel];
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				size_t in_idx = (height - y - 1) * width + x;
				size_t out_idx = y * width + x;
				buf[out_idx] = (float) image[in_idx];
			}
		}
	}
}

static GSList *generate_samples(fits *fit, int nb_per_line, double tolerance, size_t size, threading_type threads) {
	unsigned int x, y;
	int nx = fit->rx;
	int ny = fit->ry;
	size_t n = fit->naxes[0] * fit->naxes[1];
	GSList *list = NULL;

	float *image = convert_fits_to_luminance(fit, threads);	// upside down
	if (!image) return NULL;
	float median = (float)histogram_median_float(image, n, threads);
	float mad0 = (float)siril_stats_float_mad(image, n, median, threads, NULL);
	double threshold = median + mad0 * exp(tolerance);

	siril_debug_print("Background gradient: %d samples per line, threshold %f\n", nb_per_line, threshold);

	int dist = nx / nb_per_line;
	size_t radius = size / 2;
	int startx = ((nx - size) % dist) / 2;
	int starty = ((ny - size) % dist) / 2;

	// TODO: parallelize, but it's hard to maintain sample list order, work on an array
	for (y = starty; y <= ny - radius; y += dist) {
		for (x = startx; x <= nx - radius; x += dist) {
			background_sample *sample = get_sample(image, x, y, nx, ny);
			if (sample->median[RLAYER] > 0.0
					&& sample->median[RLAYER] <= threshold) {
				list = g_slist_prepend(list, sample);
			} else {
				g_free(sample);
			}
		}
	}
	list = g_slist_reverse(list);
	free(image);

	return list;
}

static GSList *update_median_for_rgb_samples(GSList *orig, fits *fit) {
	const int nx = fit->rx;
	const int ny = fit->ry;

	const size_t n = fit->naxes[0] * fit->naxes[1];
	double *channelData = malloc(n * sizeof(double));
	if (!channelData) {
		PRINT_ALLOC_ERR;
		return NULL;
	}

	for (int channel = 0; channel < fit->naxes[2]; channel++) {
		convert_fits_to_img(fit, channelData, channel, FALSE);

		for (GSList *list = orig; list; list = list->next) {
			background_sample *sample = (background_sample*) list->data;
			sample->median[channel] = get_sample_median(channelData,
					sample->position.x, sample->position.y, nx, ny);
		}
	}

	free(channelData);
	return orig;
}

static poly_order get_poly_order() {
	GtkComboBox *combo_box_poly_order = GTK_COMBO_BOX(lookup_widget("box_background_order"));
	return gtk_combo_box_get_active(combo_box_poly_order);
}

static background_correction get_correction_type() {
	GtkComboBox *combo_box_correction = GTK_COMBO_BOX(lookup_widget("box_background_correction"));
	return gtk_combo_box_get_active(combo_box_correction);
}

static int get_nb_samples_per_line() {
	GtkSpinButton *nb_samples = GTK_SPIN_BUTTON(lookup_widget("spin_background_nb_samples"));
	return gtk_spin_button_get_value_as_int(nb_samples);
}

static double get_tolerance_value() {
	GtkRange *tol = GTK_RANGE(lookup_widget("scale_background_tolerance"));
	return gtk_range_get_value(tol);
}

static void remove_gradient(double *img, const double *background, size_t ndata, background_correction type, threading_type threads) {
	size_t i;
	double mean;

	// TODO: use cached stats
	mean = gsl_stats_mean(img, 1, ndata);

	switch (type) {
	default:
	case BACKGROUND_CORRECTION_SUBTRACT:
#ifdef _OPENMP
	limit_threading(&threads, 300000, ndata);
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
		for (i = 0; i < ndata; i++) {
			img[i] -= background[i];
			img[i] += mean;
		}
		break;
	case BACKGROUND_CORRECTION_DIVIDE:
#ifdef _OPENMP
	limit_threading(&threads, 300000, ndata);
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
		for (i = 0; i < ndata; i++) {
			img[i] /= background[i];
			img[i] *= mean;
		}
	}
}

/************* PUBLIC FUNCTIONS *************/

int get_sample_radius() {
	return SAMPLE_SIZE / 2;
}

gboolean is_dither_checked() {
	return (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lookup_widget("bkg_dither_button"))));
}

void free_background_sample_list(GSList *list) {
	if (list == NULL) return;
	g_slist_free_full(list, g_free);
}

GSList *add_background_sample(GSList *orig, fits *fit, point pt) {
	GSList *list;
	int nx = fit->rx;
	int ny = fit->ry;
	float *image;

	image = convert_fits_to_luminance(fit, MULTI_THREADED);

	list = orig;

	background_sample *sample = get_sample(image, pt.x, pt.y, nx, ny);
	list = g_slist_append(list, sample);

	free(image);

	return list;
}

GSList *remove_background_sample(GSList *orig, fits *fit, point pt) {
	GSList *list;
	float *image;
	double min_radius = DBL_MAX;

	image = convert_fits_to_luminance(fit, MULTI_THREADED);

	/* search for the min radius vale */
	for (list = orig; list; list = list->next) {
		background_sample *sample = (background_sample *)list->data;
		double dx = pt.x - sample->position.x;
		double dy = pt.y - sample->position.y;
		double radius = sqrt(dx * dx + dy * dy);

		min_radius = min(min_radius, radius);
	}
	/* remove this value */
	for (list = orig; list; list = list->next) {
		background_sample *sample = (background_sample *)list->data;
		double dx = pt.x - sample->position.x;
		double dy = pt.y - sample->position.y;
		double radius = sqrt(dx * dx + dy * dy);

		if (radius == min_radius) {
			orig = g_slist_remove(orig, sample);
			g_free((background_sample *) sample);
		}
	}
	free(image);

	return orig;
}

/* generates samples and stores them in com.grad_samples */
void generate_background_samples(int nb_of_samples, double tolerance) {
	free_background_sample_list(com.grad_samples);
	com.grad_samples = generate_samples(&gfit, nb_of_samples, tolerance, SAMPLE_SIZE, MULTI_THREADED);
	if (gfit.naxes[2] > 1) {
		com.grad_samples = update_median_for_rgb_samples(com.grad_samples, &gfit);
	}

	redraw(REDRAW_OVERLAY);
}

/* uses samples from com.grad_samples */
gboolean remove_gradient_from_image(background_correction correction, poly_order degree, gboolean use_dither) {
	gchar *error;
	double *background = malloc(gfit.ry * gfit.rx * sizeof(double));
	if (!background && !com.script) {
		PRINT_ALLOC_ERR;
		set_cursor_waiting(FALSE);
		return FALSE;
	}

	const size_t n = gfit.naxes[0] * gfit.naxes[1];
	double *image = malloc(n * sizeof(double));
	if (!image) {
		free(background);
		PRINT_ALLOC_ERR;
		return FALSE;
	}

	for (int channel = 0; channel < gfit.naxes[2]; channel++) {
		/* compute background */
		convert_fits_to_img(&gfit, image, channel, use_dither);
		if (!computeBackground(com.grad_samples, background, channel, gfit.rx,
				gfit.ry, degree, &error)) {
			free(image);
			free(background);
			siril_message_dialog(GTK_MESSAGE_ERROR, _("Not enough samples."),
					error);
			set_cursor_waiting(FALSE);
			return FALSE;
		}
		/* remove background */
		const char *c_name = vport_number_to_name(channel);
		siril_log_message(_("Background extraction from %s channel.\n"), c_name);
		remove_gradient(image, background, n, correction, MULTI_THREADED);
		convert_img_to_fits(image, &gfit, channel);

	}
	/* free memory */
	free(image);
	free(background);
	return TRUE;
}

/** Apply for sequence **/

static int background_image_hook(struct generic_seq_args *args, int o, int i, fits *fit,
		rectangle *_, int threads) {
	struct background_data *b_args = (struct background_data*) args->user;

	gchar *error;
	double *background = malloc(fit->ry * fit->rx * sizeof(double));
	if (!background) {
		PRINT_ALLOC_ERR;
		error = _("Out of memory - aborting");
		siril_log_message(error);
		return 1;
	}

	GSList *samples = generate_samples(fit, b_args->nb_of_samples, b_args->tolerance, SAMPLE_SIZE, (threading_type)threads);
	if (!samples) {
		free(background);
		return 1;
	}

	if (fit->naxes[2] > 1) {
		samples = update_median_for_rgb_samples(samples, fit);
	}

	const size_t n = fit->naxes[0] * fit->naxes[1];
	double *image = malloc(n * sizeof(double));
	if (!image) {
		free(background);
		free_background_sample_list(samples);
		PRINT_ALLOC_ERR;
		return 1;
	}

	for (int channel = 0; channel < fit->naxes[2]; channel++) {
		/* compute background */
		convert_fits_to_img(fit, image, channel, b_args->dither);
		if (!computeBackground(samples, background, channel, fit->rx, fit->ry, b_args->degree, &error)) {
			if (error) {
				siril_log_message(error);
			}
			free(image);
			free(background);
			free_background_sample_list(samples);
			return 1;
		}
		/* remove background */
		remove_gradient(image, background, fit->naxes[0] * fit->naxes[1], b_args->correction, (threading_type)threads);
		convert_img_to_fits(image, fit, channel);

	}
	/* free memory */
	free(image);
	free(background);
	free_background_sample_list(samples);

	return 0;
}

static int background_mem_limits_hook(struct generic_seq_args *args, gboolean for_writer) {
	unsigned int MB_per_image, MB_avail;
	int limit = compute_nb_images_fit_memory(args->seq, 1.0, FALSE, &MB_per_image, NULL, &MB_avail);
	unsigned int required = MB_per_image;
	if (limit > 0) {
		/* allocations:
		 * generate_samples convert_fits_to_luminance allocates         rx * ry * sizeof(float)
		 * generate_samples allocates a buffer for MAD computation      rx * ry * sizeof(double)
		 * both are freed at generate_samples exit
		 * for color images:
		 *	update_median_for_rgb_samples allocates for median      rx * ry * sizeof(double)
		 * freed at update_median_for_rgb_samples exit
		 * remove_gradient_from_image allocates the background image to rx * ry * sizeof(double)
		 * remove_gradient_from_image allocates the image            to rx * ry * sizeof(double)
		 *
		 * so at maximum, ignoring the samples, we need 2 times the double channel size.
		 */
		uint64_t double_channel_size = args->seq->rx * args->seq->ry * sizeof(double);
		unsigned int double_channel_size_MB = double_channel_size / BYTES_IN_A_MB;
		required = MB_per_image + double_channel_size_MB * 2;
		int thread_limit = MB_avail / required;
		if (thread_limit > com.max_thread)
                        thread_limit = com.max_thread;

		if (for_writer) {
                        /* we allow the already allocated thread_limit images,
                         * plus how many images can be stored in what remains
                         * unused by the main processing */
                        limit = thread_limit + (MB_avail - required * thread_limit) / MB_per_image;
                } else limit = thread_limit;

	}
	if (limit == 0) {
		gchar *mem_per_thread = g_format_size_full(required * BYTES_IN_A_MB, G_FORMAT_SIZE_IEC_UNITS);
		gchar *mem_available = g_format_size_full(MB_avail * BYTES_IN_A_MB, G_FORMAT_SIZE_IEC_UNITS);

		siril_log_color_message(_("%s: not enough memory to do this operation (%s required per image, %s considered available)\n"),
				"red", args->description, mem_per_thread, mem_available);

		g_free(mem_per_thread);
		g_free(mem_available);
	} else {
#ifdef _OPENMP
		if (for_writer) {
			int max_queue_size = com.max_thread * 3;
			if (limit > max_queue_size)
				limit = max_queue_size;
		}
		siril_debug_print("Memory required per thread: %u MB, per image: %u MB, limiting to %d %s\n",
				required, MB_per_image, limit, for_writer ? "images" : "threads");
#else
		if (!for_writer)
			limit = 1;
		else if (limit > 3)
			limit = 3;
#endif
	}
	return limit;
}

void apply_background_extraction_to_sequence(struct background_data *background_args) {
	struct generic_seq_args *args = create_default_seqargs(background_args->seq);
	args->filtering_criterion = seq_filter_included;
	args->nb_filtered_images = background_args->seq->selnum;
	args->compute_mem_limits_hook = background_mem_limits_hook;
	args->prepare_hook = seq_prepare_hook;
	args->finalize_hook = seq_finalize_hook;
	args->image_hook = background_image_hook;
	args->stop_on_error = FALSE;
	args->description = _("Background Extraction");
	args->has_output = TRUE;
	args->output_type = get_data_type(args->seq->bitpix);
	args->new_seq_prefix = background_args->seqEntry;
	args->load_new_sequence = TRUE;
	args->user = background_args;

	background_args->fit = NULL;	// not used here

	start_in_new_thread(generic_sequence_worker, args);
}

/**** getter and setter ***/

gboolean background_sample_is_valid(background_sample *sample) {
	return sample->valid;
}

gdouble background_sample_get_size(background_sample *sample) {
	return sample->size;
}

point background_sample_get_position(background_sample *sample) {
	return sample->position;
}

/************* CALLBACKS *************/

void on_background_generate_clicked(GtkButton *button, gpointer user_data) {
	set_cursor_waiting(TRUE);
	int nb_of_samples;
	double tolerance;

	nb_of_samples = get_nb_samples_per_line();
	tolerance = get_tolerance_value();

	generate_background_samples(nb_of_samples, tolerance);
	set_cursor_waiting(FALSE);
}

void on_background_clear_all_clicked(GtkButton *button, gpointer user_data) {
	free_background_sample_list(com.grad_samples);
	com.grad_samples = NULL;

	redraw(REDRAW_OVERLAY);
	set_cursor_waiting(FALSE);
}

void on_background_ok_button_clicked(GtkButton *button, gpointer user_data) {
	GtkToggleButton *seq_button = GTK_TOGGLE_BUTTON(lookup_widget("checkBkgSeq"));
	if (gtk_toggle_button_get_active(seq_button)
			&& sequence_is_loaded()) {
		struct background_data *args = malloc(sizeof(struct background_data));
		args->nb_of_samples = get_nb_samples_per_line();
		args->tolerance = get_tolerance_value();
		args->correction = get_correction_type();
		args->degree = get_poly_order();
		args->dither = is_dither_checked();
		if (args->degree > BACKGROUND_POLY_1) {
			int confirm = siril_confirm_dialog(_("Polynomial order seems too high."),
					_("You are about to process a sequence of preprocessed files with "
							"a polynomial degree greater than 1. This is unlikely because such "
							"gradients are often linear and a correction with a polynomial "
							"function of degree 1 is probably enough."), _("Extract Background"));
			if (!confirm) {
				free(args);
				set_cursor_waiting(FALSE);
				return;
			}
		}

		set_cursor_waiting(TRUE);

		args->seqEntry = gtk_entry_get_text(GTK_ENTRY(lookup_widget("entryBkgSeq")));
		if (args->seqEntry && args->seqEntry[0] == '\0')
			args->seqEntry = "bkg_";
		args->seq = &com.seq;
		/* now we uncheck the button */
		gtk_toggle_button_set_active(seq_button, FALSE);
		apply_background_extraction_to_sequence(args);
	} else {
		if (com.grad_samples == NULL) {
			return;
		}
		set_cursor_waiting(TRUE);

		background_correction correction = get_correction_type();
		poly_order degree = get_poly_order();
		gboolean use_dither = is_dither_checked();
		undo_save_state(&gfit, _("Background extraction (Correction: %s)"),
				correction == BACKGROUND_CORRECTION_DIVIDE ? "Division" : "Subtraction");
		remove_gradient_from_image(correction, degree, use_dither);

		invalidate_stats_from_fit(&gfit);
		adjust_cutoff_from_updated_gfit();
		redraw(REMAP_ALL);
		set_cursor_waiting(FALSE);
	}
}

void on_background_close_button_clicked(GtkButton *button, gpointer user_data) {
	siril_close_dialog("background_extraction_dialog");
}

void on_background_extraction_dialog_hide(GtkWidget *widget, gpointer user_data) {
	free_background_sample_list(com.grad_samples);
	com.grad_samples = NULL;
	mouse_status = MOUSE_ACTION_SELECT_REG_AREA;
	redraw(REDRAW_OVERLAY);
}

void on_background_extraction_dialog_show(GtkWidget *widget, gpointer user_data) {
	mouse_status = MOUSE_ACTION_DRAW_SAMPLES;
}
