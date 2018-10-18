#if !defined(MATCH_H)
#define MATCH_H

#include "core/siril.h"


#define NB_OF_MATCHING_TRY 3


int new_star_match(fitted_PSF **s1, fitted_PSF **s2, int n, int nobj_override,
		Homography *H, point image_size);

#endif   /* MATCH_H */
