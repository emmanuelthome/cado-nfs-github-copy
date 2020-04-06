#include "cado.h"
#include "bblas_gauss.h"
#include "test_bblas_level4.hpp"
#include "bblas_perm_matrix.hpp"
#include "time_bblas_common.hpp"

#ifdef  HAVE_M4RI
void test_bblas_level4::m4ri_plu_tests(int n)
{
    mzd_t * M;
    mzd_t * LU;
    mzp_t * P, *Q;
    M = mzd_init(n, n);
#if 0
    mzd_set_mem(M, m, n);
    uint64_t * m = new uint64_t[n*n/64];
    memfill_random(m, (64) * sizeof(uint64_t), rstate);
    delete[] m;
#else
    my_mzd_randomize(M);
#endif
    LU = mzd_init(n, n);
    P = mzp_init(n);
    Q = mzp_init(n);
    TIME1N(2, mzd_mypluq, LU, M, P, Q, 0);
    TIME1N(2, mzd_myechelonize_m4ri, LU, M, 0, 0);
    TIME1N(2, mzd_myechelonize_pluq, LU, M, 0);
    mzd_free(M);
    mzd_free(LU);
    mzp_free(P);
    mzp_free(Q);
}
#endif

int gauss_MN_C(unsigned int bM, unsigned int bN, gmp_randstate_t rstate)
{
    constexpr const unsigned int B = mat64::width;
    unsigned int M = B * bM;
    unsigned int N = B * bN;
    mat64 * mm = mat64::alloc(bM * bN);
    memfill_random(mm, bM * bN * sizeof(mat64), rstate);
    int r = kernel((mp_limb_t*)mm, NULL, M, N, N/ULONG_BITS, M/ULONG_BITS);
    mat64::free(mm);
    return r;
}


test_bblas_base::tags_t test_bblas_level4::gauss_tags { "gauss", "l4" };
void test_bblas_level4::gauss() {
    mat64 m;
    mat64 e;
    mat64 mm;
    mat64 l, u, p;
    mat64_fill_random(m, rstate);
    mat64_fill_random(e, rstate);
    mat64_fill_random(mm, rstate);
    mat64 m4[4];
    mat64 u4[4];
    memfill_random(m4, 4 * sizeof(mat64), rstate);
    // printf("-- for reference: best matrix mult, 64x64 --\n");
    // TIME1(2, mul_6464_6464, mm, e, m);
    // TIME1(2, mul_N64_T6464, mm, e, m, 64);
    // TIME1(2, gauss_6464_C, mm, e, m);
    // TIME1(2, gauss_6464_imm, mm, e, m);
    // TIME1(2, PLUQ64_inner, NULL, l, u, m, 0);
    int phi[128];
    {
        perm_matrix p, q;
        mat64 m[4], l[4], u[4];
        memfill_random(m, 4 * sizeof(mat64), rstate);
        perm_matrix_init(p, 128);
        perm_matrix_init(q, 128);
        TIME1(2, PLUQ128, p, l, u, q, m);
        perm_matrix_clear(p);
        perm_matrix_clear(q);
    }
    int n=2;
    TIME1N(2, memfill_random, m4, n*sizeof(mat64), rstate);
    TIME1N_SPINS(, 2, PLUQ64_n, phi, l, u4, m4, 64*n);
    TIME1N_SPINS(memfill_random(m4, n*sizeof(mat64), rstate), 2, PLUQ64_n, phi, l, u4, m4, 64*n);
    TIME1(2, LUP64_imm, l, u, p, m);
    TIME1(2, full_echelon_6464_imm, mm, e, m);
    TIME1(2, gauss_128128_C, m4);
    TIME1(2, gauss_MN_C, 10, 2, rstate);
    TIME1(2, gauss_MN_C, 2, 10, rstate);
    TIME1(2, gauss_MN_C, 100, 2, rstate);
    TIME1(2, gauss_MN_C, 2, 100, rstate);
    TIME1(2, gauss_MN_C, 1000, 2, rstate);
    TIME1(2, gauss_MN_C, 2, 1000, rstate);
    TIME1(2, gauss_MN_C, 32, 32, rstate);
#ifdef  HAVE_M4RI
    m4ri_plu_tests(64);
    m4ri_plu_tests(128);
    m4ri_plu_tests(256);
    m4ri_plu_tests(512);
    m4ri_plu_tests(1024);
#endif
}

