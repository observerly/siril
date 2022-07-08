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

#include <math.h>
#include <gsl/gsl_matrix.h>
#include <string.h>

#include "core/siril.h"
#include "core/proto.h"
#include "algos/PSF.h"
#include "algos/photometry.h"

#define hampel_a   1.7
#define hampel_b   3.4
#define hampel_c   8.5
#define sign(x,y)  ((y)>=0?fabs(x):-fabs(x))
#define epsilon(x) 0.00000001
#define maxit      50
#define min_sky    5

static double hampel(double x) {
	if (x >= 0) {
		if (x < hampel_a)
			return x;
		if (x < hampel_b)
			return hampel_a;
		if (x < hampel_c)
			return hampel_a * (x - hampel_c) / (hampel_b - hampel_c);
	} else {
		if (x > -hampel_a)
			return x;
		if (x > -hampel_b)
			return -hampel_a;
		if (x > -hampel_c)
			return hampel_a * (x + hampel_c) / (hampel_b - hampel_c);
	}
	return 0.0;
}

static double dhampel(double x) {
	if (x >= 0) {
		if (x < hampel_a)
			return 1;
		if (x < hampel_b)
			return 0;
		if (x < hampel_c)
			return hampel_a / (hampel_b - hampel_c);
	} else {
		if (x > -hampel_a)
			return 1;
		if (x > -hampel_b)
			return 0;
		if (x > -hampel_c)
			return -hampel_a / (hampel_b - hampel_c);
	}
	return 0.0;
}

static double qmedD(int n, double *a)
	/* Vypocet medianu algoritmem Quick Median (Wirth) */
{
	double w;
	int k = ((n & 1) ? (n / 2) : ((n / 2) - 1));
	int l = 0;
	int r = n - 1;

	while (l < r) {
		double x = a[k];
		int i = l;
		int j = r;
		do {
			while (a[i] < x)
				i++;
			while (x < a[j])
				j--;
			if (i <= j) {
				w = a[i];
				a[i] = a[j];
				a[j] = w;
				i++;
				j--;
			}
		} while (i <= j);
		if (j < k)
			l = i;
		if (k < i)
			r = j;
	}
	return a[k];
}

static int robustmean(int n, double *x, double *mean, double *stdev)
	/* Newton's iterations */
{
	int i, it;
	double a, c, dt, r, s, psir;
	double *xx;

	if (n < 1) {
		if (mean)
			*mean = 0.0; /* a few data */
		if (stdev)
			*stdev = -1.0;
		return 1;
	}
	if (n == 1) { /* only one point, but correct case */
		if (mean)
			*mean = x[0];
		if (stdev)
			*stdev = 0.0;
		return 0;
	}

	/* initial values:
	   - median is the first approximation of location
	   - MAD/0.6745 is the first approximation of scale */
	xx = malloc(n * sizeof(double));
	if (!xx) {
		PRINT_ALLOC_ERR;
		return 1;
	}
	memcpy(xx, x, n * sizeof(double));
	a = qmedD(n, xx);
	for (i = 0; i < n; i++)
		xx[i] = fabs(x[i] - a);
	s = qmedD(n, xx) / 0.6745;
	free(xx);

	/* almost identical points on input */
	if (fabs(s) < epsilon(s)) {
		if (mean)
			*mean = a;
		if (stdev) {
			double sum = 0.0;
			for (i = 0; i < n; i++)
				sum += (x[i] - a) * (x[i] - a);
			*stdev = sqrt(sum / n);
		}
		return 0;
	}

	/* corrector's estimation */
	dt = 0;
	c = s * s * n * n / (n - 1);
	for (it = 1; it <= maxit; it++) {
		double sum1, sum2, sum3;
		sum1 = sum2 = sum3 = 0.0;
		for (i = 0; i < n; i++) {
			r = (x[i] - a) / s;
			psir = hampel(r);
			sum1 += psir;
			sum2 += dhampel(r);
			sum3 += psir * psir;
		}
		if (fabs(sum2) < epsilon(sum2))
			break;
		double d = s * sum1 / sum2;
		a = a + d;
		dt = c * sum3 / (sum2 * sum2);
		if ((it > 2) && ((d * d < 1e-4 * dt) || (fabs(d) < 10.0 * epsilon(d))))
			break;
	}
	if (mean)
		*mean = a;
	if (stdev)
		*stdev = (dt > 0 ? sqrt(dt) : 0);
	return 0;
}

static double getMagnitude(double intensity) {
	return -2.5 * log10(intensity);
}

double get_camera_gain(fits *fit) {
	double gain = com.pref.phot_set.gain;
	if (fit->cvf > 0.0)
		gain = fit->cvf;
	if (fit->type == DATA_FLOAT)
		return gain * USHRT_MAX_DOUBLE;
	return gain;
}

static double getInnerRadius() {
	return com.pref.phot_set.inner;
}

static double getOuterRadius() {
	return com.pref.phot_set.outer;
}

static double getAperture() {
	return com.pref.phot_set.aperture;
}

static double getMagErr(double intensity, double area, int nsky, double skysig, double cvf, double *SNR) {
	double skyvar = skysig * skysig;/* variance of the sky brightness */
	double sigsq = skyvar / nsky;	/* square of the standard error of the mean sky brightness */
	double err1 = area * skyvar;
	double err2 = intensity / cvf;
	double err3 = sigsq * area * area;
	double noise = sqrt(err1 + err2 + err3);

	*SNR = 10.0 * log10(intensity / noise);

	return fmin(9.999, 1.0857 * noise / intensity);
}

static double lo_data() {
	if (gfit.type == DATA_FLOAT) {
		return (double) com.pref.phot_set.minval / USHRT_MAX_DOUBLE;
	} else {
		return (double) com.pref.phot_set.minval;
	}
}

static double hi_data() {
	if (gfit.type == DATA_FLOAT) {
		return (double) com.pref.phot_set.maxval / USHRT_MAX_DOUBLE;
	} else {
		return (double) com.pref.phot_set.maxval;
	}
}

/* Function that compute all photometric data. The result must be freed */
photometry *getPhotometryData(gsl_matrix* z, psf_star *psf, double gain,
		gboolean force_radius, gboolean verbose, psf_error *error) {
	int width = z->size2;
	int height = z->size1;
	int n_sky = 0, ret;
	int x, y, x1, y1, x2, y2;
	double r1, r2, r, rmin_sq, appRadius;
	double apmag = 0.0, mean = 0.0, stdev = 0.0, area = 0.0;
	gboolean valid = TRUE;
	photometry *phot;

	double xc = psf->x0 - 1;
	double yc = psf->y0 - 1;

	if (xc <= 0.0 || yc <= 0.0 || xc >= width || yc >= height) {
		if (error) *error = PSF_ERR_OUT_OF_WINDOW;
		return NULL;
	}

	r1 = getInnerRadius();
	r2 = getOuterRadius();
	appRadius = force_radius ? getAperture() : psf->fwhmx * 2.0;	// in order to be sure to contain star
	if (appRadius >= r1 && !force_radius) {
		if (verbose) {
			/* Translator note: radii is plural for radius */
			siril_log_message(_("Inner and outer radii are too small (%d required for inner). Please update values in preferences or with setphot.\n"), round_to_int(appRadius));
		}
		if (error) *error = PSF_ERR_INNER_TOO_SMALL;
		return NULL;
	}

	/* compute the bounding box of the outer radius around the star */
	x1 = xc - r2;
	if (x1 < 1)
		x1 = 1;
	x2 = xc + r2;
	if (x2 > width - 1)
		x2 = width - 1;
	y1 = yc - r2;
	if (y1 < 1)
		y1 = 1;
	y2 = yc + r2;
	if (y2 > height - 1)
		y2 = height - 1;

	int ndata = (y2 - y1) * (x2 - x1);
	if (ndata <= 0) {
		siril_log_color_message(_("An error occurred in your selection. Please make another selection.\n"), "red");
		if (error) *error = PSF_ERR_OUT_OF_WINDOW;
		return NULL;
	}
	double *data = calloc(ndata, sizeof(double));
	if (!data) {
		PRINT_ALLOC_ERR;
		if (error) *error = PSF_ERR_ALLOC;
		return NULL;
	}

	r1 *= r1;	// we square the radii to avoid doing
	r2 *= r2;	// sqrts for radius checks in the loop
	rmin_sq = (appRadius - 0.5) * (appRadius - 0.5);
	double lo = lo_data(), hi = hi_data();

	/* from the matrix containing pixel data, we extract pixels within
	 * limits of pixel value and of distance to the star centre for
	 * background evaluation */
	for (y = y1; y <= y2; ++y) {
		int yp = (y - yc) * (y - yc);
		for (x = x1; x <= x2; ++x) {
			r = yp + (x - xc) * (x - xc);
			double pixel = gsl_matrix_get(z, y, x);
			if (pixel > lo && pixel < hi) {
				double f = (r < rmin_sq ? 1 : appRadius - sqrt(r) + 0.5);
				if (f >= 0) {
					area += f;
					apmag += pixel * f;
				}
				/* annulus */
				if (r < r2 && r > r1) {
					data[n_sky] = pixel;
					n_sky++;
				}
			} else {
				valid = FALSE;
				if (error) *error = PSF_ERR_INVALID_PIX_VALUE;
			}
		}
	}
	if (area < 1.0) {
		siril_debug_print("area is < 1: not enough pixels of star data, too small aperture?\n");
		free(data);
		if (error) *error = PSF_ERR_APERTURE_TOO_SMALL;
		return NULL;
	}
	if (n_sky < min_sky) {
		if (verbose)
			siril_log_message(_("Warning: There aren't enough pixels"
						" in the sky annulus. You need to make a larger selection.\n"));
		if (error) *error = PSF_ERR_TOO_FEW_BG_PIX;
		free(data);
		return NULL;
	}

	ret = robustmean(n_sky, data, &mean, &stdev);
	free(data);
	if (ret > 0) {
		if (error) *error = PSF_ERR_MEAN_FAILED;
		return NULL;
	}

	phot = calloc(1, sizeof(photometry));
	if (!phot) {
		if (error) *error = PSF_ERR_ALLOC;
		PRINT_ALLOC_ERR;
	}
	else {
		double SNR = 0.0;
		double signalIntensity = apmag - (area * mean);

		phot->mag = getMagnitude(signalIntensity);
		phot->s_mag = getMagErr(signalIntensity, area, n_sky, stdev, gain, &SNR);
		if (phot->s_mag < 9.999) {
			phot->SNR = SNR;
			if (valid && error) *error = PSF_NO_ERR;
		} else {
			phot->SNR = 0.0;
			valid = FALSE;
			if (error) *error = PSF_ERR_INVALID_STD_ERROR;
		}
		phot->valid = valid;
	}

	return phot;
}

void initialize_photometric_param() {
	com.pref.phot_set.inner = 20;
	com.pref.phot_set.outer = 30;
	com.pref.phot_set.aperture = 10;
	com.pref.phot_set.force_radius = FALSE;
	com.pref.phot_set.gain = 2.3;
	com.pref.phot_set.minval = 0;
	com.pref.phot_set.maxval = 60000;
}

static const char *psf_error_to_string(psf_error err) {
	switch (err) {
		case PSF_NO_ERR:
			return _("no error");
		case PSF_ERR_ALLOC:
			return _("memory allocation");
		case PSF_ERR_UNSUPPORTED:
			return _("unsupported image type");
		case PSF_ERR_DIVERGED:
			return _("Gaussian fit failed");
		case PSF_ERR_OUT_OF_WINDOW:
			return _("not in area");
		case PSF_ERR_INNER_TOO_SMALL:
			return _("inner radius too small");
		case PSF_ERR_APERTURE_TOO_SMALL:
			return _("aperture too small");
		case PSF_ERR_TOO_FEW_BG_PIX:
			return _("not enough background");
		case PSF_ERR_MEAN_FAILED:
			return _("statistics failed");
		case PSF_ERR_INVALID_STD_ERROR:
			return _("invalid measurement error");
		case PSF_ERR_INVALID_PIX_VALUE:
			return _("pixel out of range");
		case PSF_ERR_WINDOW_TOO_SMALL:
			return _("area too small");
		case PSF_ERR_INVALID_IMAGE:
			return _("image is invalid");
		case PSF_ERR_OUT_OF_IMAGE:
			return _("not in image");
		default:
			return _("unknown error");
	}
}

void print_psf_error_summary(gint *code_sums) {
	GString *msg = g_string_new("Distribution of errors: ");
	gboolean first = TRUE;
	for (int i = 0; i < PSF_ERR_MAX_VALUE; i++) {
		if (code_sums[i] > 0) {
			if (!first)
				msg = g_string_append(msg, ", ");
			g_string_append_printf(msg, "%d %s", code_sums[i], psf_error_to_string(i));
			first = FALSE;
		}
	}

	gchar *str = g_string_free(msg, FALSE);
	siril_log_message("%s\n", str);
	g_free(str);
}
