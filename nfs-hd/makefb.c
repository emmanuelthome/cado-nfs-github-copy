#include "cado.h"
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <gmp.h>
#include <inttypes.h>
#include "utils.h"
#include "macros.h"
#include "getprime.h"
#include "makefb.h"
#include "utils_int64.h"

/*
 * Mode:
 *   MAIN: export factor bases. Default file name is: <p>,<n>,<#numberfield>.
 *   TIME_MAKEFB: get the time to build factor bases.
 */

//mpz_poly_factor does not work in characteristic 2, so we do it the naive way.

/*
 * Add 1 to f. If the constant term is equal to 1, set this term to 0 and
 *  propagate the addition of 1 to the next coefficient, and so on.
 *
 * f: the polynomial on which the addition is computed, the modifications are
 *  made on f.
 */
static void mpz_poly_add_one_in_F2(mpz_poly_ptr f)
{
  ASSERT(f->deg >= 1);

  int i = 0;
  while (mpz_cmp_ui(f->coeff[i], 1) == 0) {
    mpz_poly_setcoeff_si(f, i, 0);
    i++;
    if (i > f->deg) {
      break;
    }
  }
  mpz_poly_setcoeff_si(f, i, 1);
}

/*
 * Factorise naively a mpz_poly mod 2.
 */
void mpz_poly_factor2(mpz_poly_factor_list_ptr list, mpz_poly_srcptr f)
{
  //set p to 2, because we work in F2.
  mpz_t p;
  mpz_init(p);
  mpz_set_ui(p, 2);

  //make a copy of f.
  mpz_poly_t fcopy;
  mpz_poly_init(fcopy, f->deg);
  mpz_poly_set(fcopy, f);

  //reduce all the coefficient mod p.
  mpz_t coeff;
  mpz_init(coeff);
  for (int i = 0; i <= f->deg; i++) {
    mpz_poly_getcoeff(coeff, i, f);
    mpz_mod(coeff, coeff, p);
    mpz_poly_setcoeff(fcopy, i, coeff);
  }

  //Purge list.
  mpz_poly_factor_list_flush(list);
 
  //If deg(f) in F2 is less than 1, we have the factor.
  if (fcopy->deg < 1) {
    mpz_clear(coeff);
    mpz_poly_clear(fcopy);
    mpz_clear(p);
    ASSERT(list->size == 0);
    return;
  } else if (fcopy->deg == 1) {
    mpz_clear(coeff);
    mpz_poly_factor_list_push(list, fcopy, 1);
    mpz_poly_clear(fcopy);
    mpz_clear(p);
    ASSERT(list->size == 1);
    return;
  }

  if (mpz_poly_is_irreducible(f, p)) {
    //If f is irreducible mod 2, fcopy is the factorisation of f mod 2.
    mpz_poly_factor_list_push(list, fcopy, 1);
  } else {
    //Create the first possible factor.
    mpz_poly_t tmp;
    mpz_poly_init(tmp, 1);
    mpz_poly_setcoeff_int64(tmp, 1, 1);

    //Enumerate all the possible factor of f mod 2.
    while (tmp->deg <= fcopy->deg) {
      //tmp is a possible factor.
      if (mpz_poly_is_irreducible(tmp, p)) {
        mpz_poly_t q;
        mpz_poly_init(q, 0);
        mpz_poly_t r;
        mpz_poly_init(r, 0);
        //Euclidean division of fcopy
        mpz_poly_div_qr(q, r, fcopy, tmp, p);
        //Power of the possible factor.
        unsigned int m = 0;
        //While fcopy is divisible by tmp.
        while (r->deg == -1) {
          //Increase the power of tmp.
          m++;
          mpz_poly_set(fcopy, q);
          if (fcopy->deg == 0 || fcopy->deg == -1) {
            //No other possible factor.
            break;
          }
          mpz_poly_div_qr(q, r, fcopy, tmp, p);
        }
        if (m != 0) {
          //Push tmp^m as a factor of f mod 2.
          mpz_poly_factor_list_push(list, tmp, m);
        }
        mpz_poly_clear(q);
        mpz_poly_clear(r);
      }
      //Go to the next possible polynomial in F2.
      mpz_poly_add_one_in_F2(tmp);
    }
    mpz_poly_clear(tmp);
  }

#ifndef NDBEBUG
  //Verify if the factorisation is good.
  mpz_poly_cleandeg(fcopy, -1);
  for (int i = 0; i <= f->deg; i++) {
    mpz_poly_getcoeff(coeff, i, f);
    mpz_mod(coeff, coeff, p);
    mpz_poly_setcoeff(fcopy, i, coeff);
  }

  mpz_poly_t fmul;
  mpz_poly_init(fmul, -1);
  mpz_poly_set(fmul, list->factors[0]->f);
  for (int j = 1; j < list->factors[0]->m; j++) {
    mpz_poly_mul(fmul, fmul, list->factors[0]->f);
  }
  for (int i = 1; i < list->size ; i++) {
    for (int j = 0; j < list->factors[i]->m; j++) {
      mpz_poly_mul(fmul, fmul, list->factors[i]->f);
    }
  }
  for (int i = 0; i <= fmul->deg; i++) {
    mpz_poly_getcoeff(coeff, i, fmul);
    mpz_mod(coeff, coeff, p);
    mpz_poly_setcoeff(fmul, i, coeff);
  }

  ASSERT(mpz_poly_cmp(fcopy, fmul) == 0);

  mpz_poly_clear(fmul);
#endif // NDBEBUG

  mpz_clear(coeff);
  mpz_poly_clear(fcopy);
  mpz_clear(p);
}

/*
 * Set an ideal_1 at an index.
 *
 * fb: the factor base.
 * index: index in the factor base.
 * r: the r of the ideal (r, h).
 * h: the h of the ideal (r, h).
 * fbb: factor base bound for this side.
 */
static void add_ideal_1_part(factor_base_ptr fb, uint64_t * index, uint64_t r,
    mpz_poly_srcptr h, uint64_t fbb, unsigned int t)
{
  ASSERT(h->deg == 1);

  //Verify if the ideal can be added.
  if (r <= fbb) {
    factor_base_set_ideal_1_part(fb, * index, r, h, t);
    * index = * index + 1;
  }
}

/*
 * Set an ideal_u at an index.
 *
 * fb: the factor base.
 * index: index in the factor base.
 * r: the r of the ideal (r, h).
 * h: the h of the ideal (r, h).
 * fbb: factor base bound for this side.
 * lpb: large prime bound.
 */
static void add_ideal_u_part(factor_base_ptr fb, uint64_t * index, uint64_t r,
    mpz_poly_srcptr h, uint64_t fbb, mpz_t lpb, unsigned int t)
{
  ASSERT(h->deg > 1);

  //Verify if the ideal can be added.
  if (mpz_cmp_ui(lpb, pow_uint64_t(r, (uint64_t)h->deg)) >= 0 && r <= fbb) {
    factor_base_set_ideal_u_part(fb, * index, r, h, t);
    * index = * index + 1;
  }
}

/*
 * Set an ideal_pr at an index.
 *
 * fb: the factor base.
 * index: index in the factor base.
 * r: the r of the ideal (r, h).
 * fbb: factor base bound for this side.
 */
static void add_ideal_pr_part(factor_base_ptr fb, uint64_t * index, uint64_t r,
    uint64_t fbb, unsigned int t)
{
  //Verify if the ideal can be added.
  if (r <= fbb) {
    factor_base_set_ideal_pr(fb, * index, r, t);
    * index = * index + 1;
  }
}

void makefb(factor_base_t * fb, mpz_poly_t * f, uint64_t * fbb, unsigned int t,
    mpz_t * lpb, unsigned int V)
{
  ASSERT(V >= 2);
  ASSERT(t >= 2);

#ifndef NDEBUG
  for (unsigned int i = 0; i < V; i++) {
    ASSERT(f[i]->deg >= 1);
    ASSERT(fbb[i] > 2);
    ASSERT(mpz_cmp_ui(lpb[i], fbb[i]) >= 0);
  }
#endif // NDEBUG
 
  //Factorise in Fq. 
  uint64_t q = 2;
  //Count number of ideal_1, ideal_u and ideal_pr for each sides.
  uint64_t * index1 = (uint64_t * ) malloc(sizeof(uint64_t) * V);
  uint64_t * indexu = (uint64_t * ) malloc(sizeof(uint64_t) * V);
  uint64_t * indexpr = (uint64_t * ) malloc(sizeof(uint64_t) * V);
  gmp_randstate_t state;
  //a = q in mpz. We need a to use mpz_poly_factor.
  mpz_t a;
  mpz_poly_factor_list l;

  mpz_t zero;
  mpz_init(zero);
  mpz_set_ui(zero, 0);

  //Contains the leading coefficient of f[k].
  mpz_t lc;
  mpz_init(lc);

  for (unsigned int k = 0; k < V; k++) {
    index1[k] = 0;
    indexu[k] = 0;
    indexpr[k] = 0;
  }

  gmp_randinit_default(state);
  mpz_init(a);
  mpz_poly_factor_list_init(l);

  //F2.
  mpz_set_ui(a, 2);
  for (unsigned int k = 0; k < V; k++) {
    mpz_set(lc, mpz_poly_lc_const(f[k]));
    //Verify if there exists a projective root.
    if (mpz_congruent_p(lc, zero, a) != 0) {
      add_ideal_pr_part(fb[k], indexpr + k, q, fbb[k], t);
    }
    //Find the factorisation of f[k] mod 2.
    mpz_poly_factor2(l, f[k]);
    for (int i = 0; i < l->size ; i++) {
      if (l->factors[i]->f->deg == 1) {
        add_ideal_1_part(fb[k], index1 + k, q, l->factors[i]->f, fbb[k], t);
      } else if (l->factors[i]->f->deg < (int)t) {
        add_ideal_u_part(fb[k], indexu + k, q, l->factors[i]->f, fbb[k], lpb[k], t);
      }
    }
  }

  //Next prime.
  q = getprime(q);
  //Find the maximum of fbb.
  uint64_t qmax = fbb[0];
  for (unsigned int k = 1; k < V; k++) {
    qmax = MAX(qmax, fbb[k]);
  }

  //For all the prime q less than the max of fbb.
  for ( ; q <= qmax; q = getprime(q)) {
    mpz_set_ui(a, q);
    for (unsigned int k = 0; k < V; k++) {
      //Projective root?
      mpz_set(lc, mpz_poly_lc_const(f[k]));
      if (mpz_congruent_p(lc, zero, a) != 0) {
        add_ideal_pr_part(fb[k], indexpr + k, q, fbb[k], t);
      }
      if (q <= fbb[k]) {
        //Factorization of f[k] mod q.
        mpz_poly_factor(l, f[k], a, state);
        for (int i = 0; i < l->size ; i++) {
          if (l->factors[i]->f->deg == 1) {
            add_ideal_1_part(fb[k], index1 + k, q, l->factors[i]->f, fbb[k], t);
          } else if (l->factors[i]->f->deg < (int)t) {
            add_ideal_u_part(fb[k], indexu + k, q, l->factors[i]->f, fbb[k],
                lpb[k], t);
          }
        }
      }
    }
  }

  mpz_poly_factor_list_clear(l);

  //Realloc the factor base with just the number of each type of ideal.
  for (unsigned int k = 0; k < V; k++) {
    factor_base_realloc(fb[k], index1[k], indexu[k], indexpr[k]);
  }

  mpz_clear(lc);
  mpz_clear(zero);
  free(index1);
  free(indexu);
  free(indexpr);
  gmp_randclear(state);
  mpz_clear(a);
  getprime(0);
}

/* Parse different types. */

static int parse_ulong(unsigned long * x, char ** endptr, char * ptr)
{
  unsigned long xx;
  errno = 0;
  xx = strtoul(ptr, endptr, 10);
  if (errno) {
    // failure
    return 0;
  }
  *x = xx;
  return 1;
}

static int parse_uint64(uint64_t * x, char ** endptr, char * ptr)
{
  return parse_ulong((unsigned long *) x, endptr, ptr);
}

/*
 * Read z from ptr and advance pointer.
 * Assume z has been initialized.
 * Return 0 or 1 for failure / success
 */
static int parse_mpz(mpz_t z, char ** endptr, char * ptr)
{
  int r = gmp_sscanf(ptr, "%Zd", z);
  if (r != 1) {
    *endptr = ptr;
    return 0; // failure
  }
  *endptr = ptr;
  while (isdigit(*endptr[0]) || *endptr[0] == '-') {
    (*endptr)++;
  }
  return 1;
}

/*
 * Read z from ptr and advance pointer.
 * Assume z has been initialized.
 * Return 0 or 1 for failure / success
 */
static int parse_int(int * i, char ** endptr, char * ptr)
{
  int r = sscanf(ptr, "%d", i);
  if (r != 1) {
    *endptr = ptr;
    return 0; // failure
  }
  *endptr = ptr;
  while (isdigit(*endptr[0]) || *endptr[0] == '-') {
    (*endptr)++;
  }
  return 1;
}

/*
 * Read z0,z1,...,zk from ptr and advance pointer.
 * Assume all the zi have been initialized and that the array is large
 * enough to hold everyone.
 * Return the number of zi that are parsed (0 means error or empty list)
 */
static int parse_cs_mpzs(mpz_t *z, char ** endptr, char * ptr)
{
  char *myptr = ptr;
  int cpt = 0;
  for(;;) {
    int ret = parse_mpz(z[cpt], endptr, myptr);
    if (!ret) {
      return 0; // failure or empty list
    }
    // got an mpz
    cpt++;
    myptr = *endptr;
    if (myptr[0] != ',') {
      // finished!
      *endptr = myptr;
      return cpt;
    }
    // prepare for next mpz
    myptr++;
  }
}

static int parse_mpz_poly(mpz_poly_ptr f, char ** endptr, char * str, int n)
{
  int ret;
  
  mpz_t * coeffs = (mpz_t *) malloc(sizeof(mpz_t) * (n + 1));
  for (int i = 0; i <= n; i++) {
    mpz_init(coeffs[i]);
  }
  ret = parse_cs_mpzs(coeffs, endptr, str);
  mpz_poly_setcoeffs(f, coeffs, n);
  for (int i = 0; i <= n; i++) {
    mpz_clear(coeffs[i]);
  }
  free(coeffs);
  
  return ret;
}

static int parse_line_mpz_poly(mpz_poly_ptr f, char * str, int n)
{
  char * tmp;
  if (str[0] != 'f' || str[1] != ':')
    return 0;
  str += 2;
  
  return parse_mpz_poly(f, &tmp, str, n);
}

static int parse_ideal_1(ideal_1_ptr ideal, char *str, unsigned int t)
{
  char *tmp;
  int ret;
 
  if (str[0] != '1' || str[1] != ':')
    return 0;
  str += 2;
  uint64_t r;
  ret = parse_uint64(&r, &tmp, str);
  
  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;
  mpz_poly_t h;
  mpz_poly_init(h, 1);
  ret = parse_mpz_poly(h, &tmp, str, 1);
  ASSERT(ret == 2);

  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;
  mpz_t * Tr = (mpz_t *) malloc(sizeof(mpz_t) * (t - 1));
  for (unsigned int i = 0; i < t - 1; i++) {
    mpz_init(Tr[i]);
  }
  ret = parse_cs_mpzs(Tr, &tmp, str);
  ASSERT(ret == (int)(t - 1));

  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;

  unsigned char log = (unsigned char)atoi(str);

  ideal_1_set_element(ideal, r, h, Tr, log, t);

  mpz_poly_clear(h);
  for (unsigned int i = 0; i < t - 1; i++) {
    mpz_clear(Tr[i]);
  }
  free(Tr);
  return ret;
}

static int parse_ideal_u(ideal_u_ptr ideal, char * str, unsigned int t)
{
  char *tmp;
  int ret;

  int deg; 
  ret = parse_int(&deg, &tmp, str);

  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;

  uint64_t r;
  ret = parse_uint64(&r, &tmp, str);
  
  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;
  mpz_poly_t h;
  mpz_poly_init(h, deg);
  ret = parse_mpz_poly(h, &tmp, str, deg);
  ASSERT(ret == deg + 1);

  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;
  int size_Tr = ((int)t - deg) * deg;
  mpz_t * Tr = (mpz_t *) malloc(sizeof(mpz_t) * size_Tr);
  for (int i = 0; i < size_Tr; i++) {
    mpz_init(Tr[i]);
  }
  ret = parse_cs_mpzs(Tr, &tmp, str);
  ASSERT(ret == size_Tr);

  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;

  unsigned char log = (unsigned char)atoi(str);

  ideal_u_set_element(ideal, r, h, Tr, log, t);

  mpz_poly_clear(h);
  for (int i = 0; i < size_Tr; i++) {
    mpz_clear(Tr[i]);
  }
  free(Tr);
  return ret;
}

static int parse_ideal_pr(ideal_pr_ptr ideal, char *str, unsigned int t)
{
  char *tmp;
  int ret;
 
  if (str[0] != '1' || str[1] != ':')
    return 0;
  str += 2;
  uint64_t r;
  ret = parse_uint64(&r, &tmp, str);
  
  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;
  mpz_poly_t h;
  mpz_poly_init(h, 1);
  ret = parse_mpz_poly(h, &tmp, str, 1);
  ASSERT(ret == 2);
  ASSERT(mpz_cmp_ui(h->coeff[0], 0) == 0);
  ASSERT(mpz_cmp_ui(h->coeff[1], 1) == 0);

  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;
  mpz_t * Tr = (mpz_t *) malloc(sizeof(mpz_t) * (t - 1));
  for (unsigned int i = 0; i < t - 1; i++) {
    mpz_init(Tr[i]);
  }
  ret = parse_cs_mpzs(Tr, &tmp, str);
  ASSERT(ret == (int)(t - 1));
#ifndef NDEBUG
  for (unsigned int i = 0; i < t - 1; i++) {
    ASSERT(mpz_cmp_ui(Tr[i], 0) == 0);
  }
#endif

  if (!ret || tmp[0] != ':')
    return 0;
  str = tmp + 1;

  unsigned char log = (unsigned char)atoi(str);

  ideal_pr_set_element(ideal, r, log, t);

  mpz_poly_clear(h);
  for (unsigned int i = 0; i < t - 1; i++) {
    mpz_clear(Tr[i]);
  }
  free(Tr);
  return ret;
}

void read_factor_base(FILE * file, factor_base_ptr fb, uint64_t fbb,
    mpz_srcptr lpb, MAYBE_UNUSED mpz_poly_srcptr f)
{
  int size_line = 1024;
  char line [size_line];

  uint64_t fbb_tmp;
  fscanf(file, "fbb:%" PRIu64 "\n", &fbb_tmp);
  ASSERT(fbb <= fbb_tmp);

  mpz_t lpb_tmp;
  mpz_init(lpb_tmp);
  gmp_fscanf(file, "lpb:%Zd\n", lpb_tmp);
  ASSERT(mpz_cmp(lpb, lpb_tmp) <= 0);
  mpz_clear(lpb_tmp);

  int n;
  fscanf(file, "deg:%d\n", &n);
  
  mpz_poly_t f_tmp;
  mpz_poly_init(f_tmp, n);
  if (fgets(line, size_line, file) == NULL) {
    return;
  }
  ASSERT_ALWAYS(parse_line_mpz_poly(f_tmp, line, n) == n + 1);
  ASSERT(mpz_poly_cmp(f, f_tmp) == 0);
  mpz_poly_clear(f_tmp);

  unsigned int t;
  fscanf(file, "t:%u\n", &t);

  uint64_t max_number_element_1, max_number_element_u, max_number_element_pr;
  fscanf(file, "%" PRIu64 ":%" PRIu64 ":%" PRIu64 "\n", &max_number_element_1,
      &max_number_element_u, &max_number_element_pr);
 
  factor_base_init(fb, max_number_element_1, max_number_element_u,
      max_number_element_pr);

  uint64_t number_element_1, number_element_u, number_element_pr;
 
  number_element_1 = 0; 
  ideal_1_t ideal_1;
  ideal_1_init(ideal_1);
  for (uint64_t i = 0; i < max_number_element_1; i++) {
    if (fgets(line, size_line, file) == NULL) {
      return;
    }
    parse_ideal_1(ideal_1, line, t);
    if (ideal_1->ideal->r <= fbb) {
      factor_base_set_ideal_1(fb, number_element_1, ideal_1, t);
      number_element_1++;
    }
  }
  ideal_1_clear(ideal_1, t);

  number_element_u = 0;
  ideal_u_t ideal_u;
  ideal_u_init(ideal_u);
  for (uint64_t i = 0; i < max_number_element_u; i++) {
    if (fgets(line, size_line, file) == NULL) {
      return;
    }
    parse_ideal_u(ideal_u, line, t);
    if (mpz_cmp_ui(lpb, pow_uint64_t(ideal_u->ideal->r,
            (uint64_t)ideal_u->ideal->h->deg)) >= 0
            && ideal_u->ideal->r <= fbb) {
      //TODO: Problem here.
      factor_base_set_ideal_u(fb, number_element_u, ideal_u, t);
      number_element_u++;
    }
  }
  ideal_u_clear(ideal_u, t);
 
  number_element_pr = 0; 
  ideal_pr_t ideal_pr;
  ideal_pr_init(ideal_pr);
  for (uint64_t i = 0; i < max_number_element_pr; i++) {
    if (fgets(line, size_line, file) == NULL) {
      return;
    }
    parse_ideal_pr(ideal_pr, line, t);
    if (ideal_pr->ideal->r <= fbb) {
      factor_base_set_ideal_pr(fb, number_element_pr, ideal_pr->ideal->r, t);
      number_element_pr++;
    }
  }
  ideal_pr_clear(ideal_pr, t);

  factor_base_realloc(fb, number_element_1, number_element_u,
      number_element_pr);
}

#ifdef MAIN
/*
 * Write ideal in a file.
 */
void write_ideal(FILE * file, ideal_srcptr ideal)
{
  fprintf(file, "%d:", ideal->h->deg);
  fprintf(file, "%" PRIu64 ":", ideal->r);
  gmp_fprintf(file, "%Zd", ideal->h->coeff[0]);
  for (int i = 1; i <= ideal->h->deg; i++) {
    gmp_fprintf(file, ",%Zd", ideal->h->coeff[i]);
  }
  fprintf(file, ":");
}

/*
 * Write ideal_1 in a file.
 */
void write_ideal_1(FILE * file, ideal_1_srcptr ideal, unsigned int t) {
  write_ideal(file, ideal->ideal);
  for (unsigned int i = 0; i < t - 2; i++) {
    gmp_fprintf(file, "%Zd,", ideal->Tr[i]);
  }
  gmp_fprintf(file, "%Zd:", ideal->Tr[t - 2]);
  fprintf(file, "%u\n", ideal->log);
}

/*
 * Write ideal_u in a file.
 */
void write_ideal_u(FILE * file, ideal_u_srcptr ideal, unsigned int t) {
  write_ideal(file, ideal->ideal);
  for (int row = 0; row < ideal->ideal->h->deg - 1; row++) {
    for (int col = 0; col < (int)t - ideal->ideal->h->deg - 1; col++) {
      gmp_fprintf(file, "%Zd,", ideal->Tr[row][col]);
    }
    gmp_fprintf(file, "%Zd,", ideal->Tr[row]
                [t - (unsigned int)ideal->ideal->h->deg - 1]);
  }
  for (int col = 0; col < (int)t - ideal->ideal->h->deg - 1; col++) {
    gmp_fprintf(file, "%Zd,", ideal->Tr[ideal->ideal->h->deg - 1][col]);
  }
  gmp_fprintf(file, "%Zd:", ideal->Tr[ideal->ideal->h->deg - 1]
              [t - (unsigned int)ideal->ideal->h->deg - 1]);
  fprintf(file, "%u\n", ideal->log);
}

/*
 * Write ideal_pr in a file.
 */
void write_ideal_pr(FILE * file, ideal_pr_srcptr ideal, unsigned int t)
{
  write_ideal(file, ideal->ideal);
  for (unsigned int i = 0; i < t - 2; i++) {
    gmp_fprintf(file, "%Zd,", ideal->Tr[i]);
  }
  gmp_fprintf(file, "%Zd:", ideal->Tr[t - 2]);
  fprintf(file, "%u\n", ideal->log);
}

/*
 * Write factor base in a file.
 *
 * file: the file.
 * fb: the factor base.
 * f: polynomial that defines the number field.
 * fbb: factor base bound.
 * lpb: large prime bound.
 * t: dimension of the lattice.
 */
void export_factor_base(FILE * file, factor_base_srcptr fb, mpz_poly_srcptr f,
    int64_t fbb, mpz_srcptr lpb, unsigned int t)
{
  fprintf(file, "fbb:");
  fprintf(file, "%" PRIu64 "\n", fbb);
  fprintf(file, "lpb:");
  gmp_fprintf(file, "%Zd\n", lpb);
  fprintf(file, "deg:%d\n", f->deg); 
  fprintf(file, "f:");
  for (int i = 0; i < f->deg; i++) {
    gmp_fprintf(file, "%Zd,", f->coeff[i]);
  }
  gmp_fprintf(file, "%Zd\n", f->coeff[f->deg]);
  fprintf(file, "t:%u\n", t);
  fprintf(file, "%" PRIu64 ":%" PRIu64 ":%" PRIu64 "\n", fb->number_element_1,
      fb->number_element_u, fb->number_element_pr);
  
  for (uint64_t i = 0; i < fb->number_element_1; i++) {
    write_ideal_1(file, fb->factor_base_1[i], t);
  }
  
  for (uint64_t i = 0; i < fb->number_element_u; i++) {
    write_ideal_u(file, fb->factor_base_u[i], t);
  }
  
  for (uint64_t i = 0; i < fb->number_element_pr; i++) {
    write_ideal_pr(file, fb->factor_base_pr[i], t);
  }
}

void declare_usage(param_list pl)
{
  param_list_decl_usage(pl, "p", "prime number");
  param_list_decl_usage(pl, "n", "extension");
  param_list_decl_usage(pl, "t", "dimension of the lattice");
  param_list_decl_usage(pl, "V", "number of number field");
  param_list_decl_usage(pl, "fbb0", "factor base bound on the number field 0");
  param_list_decl_usage(pl, "fbb1", "factor base bound on the number field 1");
  param_list_decl_usage(pl, "lpb0", "threshold on the number field 0");
  param_list_decl_usage(pl, "lpb1", "threshold on the number field 1");
  param_list_decl_usage(pl, "f0", "polynomial that defines the number field 0");
  param_list_decl_usage(pl, "f1", "polynomial that defines the number field 1");

  /* MNFS */

  param_list_decl_usage(pl, "fbb2", "factor base bound on the number field 2");
  param_list_decl_usage(pl, "fbb3", "factor base bound on the number field 3");
  param_list_decl_usage(pl, "fbb4", "factor base bound on the number field 4");
  param_list_decl_usage(pl, "fbb5", "factor base bound on the number field 5");
  param_list_decl_usage(pl, "fbb6", "factor base bound on the number field 6");
  param_list_decl_usage(pl, "fbb7", "factor base bound on the number field 7");
  param_list_decl_usage(pl, "fbb8", "factor base bound on the number field 8");
  param_list_decl_usage(pl, "fbb9", "factor base bound on the number field 9");
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
}

/*
 * Initialise all the parameters. Frequently read from command line.
 *
 * f: array of mpz_poly that define the number fields.
 * fbb: factor base bounds.
 * fb: factor bases.
 * t: dimension of the lattice.
 * lpb: large prime bounds.
 * V: number of number fields.
 * p: characteristic of the finite field.
 * n: extension of the finite field.
 */
void initialise_parameters(int argc, char * argv[], mpz_poly_t ** f,
    uint64_t ** fbb, factor_base_t ** fb, unsigned int * t, mpz_t ** lpb,
    unsigned int * V, uint64_t * p, unsigned int * n)
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

  * fbb = malloc(sizeof(uint64_t) * (* V));
  * fb = malloc(sizeof(factor_base_t) * (* V));
  * f = malloc(sizeof(mpz_poly_t) * (* V));
  * lpb = malloc(sizeof(mpz_t) * (* V));

  for (unsigned int i = 0; i < * V; i++) {
    char str [4];
    sprintf(str, "fbb%u", i);
    param_list_parse_uint64(pl, str, (* fbb) + i);
    factor_base_init((*fb)[i], (*fbb)[i], (*fbb)[i], (*fbb)[i]);
  }

  for (unsigned int i = 0; i < * V; i++) {
    char str [2];
    sprintf(str, "f%u", i);
    mpz_poly_init((*f)[i], -1);
    param_list_parse_mpz_poly(pl, str, (**f) + i, ".,");
  }

  for (unsigned int i = 0; i < * V; i++) {
    char str [4];
    sprintf(str, "lpb%u", i);
    mpz_init((**lpb) + i);
    param_list_parse_mpz(pl, str, (**lpb) + i);
    ASSERT(mpz_cmp_ui((*lpb)[i], (*fbb)[i]) >= 0);
  }

  param_list_parse_uint(pl, "t", t);
  ASSERT(* t > 2);

  param_list_parse_uint64(pl, "p", p);

  param_list_parse_uint(pl, "n", n);

  param_list_clear(pl);
}

int main(int argc, char ** argv)
{
  unsigned int V;
  mpz_poly_t * f;
  uint64_t * fbb;
  unsigned int t;
  mpz_t * lpb;
  factor_base_t * fb;
  uint64_t p;
  unsigned int n;

  initialise_parameters(argc, argv, &f, &fbb, &fb, &t, &lpb, &V, &p, &n);

#ifdef TIME_MAKEFB
  double sec = seconds();
#endif // TIME_MAKEFB

  makefb(fb, f, fbb, t, lpb, V);

  //5 because name of the file is p,n,V.
  unsigned int strlength =
    (unsigned int)(log((double) p) + log((double) n)) + 5;
  char str [strlength];
  for (unsigned int i = 0; i < V; i++) {
    sprintf(str, "%" PRIu64 ",%u,%u", p, n, i);
    FILE * file;
    file = fopen (str, "w+");
    export_factor_base(file, fb[i], f[i], fbb[i], lpb[i], t);
    fclose(file);
  }

#ifdef TIME_MAKEFB
  printf("# Time to build makefb: %fs.\n", seconds() - sec);
#endif // TIME_MAKEFB

  free(f);
  free(fbb);
  free(lpb);
  free(fb);

  return 0;
}
#endif // MAIN