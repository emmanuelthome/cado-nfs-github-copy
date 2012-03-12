#ifndef __IJVEC_H__
#define __IJVEC_H__

#include "types.h"
#include "fb.h"



/* Vector in the reduced q-lattice as a pair of polynomials (i,j).
 * Note that, by convention, j should be kept monic.
 *****************************************************************************/

// Structure definition.
typedef struct {
  ij_t i;
  ij_t j;
} __ijvec_struct;

// Type and pointer shorthands.
typedef       __ijvec_struct  ijvec_t[1];
typedef       __ijvec_struct *ijvec_ptr;
typedef const __ijvec_struct *ijvec_srcptr;

// Vector addition.
static inline
void ijvec_add(ijvec_ptr r, ijvec_srcptr u, ijvec_srcptr v)
{
  ij_add(r->i, u->i, v->i);
  ij_add(r->j, u->j, v->j);
}


// Corresponding position as an unsigned int.
typedef unsigned ijpos_t;

// Return a strict higher bound on the position, given the degree bounds
// I and J.
static inline
ijpos_t ijvec_get_max_pos(unsigned I, unsigned J)
{
  fppol64_t t;
  fppol64_set_ti(t, I+J);
  return fppol64_get_ui(t, I, J);
}

// Return the position corresponding to an (i,j)-vector.
static inline
ijpos_t ijvec_get_pos(ijvec_srcptr v, unsigned I, unsigned J)
{
  fppol64_t ii, jj;
  fppol64_set_ij       (ii, v->i);
  fppol64_set_ij       (jj, v->j);
  fppol64_mul_ti       (jj, jj, I);
  fppol64_add_disjoint (jj, jj, ii);
  return fppol64_get_ui(jj, I, J);
}

// Return the starting position of the line corresponding to j.
// Once i is known, just add the offset ijvec_get_offset(i, I) to compute the
// full position of the vector (i,j).
static inline
ijpos_t ijvec_get_start_pos(ij_srcptr j, unsigned I, unsigned J)
{
  fppol64_t jj;
  fppol64_set_ij       (jj, j);
  fppol64_mul_ti       (jj, jj, I);
  return fppol64_get_ui(jj, I, J);
}

// Return the position offset corresponding to i.
static inline
ijpos_t ijvec_get_offset(ij_srcptr i, unsigned I)
{
  return ij_get_ui(i, I, 0);
}

// Convert a position to an (i,j)-vector.
// Return 1 if successful.
static inline
int ijvec_set_pos(ijvec_ptr v, ijpos_t pos, unsigned I, unsigned J)
{
  fppol64_t ii, jj;
  if (!fppol64_set_ui(jj, pos, I, J))
    return 0;
  fppol64_mod_ti(ii, jj,  I);
  fppol64_div_ti(jj, jj,  I);
  ij_set_64(v->i, ii);
  ij_set_64(v->j, jj);
  return 1;
}



/* Basis of the p-lattice seen as a GF(p)-vector space of (i,j)-vectors.
 *****************************************************************************/

// Structure definition.
typedef struct {
  unsigned I, J;
  unsigned dim;
  ijvec_t *v;
} __ijbasis_struct;

// Type and pointer shorthands.
typedef       __ijbasis_struct  ijbasis_t[1];
typedef       __ijbasis_struct *ijbasis_ptr;
typedef const __ijbasis_struct *ijbasis_srcptr;

// Allocate memory for an (i,j)-basis of degrees bounded by I and J.
void ijbasis_init(ijbasis_ptr basis, unsigned I, unsigned J);

// Compute the (i,j)-basis of a given p-lattice.
void ijbasis_compute(ijbasis_ptr basis, fbideal_srcptr gothp);

// Clean up memory.
void ijbasis_clear(ijbasis_ptr basis);

#endif  /* __IJVEC_H__ */
