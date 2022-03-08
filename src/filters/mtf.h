#ifndef _MTF_H_
#define _MTF_H_

#include "core/siril.h"

struct mtf_params {
	float midtones, shadows, highlights;
};

/* Auto-stretch parameters */
#define SHADOWS_CLIPPING -2.80f /* Shadows clipping point measured in sigma units from the main histogram peak. */
#define TARGET_BACKGROUND 0.25f /* final "luminance" of the image for autostretch in the [0,1] range */

float MTF(float x, float m, float lo, float hi);
float MTFp(float x, struct mtf_params params);

void apply_linked_mtf_to_fits(fits *from, fits *to, struct mtf_params params);
int find_linked_midtones_balance(fits *fit, struct mtf_params *result);

void apply_unlinked_mtf_to_fits(fits *from, fits *to, struct mtf_params *params);
int find_unlinked_midtones_balance(fits *fit, float shadows_clipping, float target_bg, struct mtf_params *results);

#endif
