#include "cado.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#include "utils.h"
#include "makefb.h"
#include "vector.h"
#include "int64_poly.h"
#include "mat_int64.h"
#include "array.h"
#include "uint64_array.h"
#include "utils_mpz.h"
#include "utils_int64.h"
#include "utils_lattice.h"
#include "mat_double.h"
#include "double_poly.h"
#include <time.h>
#include <limits.h>
#include "utils_norm.h"

#include "ecm/facul.h"
#include "ecm/facul_doit.h"


//TODO: do a utils_norm.[ch]

//TODO: change % in test + add or substract.

/*
 * p = 10822639589
 * n = 6
 * f0 = 1366+1366*x^1+1368*x^2+1368*x^3+1368*x^4+1366*x^5+1365*x^6
 * f1 = 7917871+7917871*x^1-7916275*x^2-7916275*x^3-7916275*x^4+7917871*x^5
 *  +15834944*x^6
 * t = 3
 * lpb = 26
 * N_a = 2^23
 */

/*
 * Different mode:
   - TRACE_POS: follow all the operation make in a case of the array;
   - NUMBER_HIT: count the number of hit;
   - MEAN_NORM_BOUND: mean of the initialized norms;
   - MEAN_NORM_BOUND_SIEVE: mean of the log2 of the residual norms;
   - MEAN_NORM: mean of the norm we can expected if the initialisation compute
      the true norms;
   - …
 */

#ifdef TRACE_POS
FILE * file;
#endif // TRACE_POS

#ifdef MEAN_NORM_BOUND
double norm_bound = 0;
#endif // MEAN_NORM_BOUND

#ifdef MEAN_NORM
double norm = 0;
#endif // MEAN_NORM

#ifdef ASSERT_SIEVE
uint64_t index_old = 0;
#endif // ASSERT_SIEVE

#ifdef NUMBER_SURVIVALS
uint64_t number_survivals = 0;
uint64_t number_survivals_facto = 0;
#endif // NUMBER_SURVIVALS

/*
 * Build the matrix Mq for an ideal (q, g) with deg(g) = 1. The matrix need Tq
 *  with the good coefficients, i.e. t the coeffecients not reduced modulo q.
 *  Suppose that the matrix is good initialized. Can not verify if ideal->Tq
 *  and matrix has the good size.
 *
 * matrix: the matrix with the good initialization for the number of rows and
 * columns. We can not assert here if the numbers of values for Tq and matrix is
 *  good or not.
 * ideal: the ideal (q, g) with deg(g) = 1.
 */
void build_Mq_ideal_1(mat_Z_ptr matrix, ideal_1_srcptr ideal)
{
  ASSERT(matrix->NumRows == matrix->NumCols);

  //Reinitialize the coefficients.
  for (unsigned int row = 1; row <= matrix->NumRows; row++) {
    for (unsigned int col = 1; col <= matrix->NumCols; col++) {
      mat_Z_set_coeff_uint64(matrix, 0, row, col);
    }
  }
  //Coeffecients of the diagonal.
  mat_Z_set_coeff_uint64(matrix, ideal->ideal->r, 1, 1);
  for (unsigned int row = 2; row <= matrix->NumRows; row++) {
    mat_Z_set_coeff_uint64(matrix, 1, row, row);
  }
  //Coefficients of the first line.
  for (unsigned int col = 2; col <= matrix->NumCols; col++) {
    mat_Z_set_coeff(matrix, ideal->Tr[col - 2], 1, col);
  }
}

/*
 * Build the matrix Mq for an ideal (q, g) with deg(g) > 1. The matrix need Tq
 *  with the good coefficients, i.e. t the coeffecients not reduced modulo q.
 *  Suppose that the matrix is good initialized. Can not verify if ideal->Tq
 *  and matrix has the good size.
 *
 * matrix: Mq.
 * ideal: an ideal (q, g) with deg(g) > 1.
 */
void build_Mq_ideal_u(mat_Z_ptr matrix, ideal_u_srcptr ideal)
{
  ASSERT(matrix->NumRows == matrix->NumCols);

  for (unsigned int row = 1; row <= matrix->NumRows; row++) {
    for (unsigned int col = 1; col <= matrix->NumCols; col++) {
      mat_Z_set_coeff_uint64(matrix, 0, row, col);
    }
  }
  for (int row = 1; row <= ideal->ideal->h->deg; row++) {
    mat_Z_set_coeff_uint64(matrix, ideal->ideal->r, row, row);
  }
  for (int row = ideal->ideal->h->deg + 1; row <= (int)matrix->NumRows;
       row++) {
    mat_Z_set_coeff_uint64(matrix, 1, row, row);
  }
  for (int col = ideal->ideal->h->deg + 1; col <= (int)matrix->NumCols;
       col++) {
    for (int row = 1; row <= ideal->ideal->h->deg; row++) {
      mat_Z_set_coeff(matrix,
          ideal->Tr[row - 1][col - ideal->ideal->h->deg + 1], row, col);
    }
  }
}

/*
 * Build the matrix Mq for an ideal (q, g). The matrix need Tq
 *  with the good coefficients, i.e. t the coeffecients not reduced modulo q.
 *  Suppose that the matrix is good initialized. Can not verify if ideal->Tq
 *  and matrix has the good size.
 *
 * matrix: Mq.
 * ideal: an ideal (q, g) with deg(g) > 1.
 */
void build_Mq_ideal_spq(mat_Z_ptr matrix, ideal_spq_srcptr ideal)
{
  ASSERT(matrix->NumRows == matrix->NumCols);

  if (ideal->type == 0) {
    build_Mq_ideal_1(matrix, ideal->ideal_1);
  } else if (ideal->type == 1) {
    build_Mq_ideal_u(matrix, ideal->ideal_u);
  }
}

/*
 * pseudo_Tqr is the normalised Tqr obtained by compute_Tqr_1. If Tqr[0] != 0,
 *  pseudo_Tqr = [(-Tqr[0])^-1 mod r = a, a * Tqr[1], …].
 *
 * pseudo_Tqr: a matrix (a line here) obtained as describe above.
 * Tqr: the Tqr matrix.
 * t: dimension of the lattice.
 * ideal: the ideal r.
 */
void compute_pseudo_Tqr_1(uint64_t * pseudo_Tqr, uint64_t * Tqr,
    unsigned int t, ideal_1_srcptr ideal)
{
  unsigned int i = 0;
  while (Tqr[i] == 0 && i < t) {
    pseudo_Tqr[i] = 0;
    i++;
  }
  uint64_t inverse = ideal->ideal->r - 1;

  ASSERT(Tqr[i] == 1);
  ASSERT(inverse ==
         invmod_uint64((uint64_t)(-Tqr[i] + (int64_t)ideal->ideal->r),
                       ideal->ideal->r));
  pseudo_Tqr[i] = inverse;
  for (unsigned int j = i + 1; j < t; j++) {
    pseudo_Tqr[j] = (inverse * Tqr[j]) % ((int64_t)ideal->ideal->r);
  }
}

/*
 * Mqr is the a matrix that can generate vector c such that Tqr * c = 0 mod r.
 * Mqr = [[r a 0 … 0]
 *       [0 1 b … 0]
 *       [| | | | |]
 *       [0 0 0 0 1]]
 *
 * Mqr: the Mqr matrix.
 * Tqr: the Tqr matrix.
 * t: dimension of the lattice.
 * ideal: the ideal r.
 */
void compute_Mqr_1(mat_int64_ptr Mqr, uint64_t * Tqr, unsigned int t,
    ideal_1_srcptr ideal)
{
  ASSERT(Mqr->NumRows == t);
  ASSERT(Mqr->NumCols == t);

  unsigned int index = 0;
  while (Tqr[index] == 0) {
    index++;
  }

  mat_int64_init(Mqr, t, t);
  mat_int64_set_zero(Mqr);
  for (unsigned int i = 1; i <= t; i++) {
    Mqr->coeff[i][i] = 1;
  }
  Mqr->coeff[index + 1][index + 1] = ideal->ideal->r;
  for (unsigned int col = index + 2; col <= t; col++) {
    if (Tqr[col-1] != 0) {
      Mqr->coeff[index + 1][col] =
        (-(int64_t)Tqr[col-1]) + (int64_t)ideal->ideal->r;
    }
#ifndef NDEBUG
    else {
      Mqr->coeff[index + 1][col] = 0;
    }
#endif // NDEBUG
  }
}

/*
 * Compute Tqr for r an ideal of degree 1 and normalised it.
 *
 * Tqr: the Tqr matrix (Tqr is a line).
 * matrix: the MqLLL matrix.
 * t: dimension of the lattice.
 * ideal: the ideal r.
 */
void compute_Tqr_1(uint64_t * Tqr, mat_Z_srcptr matrix,
    unsigned int t, ideal_1_srcptr ideal)
{
  mpz_t tmp;
  mpz_init(tmp);
  unsigned int i = 0;
  Tqr[i] = 0;
  mpz_t invert;
  mpz_init(invert);

  //Tqr = Mq,1 - Tr * Mq,2.
  for (unsigned int j = 0; j < t; j++) {
    mpz_set(tmp, matrix->coeff[1][j + 1]);
    for (unsigned int k = 0; k < t - 1; k++) {
      mpz_submul(tmp, ideal->Tr[k], matrix->coeff[k + 2][j + 1]);
    }
    if (Tqr[i] == 0) {
      mpz_mod_ui(tmp, tmp, ideal->ideal->r);
      if (mpz_cmp_ui(tmp, 0) != 0) {
        mpz_invert_ui(tmp, tmp, ideal->ideal->r);
        mpz_set(invert, tmp);
        Tqr[j] = 1;
        i = j;
      } else {
        Tqr[j] = 0;
      }
    } else {
      mpz_mul(tmp, tmp, invert);
      mpz_mod_ui(tmp, tmp, ideal->ideal->r);
      Tqr[j] = mpz_get_ui(tmp);
    }
  }

  mpz_clear(tmp);
  mpz_clear(invert);
}

/*
 * Generate the Mqr matrix, r is an ideal of degree 1.
 *
 * Mqr: the matrix.
 * Tqr: the Tqr matrix (Tqr is a line).
 * t: dimension of the lattice.
 * ideal: the ideal r.
 */
void generate_Mqr_1(mat_int64_ptr Mqr, uint64_t * Tqr, ideal_1_srcptr ideal,
    MAYBE_UNUSED unsigned int t)
{
  ASSERT(Tqr[0] != 0);
  ASSERT(t == Mqr->NumCols);
  ASSERT(t == Mqr->NumRows);

#ifndef NDEBUG
  for (unsigned int row = 1; row <= Mqr->NumRows; row++) {
    for (unsigned int col = 1; col <= Mqr->NumCols; col++) {
      ASSERT(Mqr->coeff[row][col] == 0);
    }
  }
#endif // NDEBUG

  Mqr->coeff[1][1] = ideal->ideal->r;
  for (unsigned int col = 2; col <= Mqr->NumRows; col++) {
    if (Tqr[col] == 0) {
      Mqr->coeff[1][col] = 0;
    } else {
      Mqr->coeff[1][col] = (-Tqr[col - 1]) + ideal->ideal->r;
    }
    ASSERT(Mqr->coeff[col][1] < (int64_t)ideal->ideal->r);
  }

  for (unsigned int row = 2; row <= Mqr->NumRows; row++) {
    Mqr->coeff[row][row] = 1;
  }
}

/*
 * WARNING: Need to assert if the function does what it is supposed to do.
 * Compute Tqr for an ideal (r, h) with deg(h) > 1.
 *
 * Tqr: Tqr is a matrix in this case.
 * matrix: MqLLL.
 * t: dimension of the lattice.
 * ideal: (r, h).
 */
void compute_Tqr_u(mpz_t ** Tqr, mat_Z_srcptr matrix, unsigned int t,
    ideal_u_srcptr ideal)
{
  /* Tqr = Mq,1 - Tr * Mq,2. */
  for (int row = 0; row < ideal->ideal->h->deg; row++) {
    for (unsigned int col = 0; col < t; col++) {
      mpz_init(Tqr[row][col]);
      mpz_set(Tqr[row][col], matrix->coeff[row + 1][col + 1]);
      for (int k = 0; k < (int)t - ideal->ideal->h->deg; k++) {
        mpz_submul(Tqr[row][col], ideal->Tr[row][k],
            matrix->coeff[k + ideal->ideal->h->deg + 1][col + 1]);
      }
      mpz_mod_ui(Tqr[row][col], Tqr[row][col], ideal->ideal->r);
    }
  }
}

#ifdef ASSERT_SIEVE
/*
 * To assert that the ideal is a factor of a = matrix * c mapped in the number
 *  field defined by f.
 *
 * H: the sieving bound.
 * index: index of c in the array in which we store the norm.
 * number_element: number of element contains in the sieving region.
 * matrix: MqLLL.
 * f: the polynomial that defines the number field.
 * ideal: the ideal involved in the factorisation in ideals.
 * c: the vector corresponding to the ith value of the array in which we store
 *  the norm.
 * pos: 1 if index must increase, 0 otherwise.
 */
void assert_sieve(sieving_bound_srcptr H, uint64_t index,
    uint64_t number_element, mat_Z_srcptr matrix, mpz_poly_srcptr f,
    ideal_1_srcptr ideal, int64_vector_srcptr c, unsigned int pos)
{
  /*
   * Verify if we do not sieve the same index, because we do not sieve the power
   *  of an ideal.
   */
  if (index_old == 0) {
    ASSERT(index_old <= index);
  } else {
    if (pos == 1) {
      ASSERT(index_old < index);
    } else {
      ASSERT(index_old > index);
    }
  }
  index_old = index;

  mpz_vector_t v;
  mpz_vector_init(v, H->t);
  mpz_poly_t a;
  mpz_poly_init(a, -1);
  mpz_t res;
  mpz_init(res);

  array_index_mpz_vector(v, index, H, number_element);
  mat_Z_mul_mpz_vector_to_mpz_poly(a, matrix, v);
  norm_poly(res, a, f);
  //Verify if res = r * …
  ASSERT(mpz_congruent_ui_p(res, 0, ideal->ideal->r));

  //Verify c and v are the same.
  if (c != NULL) {
    mpz_vector_t v_tmp;
    mpz_vector_init(v_tmp, c->dim);
    int64_vector_to_mpz_vector(v_tmp, c);
    ASSERT(mpz_vector_cmp(v, v_tmp) == 0);
    mpz_vector_clear(v_tmp);
  }

  //Verify if h divides a mod r.
  if (a->deg != -1) {
    mpz_poly_t r;
    mpz_poly_init(r, -1);
    mpz_poly_t q;
    mpz_poly_init(q, -1);
    mpz_t p;
    mpz_init(p);
    mpz_set_uint64(p, ideal->ideal->r);

    ASSERT(mpz_poly_div_qr(q, r, a, ideal->ideal->h, p));
    ASSERT(r->deg == -1);
    mpz_poly_clear(r);
    mpz_poly_clear(q);
    mpz_clear(p);
  }

  mpz_clear(res);
  mpz_poly_clear(a);
  mpz_vector_clear(v);
}
#endif // ASSERT_SIEVE

#ifdef TRACE_POS
/*
 * Say all the ideal we delete in the ith position.
 *
 * index: index in array we want to follow.
 * ideal: the ideal we delete to the norm.
 * array: the array in which we store the resulting norm.
 */
void trace_pos_sieve(uint64_t index, ideal_1_srcptr ideal, array_srcptr array)
{
  if (index == TRACE_POS) {
    fprintf(file, "The ideal is: ");
    ideal_fprintf(file, ideal->ideal);
    fprintf(file, "log: %u\n", ideal->log);
    fprintf(file, "The new value of the norm is %u.\n", array->array[index]);
  }
}
#endif // TRACE_POS

/*
 * All the mode we can active during the sieving step.
 *
 * H: the sieving bound.
 * index: index of the array in which we store the resulting norm.
 * array: the array in which we store the resulting norm.
 * matrix: MqLLL.
 * f: the polynomial that defines the number field.
 * ideal: the ideal we want to delete.
 * c: the vector corresponding to indexth position in array.
 * number_c_l: number of possible c with the same ci, ci+1, …, ct. (cf line_sieve_ci)
 * nbint: say if we have already count the number_c_l or not.
 * pos: 1 if index must increase, 0 otherwise.
 */

static inline void mode_sieve(MAYBE_UNUSED sieving_bound_srcptr H,
    MAYBE_UNUSED uint64_t index, MAYBE_UNUSED array_srcptr array,
    MAYBE_UNUSED mat_Z_srcptr matrix, MAYBE_UNUSED mpz_poly_srcptr f,
    MAYBE_UNUSED ideal_1_srcptr ideal, MAYBE_UNUSED int64_vector_srcptr c,
    MAYBE_UNUSED uint64_t number_c_l, MAYBE_UNUSED unsigned int nbint,
    MAYBE_UNUSED unsigned int pos, MAYBE_UNUSED uint64_t * nb_hit)
{
#ifdef ASSERT_SIEVE
  assert_sieve(H, index, array->number_element, matrix, f, ideal, c, pos);
#endif // ASSERT_SIEVE

#ifdef NUMBER_HIT
  if (nbint) {
    * nb_hit = * nb_hit + number_c_l;
  }
#endif // NUMBER_HIT

#ifdef TRACE_POS
  trace_pos_sieve(index, ideal, array);
#endif // TRACE_POS
}

/* ----- Line sieve algorithm ----- */

/*
 * Sieve for a special-q of degree 1 and Tqr with a zero coefficient at the
 *  first place. This function sieve a c in the q lattice.
 *
 * array: in which we store the norms.
 * c: element of the q lattice.
 * ideal: an ideal with r < q.
 * ci: the possible first coordinate of c to have c in the sieving region.
 * H: the sieving bound.
 * i: index of the first non-zero coefficient in pseudo_Tqr.
 * number_c_l: number of possible c with the same ci, ci+1, …, ct.
 */
void line_sieve_ci(array_ptr array, int64_vector_ptr c, ideal_1_srcptr ideal,
    int64_t ci, sieving_bound_srcptr H, unsigned int i, uint64_t number_c_l,
    MAYBE_UNUSED mat_Z_srcptr matrix, MAYBE_UNUSED mpz_poly_srcptr f,
    MAYBE_UNUSED uint64_t * nb_hit)
{
#ifndef NDEBUG
  if (i == 0) {
    ASSERT(number_c_l == 1);
  } else {
    ASSERT(number_c_l % 2 == 0);
  }
#endif // NDEBUG

  uint64_t index = 0;

#ifdef ASSERT_SIEVE
  index_old = 0;
#endif // ASSERT_SIEVE

  if (ci < (int64_t)H->h[i]) {
    //Change the ith coordinate of c.
    int64_vector_setcoordinate(c, i, ci);

    index = array_int64_vector_index(c, H, array->number_element);
    array->array[index] = array->array[index] - ideal->log;

    mode_sieve(H, index, array, matrix, f, ideal, c, number_c_l, 1, 1, nb_hit);

    for (uint64_t k = 1; k < number_c_l; k++) {
      array->array[index + k] = array->array[index + k] - ideal->log;
    
      mode_sieve(H, index + k, array, matrix, f, ideal, NULL, number_c_l, 0, 1,
          nb_hit);
    }

    int64_t tmp = ci;
    tmp = tmp + (int64_t)ideal->ideal->r;

    while(tmp < (int64_t)H->h[i]) {

      index = index + ideal->ideal->r * number_c_l;

      array->array[index] = array->array[index] - ideal->log;
 
      mode_sieve(H, index, array, matrix, f, ideal, NULL, number_c_l, 1, 1,
          nb_hit);

      for (uint64_t k = 1; k < number_c_l; k++) {
        array->array[index + k] = array->array[index + k] - ideal->log;
 
        mode_sieve(H, index + k, array, matrix, f, ideal, NULL, number_c_l,
            1, 1, nb_hit);
      }

      tmp = tmp + (int64_t)ideal->ideal->r;
    }
  }
}

void compute_ci_1(int64_t * ci, unsigned int i, unsigned int pos,
    uint64_t * pseudo_Tqr, uint64_t ideal_r, sieving_bound_srcptr H)
{
  for (unsigned int j = i + 1; j < pos; j++) {
    * ci = * ci - ((int64_t)pseudo_Tqr[j] * (2 * (int64_t)H->h[j] - 1));
    if (* ci >= (int64_t) ideal_r) {
      while(* ci >= (int64_t)ideal_r) {
        * ci = * ci - (int64_t)ideal_r;
      }
    } else if (* ci < 0) {
      while(* ci < (int64_t)ideal_r) {
        * ci = * ci + (int64_t)ideal_r;
      }
      * ci = * ci - (int64_t)ideal_r;
    }
  }
  * ci = * ci + pseudo_Tqr[pos];
  if (* ci >= (int64_t)ideal_r) {
    * ci = * ci - (int64_t)ideal_r;
  }
  if (* ci < 0) {
    * ci = * ci + (int64_t)ideal_r;
  }
  ASSERT(* ci >= 0 && * ci < (int64_t)ideal_r);
}

/*
 *
 *
 * array: the array in which we store the resulting norms.
 * c: the 
 */
void line_sieve_1(array_ptr array, int64_vector_ptr c, uint64_t * pseudo_Tqr,
    ideal_1_srcptr ideal, sieving_bound_srcptr H, unsigned int i,
    uint64_t number_c_l, int64_t * ci, unsigned int pos,
    MAYBE_UNUSED mat_Z_srcptr matrix, MAYBE_UNUSED mpz_poly_srcptr f,
    MAYBE_UNUSED uint64_t * nb_hit)
{
  ASSERT(pos >= i);

  //Recompute ci.
  compute_ci_1(ci, i, pos, pseudo_Tqr, ideal->ideal->r, H);

  //Compute the lowest value such that Tqr*(c[0], …, c[i], …, c[t-1]) = 0 mod r.
  //lb: the minimal accessible value in the sieving region for this coordinate.
  int64_t lb = 0;
  if (i < H->t - 1) {
    lb =  -(int64_t)H->h[i];
  }
  int64_t k = (lb - * ci) / (-(int64_t) ideal->ideal->r);
  if ((lb - * ci) > 0) {
    k--;
  }
  int64_t ci_tmp = -k * (int64_t)ideal->ideal->r + * ci;
  ASSERT(ci_tmp >= lb);

#ifndef NDEBUG
  int64_t tmp_ci = ci_tmp;
  tmp_ci = tmp_ci % (int64_t) ideal->ideal->r;
  if (tmp_ci < 0) {
    tmp_ci = tmp_ci + (int64_t) ideal->ideal->r;
  }
  int64_t tmp = 0;

  for (unsigned int j = i + 1; j < H->t; j++) {
    tmp = tmp + ((int64_t)pseudo_Tqr[j] * c->c[j]);
    tmp = tmp % (int64_t) ideal->ideal->r;
    if (tmp < 0) {
      tmp = tmp + (int64_t) ideal->ideal->r;
    }
  }
  ASSERT(tmp == tmp_ci);
#endif // NDEBUG

  line_sieve_ci(array, c, ideal, ci_tmp, H, i, number_c_l, matrix, f, nb_hit);
}

/* ----- Plane sieve algorithm ----- */

/*
 * Perform the plane sieve in all the plane z = v->c[2].
 * Two different signature: if PLANE_SIEVE_STARTING_POINT is set, vs can be
 *  updated if an other vector has an x coordinate closer to zero.
 *
 * array: the array in which we store the norm.
 * vs: starting point vector.
 * e0: a vector of the Franke-Kleinjung algorithm, e0->c[0] < 0.
 * e1: a vector of the Franke-Kleinjung algorithm, e1->c[0] > 0.
 * coord_e0: deplacement in array given by e0.
 * coord_e1: deplacement in array given by e1.
 * H: sieving bound that give the sieving region.
 * r: the ideal we consider.
 * matrix: MqLLL.
 * f: the polynomial that defines the number field.
 */
#ifdef PLANE_SIEVE_STARTING_POINT
void plane_sieve_1_enum_plane(array_ptr array, int64_vector_ptr vs,
    int64_vector_srcptr e0, int64_vector_srcptr e1, uint64_t coord_e0,
    uint64_t coord_e1, sieving_bound_srcptr H, ideal_1_srcptr r, MAYBE_UNUSED
    mat_Z_srcptr matrix, MAYBE_UNUSED mpz_poly_srcptr f,
    MAYBE_UNUSED uint64_t * nb_hit)
#else
void plane_sieve_1_enum_plane(array_ptr array, int64_vector_srcptr vs,
    int64_vector_srcptr e0, int64_vector_srcptr e1, uint64_t coord_e0,
    uint64_t coord_e1, sieving_bound_srcptr H, ideal_1_srcptr r, MAYBE_UNUSED
    mat_Z_srcptr matrix, MAYBE_UNUSED mpz_poly_srcptr f,
    MAYBE_UNUSED uint64_t * nb_hit)
#endif // PLANE_SIEVE_STARTING_POINT
{
  int64_vector_t v;
  int64_vector_init(v, vs->dim);
  int64_vector_set(v, vs);

#ifdef PLANE_SIEVE_STARTING_POINT
  int64_vector_t vs_tmp;
  int64_vector_init(vs_tmp, vs->dim);
  int64_vector_set(vs_tmp, vs);
#endif // PLANE_SIEVE_STARTING_POINT

  unsigned int FK_value = 0;
  //1 if we have v was in the sieving region, 0 otherwise.
  unsigned int flag_sr = 0;
  uint64_t index_v = 0;

  //Perform Franke-Kleinjung enumeration.
  //x increases.

#ifdef ASSERT_SIEVE
  index_old = 0;
#endif // ASSERT_SIEVE
 
  while (v->c[1] < (int64_t)H->h[1]) {
    if (v->c[1] >= -(int64_t)H->h[1]) {
      if (!flag_sr) {
        ASSERT(flag_sr == 0);
        index_v = array_int64_vector_index(v, H, array->number_element);
        flag_sr = 1;
      } else {
        ASSERT(flag_sr == 1);
        if (FK_value == 0) {
          index_v = index_v + coord_e0;
        } else if (FK_value == 1) {
          index_v = index_v + coord_e1;
        } else {
          ASSERT(FK_value == 2);
          index_v = index_v + coord_e0 + coord_e1;
        }
      }
      array->array[index_v] = array->array[index_v] - r->log;

      mode_sieve(H, index_v, array, matrix, f, r, v, 1, 1, 1, nb_hit);

    }

#ifdef PLANE_SIEVE_STARTING_POINT
    if (ABS(v->c[1]) < ABS(vs_tmp->c[1])) {
      int64_vector_set(vs_tmp, v);
    }
#endif // PLANE_SIEVE_STARTING_POINT

    FK_value = enum_pos_with_FK(v, v, e0, e1, -(int64_t)H->h[0], 2 * (int64_t)
        H->h[0]);
  }

  //x decreases.

#ifdef ASSERT_SIEVE
  index_old = 0;
#endif // ASSERT_SIEVE

  flag_sr = 0;
  int64_vector_set(v, vs);
  FK_value = enum_neg_with_FK(v, v, e0, e1, -(int64_t)H->h[0] + 1, 2 *
      (int64_t)H->h[0]);
  while (v->c[1] >= -(int64_t)H->h[1]) {
    if (v->c[1] < (int64_t)H->h[1]) {
      if (!flag_sr) {
        index_v = array_int64_vector_index(v, H, array->number_element);
        flag_sr = 1;
      } else {
        if (FK_value == 0) {
          index_v = index_v - coord_e0;
        } else if (FK_value == 1) {
          index_v = index_v - coord_e1;
        } else {
          ASSERT(FK_value == 2);
          index_v = index_v - coord_e0 - coord_e1;
        }
      }
      array->array[index_v] = array->array[index_v] - r->log;

      mode_sieve(H, index_v, array, matrix, f, r, v, 1, 1, 0, nb_hit);

    }
#ifdef PLANE_SIEVE_STARTING_POINT
    if (ABS(v->c[1]) < ABS(vs_tmp->c[1])) {
      int64_vector_set(vs_tmp, v);
    }
#endif // PLANE_SIEVE_STARTING_POINT
    FK_value = enum_neg_with_FK(v, v, e0, e1, -(int64_t)H->h[0] + 1, 2 *
        (int64_t) H->h[0]);
  }
#ifdef PLANE_SIEVE_STARTING_POINT
  int64_vector_set(vs, vs_tmp);
  int64_vector_clear(vs_tmp);
#endif // PLANE_SIEVE_STARTING_POINT

  int64_vector_clear(v);
}

/*
 * Plane sieve.
 *
 * array: the array in which we store the norms.
 * r: the ideal we consider.
 * Mqr: the Mqr matrix.
 * H: the sieving bound that defines the sieving region.
 * matrix: MqLLL.
 * f: polynomial that defines the number field.
 */
void plane_sieve_1(array_ptr array, ideal_1_srcptr r,
    mat_int64_srcptr Mqr, sieving_bound_srcptr H,
    MAYBE_UNUSED mat_Z_srcptr matrix, MAYBE_UNUSED mpz_poly_srcptr f,
    MAYBE_UNUSED uint64_t * nb_hit)
{
  ASSERT(Mqr->NumRows == Mqr->NumCols);
  ASSERT(Mqr->NumRows == 3);

  //Perform the Franke-Kleinjung algorithm.
  int64_vector_t * vec = malloc(sizeof(int64_vector_t) * Mqr->NumRows);
  for (unsigned int i = 0; i < Mqr->NumCols; i++) {
    int64_vector_init(vec[i], Mqr->NumRows);
    mat_int64_extract_vector(vec[i], Mqr, i);
  }
  int64_vector_t e0;
  int64_vector_t e1;
  int64_vector_init(e0, vec[0]->dim);
  int64_vector_init(e1, vec[1]->dim);
  int boolean = reduce_qlattice(e0, e1, vec[0], vec[1], (int64_t)(2*H->h[0]));
  
  //Find some short vectors to go from z = d to z = d + 1.
  //TODO: go after boolean, no?
  list_int64_vector_t SV;
  list_int64_vector_init(SV, 3);
  SV4(SV, vec[0], vec[1], vec[2]);

  //Reduce q-lattice is not possible.
  if (boolean == 0) {
    fprintf(stderr, "# Plane sieve does not support this type of Mqr.\n");
    mat_int64_fprintf_comment(stderr, Mqr);
 
    //plane_sieve_whithout_FK(SV, vec);

    int64_vector_clear(e0);  
    int64_vector_clear(e1);
    for (unsigned int i = 0; i < Mqr->NumCols; i++) {
      int64_vector_clear(vec[i]);
    }
    free(vec);
    list_int64_vector_clear(SV);

    return;
  }

  for (unsigned int i = 0; i < Mqr->NumCols; i++) {
    int64_vector_clear(vec[i]);
  }
  free(vec);

  int64_vector_t vs;
  int64_vector_init(vs, e0->dim);
  int64_vector_set_zero(vs);

  uint64_t coord_e0 = 0, coord_e1 = 0;
  coordinate_FK_vector(&coord_e0, &coord_e1, e0, e1, H, array->number_element);

  //Enumerate the element of the sieving region.
  for (unsigned int d = 0; d < H->h[2]; d++) {
    plane_sieve_1_enum_plane(array, vs, e0, e1, coord_e0, coord_e1, H, r,
        matrix, f, nb_hit);

    //Jump in the next plane.
    plane_sieve_next_plane(vs, SV, e0, e1, H);
  }

  list_int64_vector_clear(SV);
  int64_vector_clear(vs);
  int64_vector_clear(e0);
  int64_vector_clear(e1);
}

/* ----- Space sieve algorithm ----- */

void space_sieve_1_plane(array_ptr array, MAYBE_UNUSED uint64_t * nb_hit,
    list_int64_vector_ptr list_s, int64_vector_srcptr s,
    ideal_1_srcptr r, list_int64_vector_index_srcptr list_vec_zero,
    uint64_t index_s, sieving_bound_srcptr H)
{
  ASSERT(int64_vector_equal(s, list_s->v[0]));

  int64_vector_t v_tmp;
  int64_vector_init(v_tmp, s->dim);

  for (unsigned int i = 0; i < list_vec_zero->length; i++) {
    int64_vector_add(v_tmp, s, list_vec_zero->v[i]->vec);

    uint64_t index_tmp = index_s;

    while (int64_vector_in_sieving_region(v_tmp, H)) {

#ifdef SPACE_SIEVE_CUT_EARLY
      * nb_hit = * nb_hit + 1;
#endif // SPACE_SIEVE_CUT_EARLY

      list_int64_vector_add_int64_vector(list_s, v_tmp);

      if (list_vec_zero->v[i]->vec->c[1] < 0) {
        index_tmp = index_tmp - list_vec_zero->v[i]->index;
      } else {
        index_tmp = index_tmp + list_vec_zero->v[i]->index;
      }
      array->array[index_tmp] = array->array[index_tmp] - r->log;

      int64_vector_add(v_tmp, v_tmp, list_vec_zero->v[i]->vec);
    }
    int64_vector_sub(v_tmp, s, list_vec_zero->v[i]->vec);

    index_tmp = index_s;

    while (int64_vector_in_sieving_region(v_tmp, H)) {

#ifdef SPACE_SIEVE_CUT_EARLY
      * nb_hit = * nb_hit + 1;
#endif // SPACE_SIEVE_CUT_EARLY

      list_int64_vector_add_int64_vector(list_s, v_tmp);

      if (list_vec_zero->v[i]->vec->c[1] < 0) {
        index_tmp = index_tmp + list_vec_zero->v[i]->index;
      } else {
        index_tmp = index_tmp - list_vec_zero->v[i]->index;
      }
      array->array[index_tmp] = array->array[index_tmp] - r->log;

      int64_vector_sub(v_tmp, v_tmp, list_vec_zero->v[i]->vec);
    }
  }

  int64_vector_clear(v_tmp);
}

void space_sieve_1_next_plane(array_ptr array, uint64_t * index_s,
    list_int64_vector_ptr list_s, unsigned int s_change, int64_vector_srcptr s,
    list_int64_vector_index_srcptr list_vec, unsigned int index_vec,
    ideal_1_srcptr r, sieving_bound_srcptr H)
{
  if (s->c[2] < (int64_t)H->h[2]) {
#ifdef SPACE_SIEVE_CUT_EARLY
    nb_hit++;
#endif // SPACE_SIEVE_CUT_EARLY

    list_int64_vector_delete_elements(list_s);
    list_int64_vector_add_int64_vector(list_s, s);

    if (!s_change) {
      if (list_vec->v[index_vec]->index == 0) {
        list_vec->v[index_vec]->index =
          index_vector(list_vec->v[index_vec]->vec, H,
              array->number_element);
      }
      * index_s = * index_s + list_vec->v[index_vec]->index;
    } else {
      ASSERT(s_change == 1);

      * index_s = array_int64_vector_index(s, H, array->number_element);
    }
    array->array[* index_s] = array->array[* index_s] - r->log;
  }
}

void space_sieve_1_plane_sieve(array_ptr array,
    list_int64_vector_ptr list_s,MAYBE_UNUSED uint64_t * nb_hit,
    MAYBE_UNUSED unsigned int * entropy, int64_vector_ptr s,
    uint64_t * index_s, list_int64_vector_index_ptr list_vec,
    MAYBE_UNUSED list_int64_vector_index_ptr list_vec_zero, ideal_1_srcptr r,
    mat_int64_srcptr Mqr, list_int64_vector_srcptr list_SV,
    list_int64_vector_srcptr list_FK, sieving_bound_srcptr H)
{
  int64_vector_t s_out;
  int64_vector_init(s_out, s->dim);
  int64_vector_t v_new;
  int64_vector_init(v_new, s->dim);
  plane_sieve_1_incomplete(s_out, s, Mqr, H, list_FK, list_SV);
  if (int64_vector_in_sieving_region(s_out, H)) {

#ifdef SPACE_SIEVE_CUT_EARLY
    * nb_hit = * nb_hit + 1;
#endif // SPACE_SIEVE_CUT_EARLY

    list_int64_vector_delete_elements(list_s);
    list_int64_vector_add_int64_vector(list_s, s_out);
    int64_vector_sub(v_new, s_out, s);
    uint64_t index_new = index_vector(v_new, H, array->number_element);
    list_int64_vector_index_add_int64_vector_index(list_vec, v_new,
        index_new);

#ifdef SPACE_SIEVE_ENTROPY
    //TODO: avoid duplicate.
    if (* entropy < SPACE_SIEVE_ENTROPY) {
      space_sieve_generate_new_vectors(list_vec, list_vec_zero, H,
          array->number_element);
      * entropy = * entropy + 1;

#ifdef SPACE_SIEVE_REMOVE_DUPLICATE
      list_int64_vector_index_remove_duplicate_sort(list_vec);
#endif // SPACE_SIEVE_REMOVE_DUPLICATE

    } else {
      list_int64_vector_index_sort_last(list_vec);
    }
#else
    list_int64_vector_index_sort_last(list_vec);
#endif // SPACE_SIEVE_ENTROPY

    * index_s = * index_s + index_new;
    array->array[* index_s] = array->array[* index_s] - r->log;

  }
  int64_vector_set(s, s_out);
  int64_vector_clear(s_out);
  int64_vector_clear(v_new);
}

//TODO: in many function, a v_tmp is used, try to have only one.
void space_sieve_1(array_ptr array, ideal_1_srcptr r, mat_int64_srcptr Mqr,
    sieving_bound_srcptr H)
{
  //For SPACE_SIEVE_ENTROPY
  MAYBE_UNUSED unsigned int entropy = 0;
  //For SPACE_SIEVE_CUT_EARLY
  MAYBE_UNUSED uint64_t nb_hit = 0;

#ifdef SPACE_SIEVE_CUT_EARLY
  double expected_hit = (double)(4 * H->h[0] * H->h[1] * H->h[2]) /
    (double)r->ideal->r;
#endif // SPACE_SIEVE_CUT_EARLY

  /*
   * The two list contain vector in ]-2*H0, 2*H0[x]-2*H1, 2*H1[x[0, H2[.
   * list_vec contains vector with a last non-zero coordinate.
   * list_vec_zero contains vector with a last coordinate equals to zero.
   */
  list_int64_vector_index_t list_vec;
  list_int64_vector_index_init(list_vec, 3);
  list_int64_vector_index_t list_vec_zero;
  list_int64_vector_index_init(list_vec_zero, 3);

  //List that contain the vector for the plane sieve.
  list_int64_vector_t list_FK;
  list_int64_vector_init(list_FK, 3);
  list_int64_vector_t list_SV;
  list_int64_vector_init(list_SV, 3);

  //List that contain the current set of point with the same z coordinate.
  list_int64_vector_t list_s;
  list_int64_vector_init(list_s, 3);

  unsigned int vector_1 = space_sieve_1_init(list_vec, list_vec_zero, r, Mqr,
      H, array->number_element);

  //0 if plane sieve is already done, 1 otherwise.
  int plane_sieve = 0;

  //s = 0, starting point.
  int64_vector_t s;
  int64_vector_init(s, Mqr->NumRows);
  int64_vector_set_zero(s);

#ifdef SPACE_SIEVE_CUT_EARLY
  nb_hit++;
#endif // SPACE_SIEVE_CUT_EARLY

  //Compute the index of s in array.
  uint64_t index_s = array_int64_vector_index(s, H, array->number_element);
  //TODO: not necessary to do that.
  array->array[index_s] = array->array[index_s] - r->log;

  //s is in the list of current point.
  list_int64_vector_add_int64_vector(list_s, s);

  while (s->c[2] < (int64_t)H->h[2]) {

    /*
     * if there are vectors in list_vec_zero, use it to find other point with
     * the same z coordinate as s.
     */
    space_sieve_1_plane(array, &nb_hit, list_s, s, r, list_vec_zero,
        index_s, H);

    //index_vec: index of the vector we used to go to the other plane in list_s
    unsigned int index_vec = 0;
    //s has changed, compare the s in space_sieve_1_plane.
    unsigned int s_change = 0;

    //s_tmp: TODO
    int64_vector_t s_tmp;
    int64_vector_init(s_tmp, s->dim);

    //Find the next element, with the smallest reachable z.
    //hit is set to 1 if an element is reached.
    unsigned int hit = space_sieve_1_next_plane_seek(s_tmp, &index_vec,
        &s_change, list_s, list_vec, H, s);

    if (hit) {
      int64_vector_set(s, s_tmp);
      space_sieve_1_next_plane(array, &index_s, list_s, s_change, s, list_vec,
          index_vec, r, H);
    }
#ifdef SPACE_SIEVE_CUT_EARLY
    double err_rel = ((double)expected_hit - (double)nb_hit) / (double)nb_hit;
    if (!hit && err_rel >= SPACE_SIEVE_CUT_EARLY && 0 <= err_rel) {
      //}
#else
    if (!hit) {
#endif // SPACE_SIEVE_CUT_EARLY
      ASSERT(hit == 0);

      //Need to do plane sieve to 
      if (!plane_sieve) {
        ASSERT(plane_sieve == 0);
        int boolean = space_sieve_1_plane_sieve_init(list_SV, list_FK,
            list_vec, list_vec_zero, r, H, Mqr, vector_1,
            array->number_element);

        if (!boolean) {
          ASSERT(boolean == 0);

          fprintf(stderr,
              "# Plane sieve (called by space sieve) does not support this type of Mqr.\n");
          mat_int64_fprintf_comment(stderr, Mqr);

          int64_vector_clear(s);
          list_int64_vector_clear(list_s);
          list_int64_vector_index_clear(list_vec);
          list_int64_vector_index_clear(list_vec_zero);
          list_int64_vector_clear(list_FK);
          list_int64_vector_clear(list_SV);

          return;
        }
        plane_sieve = 1;
      }

      space_sieve_1_plane_sieve(array, list_s, &nb_hit, &entropy, s, &index_s,
          list_vec, list_vec_zero, r, Mqr, list_SV, list_FK, H);
    }
#ifdef SPACE_SIEVE_CUT_EARLY
    else if (!hit) {
      ASSERT(err_rel <= SPACE_SIEVE_CUT_EARLY);

      int64_vector_clear(s_tmp);
      break;
    }
#endif // SPACE_SIEVE_CUT_EARLY
    int64_vector_clear(s_tmp);
  }

  int64_vector_clear(s);
  list_int64_vector_clear(list_s);
  list_int64_vector_index_clear(list_vec);
  list_int64_vector_index_clear(list_vec_zero);
  list_int64_vector_clear(list_FK);
  list_int64_vector_clear(list_SV);
}

/* ----- Enumeration algorithm ----- */

/*
 * Compute sum(m[j][i] * x^j, j > i).
 *
 * x: the coefficients of a linear combination of the vector of a basis of the
 *  lattice we enumerate.
 * m: the \mu{i, j} matrix given by Gram-Schmidt orthogonalisation.
 * i: index.
 */
double sum_mi(int64_vector_srcptr x, mat_double_srcptr m, unsigned int i)
{
  double sum = 0;
  for (unsigned int j = i + 1; j < x->dim; j++) {
    sum = sum + (double)x->c[j] * m->coeff[j + 1][i + 1];
  }
  return sum;
}

/*
 * Compute sum(l_j, j >= i.
 *
 * l: 
 * i: index.
 */
double sum_li(double_vector_srcptr l, unsigned int i)
{
  double sum = 0;
  for (unsigned int j = i; j < l->dim; j++) {
    sum = sum + l->c[j];
  }
  return sum;
}

/*
 * Construct v from x (the coefficients of the linear combination of the vector
 *  of a basis of the lattice) and list (a basis of the lattice).
 *
 * v: the vector we build.
 * x: the coefficients.
 * list: a basis of the lattice.
 */
void construct_v(int64_vector_ptr v, int64_vector_srcptr x,
    list_int64_vector_srcptr list)
{
  ASSERT(x->dim == list->length);
  ASSERT(v->dim == list->v[0]->dim);

  for (unsigned int i = 0; i < v->dim; i++) {
    for (unsigned int j = 0; j < x->dim; j++) {
      v->c[i] = v->c[i] + x->c[j] * list->v[j]->c[i];
    }
  }
}

/*
 * An enumeration algorithm of the element of the lattice generated by Mqr,
 *  which are in a sphere (see page 164 of "Algorithms for the Shortest and Closest
 *  Lattice Vector Problems" by Guillaume Hanrot, Xavier Pujol, and Damien Stehlé,
 *  IWCC 2011).
 *
 * array: the array in which we store the norms.
 * ideal: the ideal we consider.
 * Mqr: the Mqr matrix.
 * H: the sieving bound that generates a sieving region.
 * f: the polynomial that defines the number field.
 * matrix: MqLLL.
 */
void enum_lattice(array_ptr array, ideal_1_srcptr ideal,
    mat_int64_srcptr Mqr, sieving_bound_srcptr H,
    MAYBE_UNUSED mpz_poly_srcptr f, MAYBE_UNUSED mat_Z_srcptr matrix,
    MAYBE_UNUSED uint64_t * nb_hit)
{
  ASSERT(Mqr->NumRows == Mqr->NumCols);
  ASSERT(Mqr->NumRows == 3);
  ASSERT(H->t == Mqr->NumRows);

  //Original basis of the lattice.
  list_int64_vector_t b_root;
  list_int64_vector_init(b_root, 3);
  list_int64_vector_extract_mat_int64(b_root, Mqr);

  /* To compute and store the result of Gram-Schmidt. */
  list_double_vector_t list_e;
  list_double_vector_init(list_e);
  list_double_vector_extract_mat_int64(list_e, Mqr);

  //Matrix with the coefficient mu_{i, j}.
  mat_double_t M;
  mat_double_init(M, Mqr->NumRows, Mqr->NumCols);
  mat_double_set_zero(M);

  //Gram Schmidt orthogonalisation.
  list_double_vector_t list;
  list_double_vector_init(list);
  double_vector_gram_schmidt(list, M, list_e);
  
  /* Compute the square of the L2 norm for all the Gram-Schmidt vectors. */
  double_vector_t b;
  double_vector_init(b, list->length);
  for (unsigned int i = 0; i < b->dim; i++) {
    b->c[i] = double_vector_norml2sqr(list->v[i]);
  }

  /* Center of the cuboid. */
  double_vector_t t;
  double_vector_init(t, Mqr->NumRows);
  double_vector_set_zero(t);
  t->c[t->dim - 1] = round((double) H->h[t->dim - 1] / 2);

  //This A is equal to the A^2 in the paper we cite.
  double A = 0;
  for (unsigned int i = 0; i < t->dim - 1; i++) {
    A = A + (double)(H->h[0] * H->h[0]);
  }
  A = A + (t->c[t->dim - 1] * t->c[t->dim - 1]);

  //Verify if the matrix M has the good properties.
#ifdef NDEBUG
  for (unsigned int j = 0; j < list->v[0]->dim; j++) {
    if (j == 0) {
      ASSERT(0 != list->v[0]->c[j]);
    } else {
      ASSERT(0 == list->v[0]->c[j]);
    }
  }
  for(unsigned int i = 1; i < list->length; i++) {
    for (unsigned int j = 0; j < list->v[i]->dim; j++) {
      if (j == i) {
        ASSERT(1 == list->v[i]->c[j]);
      } else {
        ASSERT(0 == list->v[i]->c[j]);
      }
    }
  }
#endif

  //Coefficient of t in the Gram-Schmidt basis.
  double_vector_t ti;
  double_vector_init(ti, t->dim);
  double_vector_set_zero(ti);
  ti->c[ti->dim - 1] = t->c[ti->dim - 1];

  //The vector in the classical basis.
  int64_vector_t x;
  int64_vector_init(x, list->length);
  int64_vector_set_zero(x);
  x->c[x->dim - 1] = (int64_t) ceil(ti->c[x->dim - 1] -
      sqrt(A) / sqrt(b->c[x->dim - 1]));
  unsigned int i = list->length - 1;

  double_vector_t l;
  double_vector_init(l, x->dim);
  double_vector_set_zero(l);

  //TODO: is it true else if? is it true sqrt(A) in the last condition?
  while (i < list->length) {
    double tmp = (double)x->c[i] - ti->c[i] + sum_mi(x, M, i);
    l->c[i] = (tmp * tmp) * b->c[i];
    
    if (i == 0 && sum_li(l, 0) <= A) {
      int64_vector_t v_h;
      int64_vector_init(v_h, x->dim);
      int64_vector_set_zero(v_h);
      //x * orig_base.
      construct_v(v_h, x, b_root);
      if (int64_vector_in_sieving_region(v_h, H)) {
        uint64_t index =
          array_int64_vector_index(v_h, H, array->number_element);
        array->array[index] = array->array[index] - ideal->log;

#ifdef ASSERT_SIEVE
        index_old = 0;
#endif // ASSERT_SIEVE
        mode_sieve(H, index, array, matrix, f, ideal, v_h, 1, 1, 1, nb_hit);
      }
      int64_vector_clear(v_h);
      x->c[0] = x->c[0] + 1;
    } else if (i != 0 && sum_li(l, i) <= A) {
      i = i - 1;
      x->c[i] = (int64_t)ceil(ti->c[i] -
          sum_mi(x, M, i) - sqrt((A - sum_li(l, i + 1)) / b->c[i]));
    } else if (sum_li(l, i)> sqrt(A)) {
      i = i + 1;
      if (i < list->length) {
        x->c[i] = x->c[i] + 1;
      }
    }
  }

  double_vector_clear(l);
  double_vector_clear(b);
  int64_vector_clear(x);
  double_vector_clear(ti);
  mat_double_clear(M);
  double_vector_clear(t);
  list_double_vector_clear(list);
  list_double_vector_clear(list_e);
  list_int64_vector_clear(b_root);
}

#ifdef SIEVE_U
void sieve_u(array_ptr array, mpz_t ** Tqr, ideal_u_srcptr ideal,
    mpz_vector_srcptr c, sieving_bound_srcptr H)
{
  //Not optimal
  int nul = 1;
  mpz_t * tmp = (mpz_t * )
    malloc(sizeof(mpz_t) * ideal->ideal->h->deg);
  for (int row = 0; row < ideal->ideal->h->deg;
       row++) {
    mpz_init(tmp[row]);
    for (unsigned int col = 0; col < c->dim; col++) {
      mpz_addmul(tmp[row], Tqr[row][col], c->c[col]);
    }
    mpz_mod_ui(tmp[row], tmp[row], ideal->ideal->r);
    if (mpz_cmp_ui(tmp[row], 0) != 0) {
      nul = 0;
    }
  }

  if (nul == 1) {
    uint64_t index = 0;
    index = array_mpz_vector_index(c, H, array->number_element);
    array->array[index] = array->array[index] -
      ideal->log;

#ifdef TRACE_POS
    if (index == TRACE_POS) {
      fprintf(file, "The ideal is: ");
      ideal_u_fprintf(file, ideal, H->t);
      fprintf(file, "The new value of the norm is %u.\n", array->array[index]);
    }
#endif // TRACE_POS
  }

  for (int row = 0; row < ideal->ideal->h->deg;
       row++) {
    mpz_clear(tmp[row]);
  }
  free(tmp);
}
#endif // SIEVE_U

/*
 *
 *
 * array: the array in which we store the resulting norms.
 * matrix: MqLLL.
 * fb: factor base of this side.
 * H: sieving bound.
 * f: the polynomial that defines the number field.
 */
void special_q_sieve(array_ptr array, mat_Z_srcptr matrix,
    factor_base_srcptr fb, sieving_bound_srcptr H,
    MAYBE_UNUSED mpz_poly_srcptr f)
{
#ifdef TIME_SIEVES
  double time_line_sieve = 0;
  uint64_t ideal_line_sieve = 0;
  double time_plane_sieve = 0;
  uint64_t ideal_plane_sieve = 0;
  double time_space_sieve = 0;
  uint64_t ideal_space_sieve = 0;
#endif // TIME_SIEVES

  MAYBE_UNUSED uint64_t number_hit = 0;

  ideal_1_t r;
  ideal_1_init(r);

  uint64_t * Tqr = (uint64_t *) malloc(sizeof(uint64_t) * (H->t));

#ifdef TIME_SIEVES
  time_line_sieve = seconds();
#endif // TIME_SIEVES

  /* --- Line sieve --- */

  uint64_t i = 0;
  uint64_t line_sieve_stop = 2 * (uint64_t) H->h[0];

  while (i < fb->number_element_1 &&
      fb->factor_base_1[i]->ideal->r < line_sieve_stop) {
    ideal_1_set(r, fb->factor_base_1[i], H->t);

#ifdef ASSERT_SIEVE
    printf("# ");
    ideal_fprintf(stdout, r->ideal);
    /*fprintf(stdout, "# Tqr = [");*/
    /*for (unsigned int i = 0; i < H->t - 1; i++) {*/
    /*fprintf(stdout, "%" PRIu64 ", ", Tqr[i]);*/
    /*}*/
    /*fprintf(stdout, "%" PRIu64 "]\n", Tqr[H->t - 1]);*/
    printf("# Line sieve.\n");
#endif // ASSERT_SIEVE

    //Compute the true Tqr
    compute_Tqr_1(Tqr, matrix, H->t, r);

    uint64_t * pseudo_Tqr = (uint64_t *) malloc(sizeof(uint64_t) * (H->t));
    //Compute a usefull pseudo_Tqr for line_sieve.
    compute_pseudo_Tqr_1(pseudo_Tqr, Tqr, H->t, r);

    //Initialise c = (-H[0], -H[1], …, 0).
    int64_vector_t c;
    int64_vector_init(c, H->t);
    for (unsigned int j = 0; j < H->t - 1; j++) {
      int64_vector_setcoordinate(c, j, -(int64_t)H->h[j]);
    }
    int64_vector_setcoordinate(c, H->t - 1, 0);

    unsigned int index = 0;
    while (pseudo_Tqr[index] == 0) {
      index++;
    }

#ifndef NDEBUG
    if (index == 0) {
      ASSERT(pseudo_Tqr[0] != 0);
    }
#endif // NDEBUG

    if (index + 1 != c->dim) {

      int64_vector_setcoordinate(c, index + 1, c->c[index + 1] - 1);

      //Compute ci = (-Tqr[index])^(-1) * (Tqr[index + 1]*c[1] + …) mod r.
      //TODO: modify that.
      int64_t ci = 0;
      for (unsigned int j = index + 1; j < H->t; j++) {
        ci = ci + (int64_t)pseudo_Tqr[j] * c->c[j];
        if (ci >= (int64_t) r->ideal->r) {
          while(ci >= (int64_t)r->ideal->r) {
            ci = ci - (int64_t)r->ideal->r;
          }
        } else if (ci < 0) {
          while(ci < (int64_t)r->ideal->r) {
            ci = ci + (int64_t)r->ideal->r;
          }
          ci = ci - (int64_t)r->ideal->r;
        }
      }
      ASSERT(ci >= 0 && ci < (int64_t)r->ideal->r);
      uint64_t number_c = array->number_element;
      uint64_t number_c_l = 1;
      //Number of c with the same c[index + 1], …, c[t-1].
      for (unsigned int i = 0; i < index + 1; i++) {
        number_c = number_c / (2 * H->h[i]);
        number_c_l = number_c_l * (2 * H->h[i]);
      }
      number_c_l = number_c_l / (2 * H->h[index]);

      for (uint64_t j = 0; j < number_c; j++) {
        unsigned int pos = int64_vector_add_one_i(c, index + 1, H);
        line_sieve_1(array, c, pseudo_Tqr, r, H, index, number_c_l, &ci,
            pos, matrix, f, &number_hit);
      }
    } else {
#ifndef NDEBUG
      for (unsigned int i = 0; i < index; i++) {
        ASSERT(pseudo_Tqr[i] == 0);
      }
      ASSERT(pseudo_Tqr[index] != 0);
#endif // NDEBUG

      uint64_t number_c_l = 1;
      for (unsigned int i = 0; i < index; i++) {
        number_c_l = number_c_l * (2 * H->h[i]);
      }
      line_sieve_ci(array, c, r, 0, H, index, number_c_l,  matrix, f,
          &number_hit);
    }

#ifdef NUMBER_HIT
    printf("# Number of hits: %" PRIu64 " for r: %" PRIu64 ", h: ",
        number_hit, r->ideal->r);
    mpz_poly_fprintf(stdout, r->ideal->h);
    printf("# Estimated number of hits: %u.\n",
        (unsigned int) nearbyint((double) array->number_element /
          (double) r->ideal->r));
    number_hit = 0;
#endif // NUMBER_HIT

    free(pseudo_Tqr);
    int64_vector_clear(c);

    i++;
  }

#ifdef TIME_SIEVES
  time_line_sieve = seconds() - time_line_sieve;
  ideal_line_sieve = i;
#endif // TIME_SIEVES

#ifdef TIME_SIEVES
  time_plane_sieve = seconds();
#endif // TIME_SIEVES

  /* --- Plane sieve --- */

  mat_int64_t Mqr;
  mat_int64_init(Mqr, H->t, H->t);

  uint64_t plane_sieve_stop = 4 * (int64_t)(H->h[0] * H->h[1]);
  while (i < fb->number_element_1 &&
      fb->factor_base_1[i]->ideal->r < plane_sieve_stop) {
    ideal_1_set(r, fb->factor_base_1[i], H->t);

#ifdef ASSERT_SIEVE
    printf("# ");
    ideal_fprintf(stdout, r->ideal);
    /*fprintf(stdout, "# Tqr = [");*/
    /*for (unsigned int i = 0; i < H->t - 1; i++) {*/
    /*fprintf(stdout, "%" PRIu64 ", ", Tqr[i]);*/
    /*}*/
    /*fprintf(stdout, "%" PRIu64 "]\n", Tqr[H->t - 1]);*/
    printf("# Plane sieve.\n");
#endif // ASSERT_SIEVE

    //Compute the true Tqr
    compute_Tqr_1(Tqr, matrix, H->t, r);

#ifdef TEST_MQR
    printf("# Tqr = [");
    for (unsigned int i = 0; i < H->t - 1; i++) {
      printf("%" PRIu64 ", ", Tqr[i]);
    }
    printf("%" PRIu64 "]\n# Mqr =\n", Tqr[H->t - 1]);
    mat_int64_t Mqr_test;
    mat_int64_init(Mqr_test, H->t, H->t);
    compute_Mqr_1(Mqr_test, Tqr, H->t, r);
    mat_int64_fprintf_comment(stdout, Mqr_test);
    mat_int64_clear(Mqr_test);
#endif // TEST_MQR

    ASSERT(H->t == 3);

    compute_Mqr_1(Mqr, Tqr, H->t, r);

    if (Mqr->coeff[1][1] != 1) {
      plane_sieve_1(array, r, Mqr, H, matrix, f, &number_hit);
    } else {
      fprintf(stderr, "# Tqr = [");
      for (unsigned int i = 0; i < H->t - 1; i++) {
        fprintf(stderr, "%" PRIu64 ", ", Tqr[i]);
      }
      fprintf(stderr, "%" PRIu64 "]\n", Tqr[H->t - 1]);
      fprintf(stderr, "# Plane sieve does not support this type of Mqr.\n");
      mat_int64_fprintf_comment(stderr, Mqr);
    }

#ifdef NUMBER_HIT
    printf("# Number of hits: %" PRIu64 " for r: %" PRIu64 ", h: ",
        number_hit, r->ideal->r);
    mpz_poly_fprintf(stdout, r->ideal->h);
    printf("# Estimated number of hits: %u.\n",
        (unsigned int) nearbyint((double) array->number_element /
          (double) r->ideal->r));
    number_hit = 0;
#endif // NUMBER_HIT

    i++;
  }

#ifdef TIME_SIEVES
  time_plane_sieve = seconds() - time_plane_sieve;
  ideal_plane_sieve = i - ideal_line_sieve;
#endif // TIME_SIEVES

  /* --- Space sieve --- */

#ifdef TIME_SIEVES
  time_space_sieve = seconds();
#endif // TIME_SIEVES

  while (i < fb->number_element_1) {
    ideal_1_set(r, fb->factor_base_1[i], H->t);

#ifdef ASSERT_SIEVE
    printf("# ");
    ideal_fprintf(stdout, r->ideal);
    /*fprintf(stdout, "# Tqr = [");*/
    /*for (unsigned int i = 0; i < H->t - 1; i++) {*/
    /*fprintf(stdout, "%" PRIu64 ", ", Tqr[i]);*/
    /*}*/
    /*fprintf(stdout, "%" PRIu64 "]\n", Tqr[H->t - 1]);*/
    printf("# Space sieve.\n");
#endif // ASSERT_SIEVE

    //Compute the true Tqr
    compute_Tqr_1(Tqr, matrix, H->t, r);

#ifdef TEST_MQR
    printf("# Tqr = [");
    for (unsigned int i = 0; i < H->t - 1; i++) {
      printf("%" PRIu64 ", ", Tqr[i]);
    }
    printf("%" PRIu64 "]\n# Mqr =\n", Tqr[H->t - 1]);
    mat_int64_t Mqr_test;
    mat_int64_init(Mqr_test, H->t, H->t);
    compute_Mqr_1(Mqr_test, Tqr, H->t, r);
    mat_int64_fprintf_comment(stdout, Mqr_test);
    mat_int64_clear(Mqr_test);
#endif // TEST_MQR

    ASSERT(H->t == 3);

    compute_Mqr_1(Mqr, Tqr, H->t, r);

#ifdef ENUM_LATTICE
    enum_lattice(array, r, Mqr, H, f, matrix);
#else // ENUM_LATTICE
    if (Mqr->coeff[1][1] != 0) {
#ifdef SPACE_SIEVE
      space_sieve_1(array, r, Mqr, H);
#else
      plane_sieve_1(array, r, Mqr, H, matrix, f);
#endif // SPACE_SIEVE
    } else {
      fprintf(stderr, "# Tqr = [");
      for (unsigned int i = 0; i < H->t - 1; i++) {
        fprintf(stderr, "%" PRIu64 ", ", Tqr[i]);
      }
      fprintf(stderr, "%" PRIu64 "]\n", Tqr[H->t - 1]);
      fprintf(stderr, "# Space sieve does not support this type of Mqr.\n");
      mat_int64_fprintf_comment(stderr, Mqr);
    }
#endif // ENUM_LATTICE


#ifdef NUMBER_HIT
    printf("# Number of hits: %" PRIu64 " for r: %" PRIu64 ", h: ",
        number_hit, r->ideal->r);
    mpz_poly_fprintf(stdout, r->ideal->h);
    printf("# Estimated number of hits: %u.\n",
        (unsigned int) nearbyint((double) array->number_element /
          (double) r->ideal->r));
    number_hit = 0;
#endif // NUMBER_HIT

    i++;
  }

#ifdef TIME_SIEVES
  time_space_sieve = seconds() - time_space_sieve;
  ideal_space_sieve = i - ideal_line_sieve - ideal_space_sieve;
#endif // TIME_SIEVES

  mat_int64_clear(Mqr);
  ideal_1_clear(r, H->t);
  free(Tqr);

#ifdef TIME_SIEVES
  printf("# Perform line sieve: %fs for %" PRIu64 " ideals, %fs per ideal.\n",
      time_line_sieve, ideal_line_sieve,
      time_line_sieve / (double)ideal_line_sieve);
  double time_per_ideal = 0.0;
  if (ideal_plane_sieve != 0) {
    time_per_ideal = time_plane_sieve / (double)ideal_plane_sieve;
  } else {
    time_per_ideal = 0.0;
  }
  printf("# Perform plane sieve: %fs for %" PRIu64 " ideals, %fs per ideal.\n",
      time_plane_sieve, ideal_plane_sieve, time_per_ideal);
  if (ideal_space_sieve != 0) {
    time_per_ideal = time_space_sieve / (double)ideal_space_sieve;
  } else {
    time_per_ideal = 0.0;
  }
  printf("# Perform space sieve: %fs for %" PRIu64 " ideals, %fs per ideal.\n",
      time_space_sieve, ideal_space_sieve, time_per_ideal);
#endif // TIME_SIEVES
}

#if 0
#ifdef SIEVE_TQR
    else {
      unsigned int index = 1;
      while (pseudo_Tqr[index] == 0 && index < H->t) {
        index++;
      }

      if (index == H->t - 1) {
        /* uint64_t number_c_u = array->number_element; */
        /* uint64_t number_c_l = 1; */
        /* for (uint64_t j = 0; j < index; j++) { */
        /*   number_c_u = number_c_u / (2 * H->h[j] + 1); */
        /*   number_c_l = number_c_l * (2 * H->h[j] + 1); */
        /* } */

        /* line_sieve(array, c, r, 0, H, index, number_c_l, */
        /*          matrix, f); */
      } else {
        int64_t ci = 0;
        if (index + 1 < H->t - 1) {
          int64_vector_setcoordinate(c, index + 1, c->c[index + 1] - 1);
        } else if (index + 1 == H->t - 1) {
          int64_vector_setcoordinate(c, index + 1, -1);
        }

        if (index < H->t - 1) {
          for (unsigned int j = index + 1; j < H->t; j++) {
            ci = ci + (int64_t)pseudo_Tqr[j] * c->c[j];
            if (ci >= (int64_t)r->ideal->r || ci < 0) {
              ci = ci % (int64_t)r->ideal->r;
            }
          }
          if (ci < 0) {
            ci = ci + (int64_t)r->ideal->r;
          }
          ASSERT(ci >= 0);
        }

        uint64_t number_c_u = array->number_element;
        uint64_t number_c_l = 1;
        for (uint64_t j = 0; j < index; j++) {
          number_c_u = number_c_u / (2 * H->h[j]);
          number_c_l = number_c_l * (2 * H->h[j]);
        }
        number_c_u = number_c_u / (2 * H->h[index]);

        unsigned int pos = 0;
        pos = int64_vector_add_one_i(c, index + 1, H);
        line_sieve_1(array, c, pseudo_Tqr, r, H, index,
                number_c_l, &ci, pos, matrix, f);
        for (uint64_t j = 1; j < number_c_u; j++) {
          pos = int64_vector_add_one_i(c, index + 1, H);
          line_sieve_1(array, c, pseudo_Tqr, r, H, index,
                  number_c_l, &ci, pos, matrix, f);
        }
      }
    }
#endif // SIEVE_TQR

#ifdef SIEVE_U
  for (uint64_t i = 0; i < fb->number_element_u; i++) {
    mpz_t ** Tqr = (mpz_t **)
      malloc(sizeof(mpz_t *) * (fb->factor_base_u[i]->ideal->h->deg));
    for (int j = 0; j < fb->factor_base_u[i]->ideal->h->deg; j++) {
      Tqr[j] = (mpz_t *) malloc(sizeof(mpz_t) * (H->t));
    }

    compute_Tqr_u(Tqr, matrix, H->t, fb->factor_base_u[i]);

    mpz_vector_t c;
    mpz_vector_init(c, H->t);
    for (unsigned int j = 0; j < H->t - 1; j++) {
      mpz_vector_setcoordinate_si(c, j, -H->h[j]);
    }
    mpz_vector_setcoordinate_si(c, H->t - 1, 0);

    sieve_u(array, Tqr, fb->factor_base_u[i], c, H);

    for (uint64_t j = 1; j < array->number_element; j++) {
      mpz_vector_add_one(c, H);
      sieve_u(array, Tqr, fb->factor_base_u[i], c, H);
    }
    mpz_vector_clear(c);
    for (unsigned int col = 0; col < H->t; col++) {
      for (int row = 0; row < fb->factor_base_u[i]->ideal->h->deg;
           row++) {
        mpz_clear(Tqr[row][col]);
      }
    }
    for (int j = 0; j < fb->factor_base_u[i]->ideal->h->deg; j++) {
      free(Tqr[j]);
    }
    free(Tqr);
  }
#endif // SIEVE_U
#endif // Draft for special-q_sieve

#ifdef MEAN_NORM_BOUND_SIEVE
void find_index(uint64_array_ptr indexes, array_srcptr array,
    unsigned char thresh, double * norm_bound_sieve, MAYBE_UNUSED f)
#else
void find_index(uint64_array_ptr indexes, array_srcptr array,
    unsigned char thresh)
#endif // MEAN_NORM_BOUND_SIEVE
{
  uint64_t ind = 0;
  for (uint64_t i = 0; i < array->number_element; i++) {

#ifdef MEAN_NORM_BOUND_SIEVE
    * norm_bound_sieve = * norm_bound_sieve + (double)array->array[i];
#endif // MEAN_NORM_BOUND_SIEVE

    if (array->array[i] <= thresh) {
      ASSERT(ind < indexes->length);
      indexes->array[ind] = i;
      ind++;
    }
  }
  uint64_array_realloc(indexes, ind);
}

/* ----- Cofactorisation ----- */

/*
 * Print a relation.
 *
 * factor: factorisation of a if the norm is smooth is the corresponding number
 *  field.
 * I:
 * L:
 * a: the polynomial in the original lattice.
 * t: dimension of the lattice.
 * V: number of number fields.
 * size:
 * assert_facto: 1 if the factorisation is correct, 0 otherwise.
 */
void printf_relation(factor_t * factor, unsigned int * I, unsigned int * L,
    mpz_poly_srcptr a, unsigned int t, unsigned int V, unsigned int size,
    MAYBE_UNUSED unsigned int * assert_facto)
{
  //Print a.
  printf("# ");
  mpz_poly_fprintf(stdout, a);

  unsigned int index = 0;
  //Print if the factorisation is good.
#ifdef ASSERT_FACTO
  printf("# ");
  for (unsigned int i = 0; i < V - 1; i++) {
    if (index < size) {
      if (i == L[index]) {
        if (I[index]) {
          printf("%u:", assert_facto[index]);
        } else {
          printf(":");
        }
        index++;
      } else {
        printf(":");
      }
    } else {
      printf(":");
    }
  }

  if (index < size) {
    if (V - 1 == L[index]) {
      if (I[index]) {
        printf("%u", assert_facto[index]);
      }
      index++;
    }
  }

  printf("\n");
#endif // ASSERT_FACTO

  //Print the coefficient of a.
  for (int i = 0; i < a->deg; i++) {
    gmp_printf("%Zd,", a->coeff[i]);
  }
  if ((int)t - 1 == a->deg) {
    gmp_printf("%Zd:", a->coeff[a->deg]);
  } else {
    gmp_printf("%Zd,", a->coeff[a->deg]);
    for (int i = a->deg + 1; i < (int)t - 1; i++) {
      printf("0,");
    }
    printf("0:");
  }

  //Print the factorisation in the different number field.
  index = 0;
  for (unsigned int i = 0; i < V - 1; i++) {
    if (index < size) {
      if (i == L[index]) {
        if (I[index]) {
          for (unsigned int j = 0; j < factor[index]->number - 1; j++) {
#ifdef PRINT_DECIMAL
            gmp_printf("%Zd,", factor[index]->factorization[j]);
#else // PRINT_DECIMAL
            printf("%s,",
                mpz_get_str(NULL, 16, factor[index]->factorization[j]));
#endif // PRINT_DECIMAL
          }
#ifdef PRINT_DECIMAL
          gmp_printf("%Zd:", factor[index]->factorization[
                       factor[index]->number - 1]);
#else // PRINT_DECIMAL
          printf("%s:", mpz_get_str(NULL, 16,
                factor[index]->factorization[factor[index]->number - 1]));
#endif // PRINT_DECIMAL
        } else {
          printf(":");
        }
        index++;
      } else {
        printf(":");
      }
    } else {
      printf(":");
    }
  }

  if (index < size) {
    if (V - 1 == L[index]) {
      if (I[index]) {
        for (unsigned int j = 0; j < factor[index]->number - 1; j++) {
#ifdef PRINT_DECIMAL
          gmp_printf("%Zd,", factor[index]->factorization[j]);
#else // PRINT_DECIMAL
          printf("%s,",
                mpz_get_str(NULL, 16, factor[index]->factorization[j]));
#endif // PRINT_DECIMAL
        }
#ifdef PRINT_DECIMAL
        printf("%s", mpz_get_str(NULL, 16,
              factor[index]->factorization[factor[index]->number - 1]));
#else // PRINT_DECIMAL
        gmp_printf("%Zd", factor[index]->factorization[
                     factor[index]->number - 1]);
#endif // PRINT_DECIMAL
      }
      index++;
    }
  }
  printf("\n");
}


typedef struct {
  unsigned long lpb;            // large prime bound = 2^lpb
  unsigned long fbb;            // fbb (the real bound, not its log)
  double BB;                    // square of fbb
  double BBB;                   // cube of fbb
  facul_method_t * methods;     // list of ECMs (null-terminated)
} facul_aux_data;


// FIXME: This has been duplicated from facul.cpp
// (September 2015).
// Maybe merge again, at some point.
// It returns -1 if the factor is not smooth, otherwise the number of
// factors.
// Remark: FACUL_NOT_SMOOTH is just -1.
static int
facul_aux (mpz_t *factors, const struct modset_t m,
    const facul_aux_data *data, int method_start)
{
  int found = 0;
  facul_method_t* methods = data->methods;
  if (methods == NULL)
    return found;

  int i = 0;
  for (i = method_start; methods[i].method != 0; i++)
  {
    struct modset_t fm, cfm;
    int res_fac = 0;

    switch (m.arith) {
      case CHOOSE_UL:
        res_fac = facul_doit_onefm_ul(factors, m.m_ul, 
            methods[i], &fm, &cfm,
            data->lpb, data->BB, data->BBB);
        break; 
      case CHOOSE_15UL:
        res_fac = facul_doit_onefm_15ul(factors, m.m_15ul,
            methods[i], &fm, &cfm,
            data->lpb, data->BB, data->BBB);
        break; 
      case CHOOSE_2UL2:
        res_fac = facul_doit_onefm_2ul2 (factors, m.m_2ul2,
            methods[i], &fm, &cfm,
            data->lpb, data->BB, data->BBB);
        break; 
      case CHOOSE_MPZ:
        res_fac = facul_doit_onefm_mpz (factors, m.m_mpz,
            methods[i], &fm, &cfm,
            data->lpb, data->BB, data->BBB);
        break; 
      default: abort();
    }
    // check our result
    // res_fac contains the number of factors found
    if (res_fac == -1)
    {
      /*
         The cofactor m is not smooth. So, one stops the
         cofactorization.
         */
      found = FACUL_NOT_SMOOTH;
      break;
    }
    if (res_fac == 0)
    {
      /* Zero factor found. If it was the last method for this
         side, then one stops the cofactorization. Otherwise, one
         tries with an other method */
      continue;
    }

    found += res_fac;
    if (res_fac == 2)
      break;

    /*
       res_fac == 1  Only one factor has been found. Hence, our
       factorization is not finished.
       */
    if (fm.arith != CHOOSE_NONE)
    {
      int found2 = facul_aux(factors+res_fac, fm, data, i+1);
      if (found2 < 1)// FACUL_NOT_SMOOTH or FACUL_MAYBE
      {
        found = FACUL_NOT_SMOOTH;
        modset_clear(&cfm);
        modset_clear(&fm);
        break;
      }
      else
        found += found2;
      modset_clear(&fm);
    }
    if (cfm.arith != CHOOSE_NONE)
    {
      int found2 = facul_aux(factors+res_fac, cfm, data, i+1);
      if (found2 < 1)// FACUL_NOT_SMOOTH or FACUL_MAYBE
      {
        found = FACUL_NOT_SMOOTH;
      }
      else
        found += found2;
      modset_clear(&cfm);
      break;
    }
    break;
  }
  return found;
}


// Returns 1 if norm is smooth, 0 otherwise.
// Factorization is given in factors.
int call_facul(factor_ptr factors, mpz_srcptr norm_r, facul_aux_data * data) {
  unsigned long B = data->fbb;

  mpz_t norm;
  mpz_init(norm);
  mpz_set(norm, norm_r);

  // Trial divide all the small factors.
  int success = brute_force_factorize_ul(factors, norm, norm, B);
  if (success) {
    return 1;
  }

  // TODO: move this test after the next block
  // and do the primality test with fast routines.
  if (mpz_probab_prime_p(norm, 1)) {
    if (mpz_sizeinbase(norm, 2) <= data->lpb) {
      factor_append(factors, norm);
      return 1;
    } else {
      return 0;
    }
  }

  // Prepare stuff for calling facul() machinery.
  // Choose appropriate arithmetic according to size of norm
  // Result is stored in n, which is of type struct modset_t (see facul.h)
  struct modset_t n;
  n.arith = CHOOSE_NONE;
  size_t bits = mpz_sizeinbase(norm, 2);
  if (bits <= MODREDCUL_MAXBITS) {
    ASSERT(mpz_fits_ulong_p(norm));
    modredcul_initmod_ul(n.m_ul, mpz_get_ui(norm));
    n.arith = CHOOSE_UL;
  }
  else if (bits <= MODREDC15UL_MAXBITS)
  {
    unsigned long t[2];
    modintredc15ul_t m;
    size_t written;
    mpz_export(t, &written, -1, sizeof(unsigned long), 0, 0, norm);
    ASSERT_ALWAYS(written <= 2);
    modredc15ul_intset_uls(m, t, written);
    modredc15ul_initmod_int(n.m_15ul, m);
    n.arith = CHOOSE_15UL;
  }
  else if (bits <= MODREDC2UL2_MAXBITS)
  {
    unsigned long t[2];
    modintredc2ul2_t m;
    size_t written;
    mpz_export (t, &written, -1, sizeof(unsigned long), 0, 0, norm);
    ASSERT_ALWAYS(written <= 2);
    modredc2ul2_intset_uls (m, t, written);
    modredc2ul2_initmod_int (n.m_2ul2, m);
    n.arith = CHOOSE_2UL2;
  }
  else
  {
    modmpz_initmod_int(n.m_mpz, norm);
    n.arith = CHOOSE_MPZ;
  }
  ASSERT_ALWAYS(n.arith != CHOOSE_NONE);

  

  // Call the facul machinery.
  // TODO: think about this hard-coded 16...
  mpz_t * fac = (mpz_t *) malloc(sizeof(mpz_t)*16);
  for (int i = 0; i < 16; ++i)
    mpz_init(fac[i]);
  int found = facul_aux(fac, n, data, 0);
  if (found > 0) {
    for (int i = 0; i < found; ++i)
      factor_append(factors, fac[i]);
  }

  for (int i = 0; i < 16; ++i)
    mpz_clear(fac[i]);
  free(fac);
  mpz_clear(norm);
  return found > 0;
}

/*
 * A potential polynomial is found. Factorise it to verify if it gives a
 *  relation.
 *
 * a: the polynomial in the original lattice.
 * f: the polynomials that define the number fields.
 * lpb: the large prime bounds.
 * L:
 * size:
 * t: dimension of the lattice.
 * V: number of number fields.
 */
void good_polynomial(mpz_poly_srcptr a, mpz_poly_t * f,
    unsigned int * L, unsigned int size, unsigned int t, unsigned int V,
    int main, facul_aux_data *data)
{
  mpz_t res;
  mpz_init(res);
  factor_t * factor = (factor_t * ) malloc(sizeof(factor_t) * size);
  unsigned int * I = (unsigned int * ) malloc(sizeof(unsigned int) * size);

  unsigned int * assert_facto;
#ifdef ASSERT_FACTO
    assert_facto = (unsigned int * ) malloc(sizeof(unsigned int) * size);
#else // ASSERT_FACTO
    assert_facto = NULL;
#endif // ASSERT_FACTO

  unsigned int find = 0;
  if (main == -1) {
    for (unsigned int i = 0; i < size; i++) {
      norm_poly(res, f[L[i]], a);

      int is_smooth = call_facul(factor[i], res, &data[L[i]]);
#ifdef ASSERT_FACTO
      assert_facto[i] = factor_assert(factor[i], res);
#endif

      if (is_smooth) {
        find++;
        I[i] = 1;
        sort_factor(factor[i]);
      } else {
        I[i] = 0;
      }
    }
  } else {
    // TODO: this part of code is not tested.
    ASSERT(main >= 0);
    unsigned int Lmain = 0;
    while (L[Lmain] != (unsigned int)main) {
      Lmain++;
    }

    norm_poly(res, f[main], a);

    int main_is_smooth = call_facul(factor[Lmain], res, &data[main]);

    if (main_is_smooth) {
      find = 1;
#ifdef ASSERT_FACTO
      assert_facto[Lmain] = factor_assert(factor[Lmain], res);
#endif

      for (unsigned int i = 0; i < size; i++) {
        if (i != Lmain) {
          norm_poly(res, f[L[i]], a);

          int is_smooth = call_facul(factor[i], res, &data[L[i]]);
#ifdef ASSERT_FACTO
          assert_facto[i] = factor_assert(factor[i], res);
#endif

          if (is_smooth) {
            find++;
            I[i] = 1;
            sort_factor(factor[i]);
          } else {
            I[i] = 0;
          }
        }
      }
    }
  }

  if (find >= 2) {

    printf_relation(factor, I, L, a, t, V, size, assert_facto);

  }

  mpz_clear(res);
  for (unsigned int i = 0; i < size; i++) {
    factor_clear(factor[i]);
  }
  free(factor);
  free(I);

#ifdef ASSERT_FACTO
  free(assert_facto);
#endif
}

/*
 * Return the sum of the element in index. index has size V.
 *
 * index: array of index.
 * V: number of element, number of number fields.
 */
uint64_t sum_index(uint64_t * index, unsigned int V, int main)
{
  ASSERT(main >= -1);
  ASSERT(V >= 2);

  if (main != -1) {
    return index[main];
  }
  uint64_t sum = 0;
  for (unsigned int i = 0; i < V; i++) {
    sum += index[i];
  }
  return sum;
}

/*
 * Find in all the indices which one has the smallest value.
 *
 *
 *
 */
unsigned int find_indices(unsigned int ** L,
    uint64_array_t * indices, uint64_t * index, unsigned int V,
    uint64_t max_indices)
{
  * L = (unsigned int * ) malloc(sizeof(unsigned int) * (V));
  unsigned int size = 0;
  uint64_t min = max_indices;
  for (unsigned int i = 0; i < V; i++) {
    if (indices[i]->length != 0 && index[i] < indices[i]->length) {
      if (indices[i]->array[index[i]] < min) {
        min = indices[i]->array[index[i]];
        (*L)[0] = i;
        size = 1;
      } else if (min == indices[i]->array[index[i]]) {
        (*L)[size] = i;
        size++;
      }
    }
  }
  * L = realloc(* L, size * sizeof(unsigned int));
  return size;
}

unsigned int find_indices_main(unsigned int ** L, uint64_array_t * indices,
    uint64_t * index, unsigned int V, int main)
{
  ASSERT(main >= 0);

  unsigned int size = 1;
  uint64_t target = indices[main]->array[index[main]];
  (*L)[0] = main;
  for (unsigned int i = 0; i < V; i++) {
    if (i != (unsigned int) main && indices[i]->length != 0 && index[i] <
        indices[i]->length) {
      int test = 0;
      while (target > indices[i]->array[index[i]]) {
        index[i] = index[i] + 1;
        if (index[i] == indices[i]->length) {
          test = 1;
          break;
        }
      }
      if (test) {
        ASSERT(test == 1);

        if (target == indices[i]->array[index[i]]) {
          (*L)[size] = main;
          size++;
        }
      }
    }
  }
  
  * L = realloc(* L, size * sizeof(unsigned int));
  return size;
}

/*
 * For 
 */
void find_relation(uint64_array_t * indices, uint64_t * index,
    uint64_t number_element, mat_Z_srcptr matrix, mpz_poly_t * f,
    sieving_bound_srcptr H, unsigned int V, int main, uint64_t max_indices,
    facul_aux_data *data)
{
  unsigned int * L;
  unsigned int size = 0;
  if (main == -1) {
    size = find_indices(&L, indices, index, V, max_indices);
  } else {
    size = find_indices_main(&L, indices, index, V, main);
  }

  ASSERT(size >= 1);

  if (size >= 2) {
    mpz_vector_t c;
    mpz_t gcd;
    mpz_init(gcd);
    mpz_vector_init(c, H->t);
    array_index_mpz_vector(c, indices[L[0]]->array[index[L[0]]], H,
        number_element);

    mpz_poly_t a;
    mpz_poly_init(a, 0);
    mat_Z_mul_mpz_vector_to_mpz_poly(a, matrix, c);
    mpz_poly_content(gcd, a);

#ifdef NUMBER_SURVIVALS
      number_survivals++;
#endif // NUMBER_SURVIVALS

    //a must be irreducible.
    if (mpz_cmp_ui(gcd, 1) == 0 && a->deg > 0 &&
        mpz_cmp_ui(mpz_poly_lc_const(a), 0) > 0) {

#ifdef NUMBER_SURVIVALS
      number_survivals_facto++;
#endif // NUMBER_SURVIVALS

      good_polynomial(a, f, L, size, H->t, V, main, data);
    }

    mpz_poly_clear(a);

    mpz_clear(gcd);
    mpz_vector_clear(c);

    for (unsigned int i = 0; i < size; i++) {
      index[L[i]] = index[L[i]] + 1;
    }
  } else {
    index[L[0]] = index[L[0]] + 1;
  }
  free(L);
}

/*
 * To find the relations.
 *
 * indices: for each number field, positions where the norm is less than the
 *  threshold.
 * number_element: number of elements in the sieving region.
 * lpb: large prime bounds.
 * matrix: MqLLL.
 * f: polynomials that define the number fields.
 * H: sieving bounds.
 * V: number of number fields.
 */
void find_relations(uint64_array_t * indices, uint64_t number_element,
    mpz_t * lpb, mat_Z_srcptr matrix, mpz_poly_t * f, sieving_bound_srcptr H,
    unsigned int V, int main)
{
  //index[i] is the current index of indices[i].
  uint64_t * index = (uint64_t * ) malloc(sizeof(uint64_t) * V);
  uint64_t length_tot = 0;
  //Maximum of all the indices.
  uint64_t max_indices = 0;
  for (unsigned int i = 0; i < V; i++) {
    index[i] = 0;
    if (i == (unsigned int)main) {
      ASSERT(main >= 0);

      length_tot = indices[main]->length;
    } else if (indices[i]->length != 0 && main == -1) {
      length_tot += indices[i]->length;
      if (max_indices < indices[i]->array[indices[i]->length - 1]) {
        max_indices = indices[i]->array[indices[i]->length - 1];
      }
    }
  }

  //  prepare cofactorization strategy
  facul_aux_data * data;
  data = (facul_aux_data *)malloc(V*sizeof(facul_aux_data));
  ASSERT_ALWAYS(data != NULL);
  for (unsigned int i = 0; i < V; ++i) {
    size_t lpb_bit = mpz_sizeinbase(lpb[i], 2);
    unsigned int B = 1000; // FIXME: should be a parameter.
    data[i].lpb = lpb_bit;
    data[i].fbb = B;
    data[i].BB = ((double)B) * ((double)B);
    data[i].BBB = ((double)B) * data[i].BB;
    data[i].methods = facul_make_aux_methods(nb_curves95(lpb_bit), 0, 0);
  }

  if (0 != length_tot) {
    while(sum_index(index, V, main) < length_tot) {
      find_relation(indices, index, number_element, matrix, f, H, V, main,
          max_indices, data);
    }
  } else {
    printf("# No relations\n");
  }
  for (unsigned int i = 0; i < V; ++i)   
    facul_clear_aux_methods(data[i].methods);
  free(data);
  free(index);
}

/* ----- Usage and main ----- */

void declare_usage(param_list pl)
{
  param_list_decl_usage(pl, "H", "the sieving region");
  param_list_decl_usage(pl, "V", "number of number field");
  param_list_decl_usage(pl, "fbb0", "factor base bound on the number field 0");
  param_list_decl_usage(pl, "fbb1", "factor base bound on the number field 1");
  param_list_decl_usage(pl, "thresh0", "threshold on the number field 0");
  param_list_decl_usage(pl, "thresh1", "threshold on the number field 1");
  param_list_decl_usage(pl, "lpb0", "threshold on the number field 0");
  param_list_decl_usage(pl, "lpb1", "threshold on the number field 1");
  param_list_decl_usage(pl, "f0", "polynomial that defines the number field 0");
  param_list_decl_usage(pl, "f1", "polynomial that defines the number field 1");
  param_list_decl_usage(pl, "q_min", "minimum of the special-q");
  param_list_decl_usage(pl, "q_max", "maximum of the special-q");
  param_list_decl_usage(pl, "q_side", "side of the special-q");
  param_list_decl_usage(pl, "fb0",
      "path to the file that describe the factor base 0");
  param_list_decl_usage(pl, "fb1",
      "path to the file that describe the factor base 1");

  /* MNFS */

  param_list_decl_usage(pl, "fbb2", "factor base bound on the number field 2");
  param_list_decl_usage(pl, "fbb3", "factor base bound on the number field 3");
  param_list_decl_usage(pl, "fbb4", "factor base bound on the number field 4");
  param_list_decl_usage(pl, "fbb5", "factor base bound on the number field 5");
  param_list_decl_usage(pl, "fbb6", "factor base bound on the number field 6");
  param_list_decl_usage(pl, "fbb7", "factor base bound on the number field 7");
  param_list_decl_usage(pl, "fbb8", "factor base bound on the number field 8");
  param_list_decl_usage(pl, "fbb9", "factor base bound on the number field 9");
  param_list_decl_usage(pl, "thresh2", "threshold on the number field 2");
  param_list_decl_usage(pl, "thresh3", "threshold on the number field 3");
  param_list_decl_usage(pl, "thresh4", "threshold on the number field 4");
  param_list_decl_usage(pl, "thresh5", "threshold on the number field 5");
  param_list_decl_usage(pl, "thresh6", "threshold on the number field 6");
  param_list_decl_usage(pl, "thresh7", "threshold on the number field 7");
  param_list_decl_usage(pl, "thresh8", "threshold on the number field 8");
  param_list_decl_usage(pl, "thresh9", "threshold on the number field 9");
  param_list_decl_usage(pl, "lpb2", "threshold on the number field 2");
  param_list_decl_usage(pl, "lpb3", "threshold on the number field 3");
  param_list_decl_usage(pl, "lpb4", "threshold on the number field 4");
  param_list_decl_usage(pl, "lpb5", "threshold on the number field 5");
  param_list_decl_usage(pl, "lpb6", "threshold on the number field 6");
  param_list_decl_usage(pl, "lpb7", "threshold on the number field 7");
  param_list_decl_usage(pl, "lpb8", "threshold on the number field 8");
  param_list_decl_usage(pl, "lpb9", "threshold on the number field 9");
  param_list_decl_usage(pl, "f2", "polynomial that defines the number field 2");
  param_list_decl_usage(pl, "f3", "polynomial that defines the number field 3");
  param_list_decl_usage(pl, "f4", "polynomial that defines the number field 4");
  param_list_decl_usage(pl, "f5", "polynomial that defines the number field 5");
  param_list_decl_usage(pl, "f6", "polynomial that defines the number field 6");
  param_list_decl_usage(pl, "f7", "polynomial that defines the number field 7");
  param_list_decl_usage(pl, "f8", "polynomial that defines the number field 8");
  param_list_decl_usage(pl, "f9", "polynomial that defines the number field 9");
  param_list_decl_usage(pl, "fb2",
      "path to the file that describe the factor base 2");
  param_list_decl_usage(pl, "fb3",
      "path to the file that describe the factor base 3");
  param_list_decl_usage(pl, "fb4",
      "path to the file that describe the factor base 4");
  param_list_decl_usage(pl, "fb5",
      "path to the file that describe the factor base 5");
  param_list_decl_usage(pl, "fb6",
      "path to the file that describe the factor base 6");
  param_list_decl_usage(pl, "fb7",
      "path to the file that describe the factor base 7");
  param_list_decl_usage(pl, "fb8",
      "path to the file that describe the factor base 8");
  param_list_decl_usage(pl, "fb8",
      "path to the file that describe the factor base 9");
  param_list_decl_usage(pl, "main",
      "main side to cofactorise");
}

/*
 * Initialise the parameters of the special-q sieve.
 *
 * f: the V functions to define the number fields.
 * fbb: the V factor base bounds.
 * t: dimension of the lattice.
 * H: sieving bound.
 * q_min: lower bound of the special-q range.
 * q_max: upper bound of the special-q range.
 * thresh: the V threshold.
 * lpb: the V large prime bounds.
 * array: array in which the norms are stored.
 * matrix: the Mq matrix (set with zero coefficients).
 * q_side: side of the special-q.
 * V: number of number fields.
 */
void initialise_parameters(int argc, char * argv[], mpz_poly_t ** f,
    uint64_t ** fbb, factor_base_t ** fb, sieving_bound_ptr H,
    uint64_t * q_min, uint64_t * q_max, unsigned char ** thresh, mpz_t ** lpb,
    array_ptr array, mat_Z_ptr matrix, unsigned int * q_side, unsigned int * V,
    int * main_side)
{
  param_list pl;
  param_list_init(pl);
  declare_usage(pl);
  FILE * fpl;
  char * argv0 = argv[0];

  argv++, argc--;
  for( ; argc ; ) {
    if (param_list_update_cmdline(pl, &argc, &argv)) { continue; }

    /* Could also be a file */
    if ((fpl = fopen(argv[0], "r")) != NULL) {
      param_list_read_stream(pl, fpl, 0);
      fclose(fpl);
      argv++,argc--;
      continue;
    }

    fprintf(stderr, "Unhandled parameter %s\n", argv[0]);
    param_list_print_usage(pl, argv0, stderr);
    exit (EXIT_FAILURE);
  }

  param_list_parse_uint(pl, "V", V);
  ASSERT(* V >= 2 && * V < 11);

  unsigned int t;
  int * r;
  param_list_parse_int_list_size(pl, "H", &r, &t, ".,");
  ASSERT(t > 2);
  ASSERT(t == 3); // TODO: remove as soon as possible.
  sieving_bound_init(H, t);
  for (unsigned int i = 0; i < t; i++) {
    sieving_bound_set_hi(H, i, (unsigned int) r[i]);
  }
  free(r);

  //TODO: something strange here.
  * fbb = malloc(sizeof(uint64_t) * (* V));
  * thresh = malloc(sizeof(unsigned char) * (* V));
  * fb = malloc(sizeof(factor_base_t) * (* V));
  * f = malloc(sizeof(mpz_poly_t) * (* V));
  * lpb = malloc(sizeof(mpz_t) * (* V));

  for (unsigned int i = 0; i < * V; i++) {
    char str [2];
    sprintf(str, "f%u", i);
    mpz_poly_init((*f)[i], -1);
    param_list_parse_mpz_poly(pl, str, (**f) + i, ".,");
  }

  for (unsigned int i = 0; i < * V; i++) {
    char str [4];
    sprintf(str, "fbb%u", i);
    param_list_parse_uint64(pl, str, (* fbb) + i);
  }

  for (unsigned int i = 0; i < * V; i++) {
    char str [4];
    sprintf(str, "lpb%u", i);
    mpz_init((*lpb)[i]);
    param_list_parse_mpz(pl, str,(*lpb)[i]);
    ASSERT(mpz_cmp_ui((*lpb)[i], (*fbb)[i]) >= 0);
  }

#ifdef MAKE_FB_DURING_SIEVE
  for (unsigned int i = 0; i < * V; i++) {
    factor_base_init((*fb)[i], (*fbb)[i], (*fbb)[i], (*fbb)[i]);
  }
  double sec = seconds();
  makefb(*fb, *f, *fbb, H->t, *lpb, * V);
  printf("# Time for makefb: %f.\n", seconds() - sec);
#else
  double sec = seconds();
  for (unsigned int i = 0; i < * V; i++) {
    char str [3];
    sprintf(str, "fb%u", i);
    unsigned int size_path = 1024;
    char path [size_path];
    param_list_parse_string(pl, str, path, size_path);
    FILE * file;
    file = fopen(path, "r");
    read_factor_base(file, (*fb)[i], (*fbb)[i], (*lpb)[i], (*f)[i]);
    fclose(file);
  }
  printf("# Time to read factor bases: %f.\n", seconds() - sec);
#endif

  for (unsigned int i = 0; i < * V; i++) {
    char str [7];
    sprintf(str, "thresh%u", i);
    param_list_parse_uchar(pl, str, (* thresh) + i);
  }

  uint64_t number_element = sieving_bound_number_element(H);
  ASSERT(number_element >= 6);

  //TODO: q_side is an unsigned int.
  param_list_parse_int(pl, "q_side", (int *)q_side);
  ASSERT(* q_side < * V);
  param_list_parse_uint64(pl, "q_min", q_min);
  ASSERT(* q_min > fbb[0][* q_side]);
  param_list_parse_uint64(pl, "q_max", q_max);
  ASSERT(* q_min < * q_max);

  param_list_clear(pl);

  array_init(array, number_element);

  mat_Z_init(matrix, t, t);

  * main_side = -1;
  //TODO: error in valgrind, because of no main.
  param_list_parse_int(pl, "main", main_side);
  param_list_clear(pl);
}

/*
  The main.
*/
int main(int argc, char * argv[])
{
  unsigned int V;
  mpz_poly_t * f;
  uint64_t * fbb;
  sieving_bound_t H;
  uint64_t q_min;
  uint64_t q_max;
  unsigned int q_side;
  unsigned char * thresh;
  mpz_t * lpb;
  array_t array;
  mat_Z_t matrix;
  factor_base_t * fb;
  uint64_t q;
  int main_side;

  initialise_parameters(argc, argv, &f, &fbb, &fb, H, &q_min, &q_max,
                        &thresh, &lpb, array, matrix, &q_side, &V, &main_side);

#ifdef PRINT_PARAMETERS
  printf("# H =\n");
  sieving_bound_fprintf_comment(stdout, H);
  printf("# V = %u\n", V);
  for (unsigned int i = 0; i < V; i++) {
    printf("# fbb%u = %" PRIu64 "\n", i, fbb[i]);
  }
  for (unsigned int i = 0; i < V; i++) {
    printf("# thresh%u = %u\n", i, (unsigned int)thresh[i]);
  }
  for (unsigned int i = 0; i < V; i++) {
    gmp_printf("# lpb%u = %Zd\n", i, lpb[i]);
  }
  for (unsigned int i = 0; i < V; i++) {
    printf("# f%u = ", i);
    mpz_poly_fprintf(stdout, f[i]);
  }
  printf("# q_min = %" PRIu64 "\n", q_min);
  printf("# q_max = %" PRIu64 "\n", q_min);
  printf("# q_side = %u\n", q_side);
#endif // PRINT_PARAMETERS

  //Store all the index of array with resulting norm less than thresh.
  uint64_array_t * indexes =
    (uint64_array_t * ) malloc(sizeof(uint64_array_t) * V);

#ifdef OLD_NORM
  double ** pre_compute = (double ** ) malloc(sizeof(double * ) * V);
  for (unsigned int i = 0; i < V; i++) {
    pre_compute[i] = (double * ) malloc((H->t) * sizeof(double));
    pre_computation(pre_compute[i], f[i], H->t);
  }
#endif // OLD_NORM

  double ** time = (double ** ) malloc(sizeof(double * ) * V);
  for (unsigned int i = 0; i < V; i++) {
    time[i] = (double * ) malloc(sizeof(double) * 3);
  }
  double sec_tot;
  double sec_cofact;
  double sec;

#ifdef NUMBER_SURVIVALS
  uint64_t * numbers_survivals = (uint64_t * ) malloc(sizeof(uint64_t) * V);
#endif // NUMBER_SURVIVALS

#ifdef TRACE_POS
  file = fopen("TRACE_POS.txt", "w+");
  fprintf(file, "TRACE_POS: %d\n", TRACE_POS);
#endif // TRACE_POS

  ASSERT(q_min >= fbb[q_side]);
  gmp_randstate_t state;
  mpz_t a;
  mpz_poly_factor_list l;

  mpz_poly_factor_list_init(l);
  gmp_randinit_default(state);
  mpz_init(a);


  prime_info pi;
  prime_info_init (pi);
  //Pass all the prime less than q_min.
  for (q = 2; q < q_min; q = getprime_mt(pi)) {}

#ifdef SPECIAL_Q_IDEAL_U
    int deg_bound_factorise = (int)H->t;
#else // SPECIAL_Q_IDEAL_U
    int deg_bound_factorise = 2;
#endif // SPECIAL_Q_IDEAL_U

  for ( ; q <= q_max; q = getprime_mt(pi)) {
    ideal_spq_t special_q;
    ideal_spq_init(special_q);
    mpz_set_si(a, q);
    mpz_poly_factor(l, f[q_side], a, state);

    for (int i = 0; i < l->size ; i++) {

      if (l->factors[i]->f->deg < deg_bound_factorise) {
        if (l->factors[i]->f->deg == 1) {
          ideal_spq_set_part(special_q, q, l->factors[i]->f, H->t, 0);
        } else {
          ASSERT(l->factors[i]->f->deg > 1);

          ideal_spq_set_part(special_q, q, l->factors[i]->f, H->t, 1);
        } 

        printf("# Special-q: q: %" PRIu64 ", g: ", q);
        mpz_poly_fprintf(stdout, l->factors[i]->f);

#ifdef TRACE_POS
        fprintf(file, "Special-q: q: %" PRIu64 ", g: ", q);
        mpz_poly_fprintf(file, l->factors[i]->f);
#endif // TRACE_POS

        sec = seconds();
        sec_tot = sec;

        /* LLL part */
        build_Mq_ideal_spq(matrix, special_q);

#ifdef TRACE_POS
        fprintf(file, "Mq:\n");
        mat_Z_fprintf(file, matrix);
#endif // TRACE_POS

        mat_Z_LLL_transpose(matrix, matrix);

        /* /\* */
        /*   TODO: continue here. */
        /*   If the last coefficient of the last vector is negative, we do not */
        /*    sieve in the good direction. We therefore take the opposite vector */
        /*    because we want that a = MqLLL * c, with c in the sieving region */
        /*    has the last coordinate positive. */
        /* *\/ */
        /* if (mpz_cmp_ui(matrix->coeff[matrix->NumRows] */
        /*                [matrix->NumCols], 0) < 0) { */
        /*   for (unsigned int rows = 1; rows <= matrix->NumRows; rows++) { */
        /*     mpz_mul_si(matrix->coeff[rows][matrix->NumCols], */
        /*                matrix->coeff[rows][matrix->NumCols], -1); */
        /*   } */
        /* } */

#ifdef TRACE_POS
        fprintf(file, "MqLLL:\n");
        mat_Z_fprintf(file, matrix);
#endif // TRACE_POS

#ifdef MEAN_NORM_BOUND
        double * norms_bound = (double * ) malloc(sizeof(double) * V);
#endif // MEAN_NORM_BOUND

#ifdef MEAN_NORM
        double * norms = (double * ) malloc(sizeof(double) * V);
#endif // MEAN_NORM

#ifdef MEAN_NORM_BOUND_SIEVE
        double * norms_bound_sieve = (double * ) malloc(sizeof(double) * V);
        memset(norms_bound_sieve, 0, sizeof(double) * V);
#endif // MEAN_NORM_BOUND_SIEVE

        for (unsigned int j = 0; j < V; j++) {
          sec = seconds();
          uint64_array_init(indexes[j], array->number_element);
          memset(array->array, 255,
              sizeof(unsigned char) * array->number_element);
#ifndef OLD_NORM
          init_norm(array, H, matrix, f[j], special_q, !(j ^ q_side));
#else // OLD_NORM
          init_norm(array, pre_compute[j], H, matrix, f[j], special_q,
              !(j ^ q_side));
#endif // OLD_NORM

          time[j][0] = seconds() - sec;

#ifdef ASSERT_NORM
          assert_norm(array, H, f[j], matrix);
          printf("----------------------------------------\n");
#endif // ASSERT_NORM

#ifdef MEAN_NORM_BOUND
          norms_bound[j] = norm_bound;
#endif // MEAN_NORM_BOUND

#ifdef MEAN_NORM
          norms[j] = norm;
#endif // MEAN_NORM

          sec = seconds();
          special_q_sieve(array, matrix, fb[j], H, f[j]);
          time[j][1] = seconds() - sec;
          sec = seconds();
#ifdef MEAN_NORM_BOUND_SIEVE
          find_index(indexes[j], array, thresh[j], norms_bound_sieve + j);
#else
          find_index(indexes[j], array, thresh[j]);
#endif // MEAN_NORM_BOUND_SIEVE
          time[j][2] = seconds() - sec;

#ifdef TRACE_POS
        fprintf(file, "********************\n");
#endif // TRACE_POS
        }

        sec = seconds();
        find_relations(indexes, array->number_element, lpb, matrix, f, H, V,
            main_side);
        sec_cofact = seconds() - sec;

        for (unsigned j = 0; j < V; j++) {

#ifdef NUMBER_SURVIVALS
          numbers_survivals[j] = indexes[j]->length;
#endif // NUMBER_SURVIVALS

          uint64_array_clear(indexes[j]);
        }

        printf("# Time for this special-q: %fs.\n", seconds() - sec_tot);
        for (unsigned int j = 0; j < V; j++) {
          printf("# Time to init norm %d: %fs.\n", j, time[j][0]);

#ifdef MEAN_NORM
          printf("# Mean of the norm (bit size) %d: %f.\n",
                 j, log2(norms[j] / (double) array->number_element));
#endif // MEAN_NORM

#ifdef MEAN_NORM_BOUND
          printf("# Mean of the bound of the norm (bit size) %d: %f.\n",
                 j, log2(norms_bound[j] / (double) array->number_element));
#endif // MEAN_NORM_BOUND

#ifdef MEAN_NORM_BOUND_SIEVE
          printf("# Mean of the bit size of the bound of the residual norm %d: %f.\n",
                 j, norms_bound_sieve[j] / (double) array->number_element);
#endif // MEAN_NORM_BOUND_SIEVE

          printf("# Time to sieve %d: %fs.\n", j, time[j][1]);
          printf("# Time to find indexes %d: %fs.\n", j, time [j][2]);

#ifdef NUMBER_SURVIVALS
          printf("# Number of survivals %d: %" PRIu64 ".\n", j,
                 numbers_survivals[j]);
#endif // NUMBER_SURVIVALS
        }

#ifdef NUMBER_SURVIVALS
        free(numbers_survivals);
#endif // NUMBER_SURVIVALS

        printf("# Time to factorize: %fs.\n", sec_cofact);

#ifdef NUMBER_SURVIVALS
        printf("# Number total of survivals: %" PRIu64 ".\n",
               number_survivals);
        printf("# Number total of polynomial a survivals: %" PRIu64 ".\n",
               number_survivals_facto);
#endif // NUMBER_SURVIVALS

        printf("# ----------------------------------------\n");

#ifdef MEAN_NORM
        free(norms);
#endif // MEAN_NORM

#ifdef MEAN_NORM_BOUND
        free(norms_bound);
#endif // MEAN_NORM_BOUND

#ifdef MEAN_NORM_BOUND_SIEVE
        free(norms_bound_sieve);
#endif // MEAN_NORM_BOUND_SIEVE

#ifdef TRACE_POS
        fprintf(file, "----------------------------------------\n");
#endif // TRACE_POS

      }
    }

    ideal_spq_clear(special_q, H->t);
  }

  mpz_poly_factor_list_clear(l);
  gmp_randclear(state);
  mpz_clear(a);
  prime_info_clear (pi);

#ifdef TRACE_POS
  fclose(file);
#endif // TRACE_POS

  mat_Z_clear(matrix);
  for (unsigned int i = 0; i < V; i++) {
    mpz_clear(lpb[i]);
    mpz_poly_clear(f[i]);
    factor_base_clear(fb[i], H->t);
#ifdef OLD_NORM
    free(pre_compute[i]);
#endif // OLD_NORM
    free(time[i]);
  }
  free(time);
  free(indexes);
#ifdef OLD_NORM
  free(pre_compute);
#endif // OLD_NORM
  array_clear(array);
  free(lpb);
  free(f);
  free(fb);
  sieving_bound_clear(H);
  free(fbb);
  free(thresh);

  return 0;
}