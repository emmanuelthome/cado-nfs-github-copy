#include <stdio.h>
#include <stdlib.h>
#include "macros.h"

#include "types.h"
#include "fppol.h"
#include "ffspol.h"
#include "norm.h"
#include "qlat.h"
#include "ijvec.h"



/* Function fppol_pow computes the power-th power of a polynomial
   should power be an unsigned int?  
   Should this function survive? 
   Will anyone use it ?  
   Does it have its place here? */
void fppol_pow(fppol_t res, fppol_t in, int power)
{
  fppol_t tmp;
  fppol_init(tmp);
  fppol_set(tmp, in);
  for (int i = 0; i < power; i++)
    fppol_mul(tmp, tmp, in);
  fppol_set(res, tmp);
  fppol_clear(tmp);
}  

/* Function computing the norm of ffspol at (a,b)
   norm = b^d * ffspol(a/b), d = deg(ffspol) */

void ffspol_norm(fppol_t norm, ffspol_ptr ffspol, fppol_t a, fppol_t b)
{
  fppol_t *pow_b;
  fppol_t pow_a;
  fppol_t pol_norm_i;
  fppol_t tmp_norm;
  
  fppol_init(pow_a);
  fppol_init(pol_norm_i);
  fppol_init(tmp_norm);

  fppol_set_zero(pol_norm_i);
  fppol_set_zero(tmp_norm);
  fppol_set_one(pow_a);

  /* pow_b contains b^d, b^{d-1}, ... , b^2, b, 1 */
  pow_b = (fppol_t *)malloc((ffspol->alloc) * sizeof(fppol_t));
  fppol_init(pow_b[ffspol->deg]);
  fppol_set_one(pow_b[ffspol->deg]);

  for (int i = ffspol->deg - 1; i > -1; i--) {
    fppol_init(pow_b[i]);
    fppol_mul(pow_b[i], pow_b[i+1], b);
  }
  for (int i = 0; i < ffspol->deg + 1; i++) {
    fppol_mul(pol_norm_i, ffspol->coeffs[i], pow_b[i]);
    fppol_mul(pol_norm_i, pol_norm_i, pow_a);
    fppol_add(tmp_norm, tmp_norm, pol_norm_i);
    fppol_mul(pow_a, pow_a, a);
  }
  fppol_set(norm, tmp_norm);
  fppol_clear(pow_a);
  fppol_clear(pol_norm_i);
  fppol_clear(tmp_norm);
  for (int i = 0; i < ffspol->deg; i++)
    fppol_clear(pow_b[i]);
  free(pow_b);
}

/* max_special(prev_max, j, &repeated) returns the maximum of prev_max
   and j
   The value repeated should be initialized to 1
   If there is only one maximum, then repeated is one
   If prev_max == j, the variable (*repeated) is *incremented* 
   This function is completely ad hoc to be used in a for loop
   like max = max_special(max, tab[i], &repeated) so the result
   will be different from max = max_special(tab[i], max, &repeated) */

static int max_special(int prev_max, int j, int *repeated)
{
  if (prev_max == j) {
    (*repeated)++;
    return prev_max;
  } 
  else if (prev_max > j) {
      return prev_max;
  }
  else {
    *repeated = 1;
    return j;
  }
}

     
/* deg_norm returns the degree of the norm. 
   If during computation of pol_norm_i, only one pol_norm_i has
   maximal degree then deg_norm is equal to this degree otherwise, we
   have to compute the norm and call the fppol_deg function on it */
int deg_norm(ffspol_ptr ffspol, fppol_t a, fppol_t b)
{
  int deg, max_deg = -1;
  int repeated = 1;
  int dega = fppol_deg(a);
  int degb = fppol_deg(b);
  static int c_deg = 0;
  static int c_tot = 0;

#if 0
  if ((c_tot & 0xFFF) == 1)
    fprintf(stderr, "deg_norm stat: %d / %d\n", c_deg, c_tot);
#endif
  c_tot++;
  for (int i = 0; i < ffspol->deg + 1; i++) {
    deg = fppol_deg(ffspol->coeffs[i]) + i * dega + (ffspol->deg - i) * degb;
    max_deg = max_special(max_deg, deg, &repeated);
  }

#if FP_SIZE == 2
  if (repeated & 1u) {
#else
  if (repeated == 1) {
#endif
      c_deg++;
      return max_deg;
  }
  else {
    /* We should think about a cheaper way to compute this degree
       otherwise */
    fppol_t norm;
    fppol_init(norm);
    ffspol_norm(norm, ffspol, a, b);
    deg = fppol_deg(norm);
    fppol_clear(norm);
    return deg;
  }
}


/* Function init_norms 
   For each (i,j), it compute the corresponding (a,b) using ij2ab from
   qlat.h. Then it computes deg_norm(ffspol, a, b).
   In a first approximation, we will assume that it fits in an
   unsigned char.
   For the moment i and j are assumed to be unsigned int for their
   limb part 
   Enumerating i, j the following way only works for charac. 2 
   
   The last parameter is a boolean that tells whether we are on the
   side of the special q. If so, then the degree of q must be subtracted
   from the norm.
   */

void init_norms(unsigned char *S, ffspol_ptr ffspol, int I, int J, qlat_t qlat,
        int sqside)
{
  fppol_t a, b;
  unsigned int II, JJ;
  fppol_init(a);
  fppol_init(b);
  int degq = 0;
  if (sqside)
      degq = sq_deg(qlat->q);
  
  {
      ij_t max;
      ij_set_ti(max, I);
      II = ij_get_ui(max, I+1);
      ij_set_ti(max, J);
      JJ = ij_monic_get_ui(max, J+1);
  }


  ijvec_t V;
  for (unsigned int j = 0; j < JJ; ++j) {
    if (!ij_monic_set_ui(V->j, j, J))
      continue;
    if (!ij_is_monic(V->j))
        continue;
    ij_set_zero(V->i);
    unsigned int j0 = ijvec_get_pos(V, I, J);
    for (unsigned int i = 0; i < II; ++i) {
      if (!ij_set_ui(V->i, i, I))
        continue;
      unsigned int position = i + j0;
#ifdef TRACE_POS
      if (position == TRACE_POS) {
          fprintf(stderr, "TRACE_POS(%d): (i,j) = (", position);
          ij_out(stderr, V->i); fprintf(stderr, " ");
          ij_out(stderr, V->j); fprintf(stderr, ")\n");
          fprintf(stderr, "TRACE_POS(%d): norm = ", position);
          fppol_t norm;
          fppol_init(norm);
          ij2ab(a, b, V->i, V->j, qlat);
          ffspol_norm(norm, ffspol, a, b);
          fppol_out(stderr, norm);
          fppol_clear(norm);
          fprintf(stderr, "\n");
          fprintf(stderr, "TRACE_POS(%d): degnorm - deg(sq) = %d\n",
                  position, fppol_deg(norm)-degq);
      }
#endif
      if (S[position] != 255) {
        ij2ab(a, b, V->i, V->j, qlat);
        int deg = deg_norm(ffspol, a, b);
        if (deg > 0) {
          ASSERT_ALWAYS(deg < 255);
          S[position] = deg - degq;
        }
        else
          S[position] = 255;
      }
    }
  }
  fppol_clear(a);
  fppol_clear(b);
}
