/* Copyright (C) 1999--2007 Emmanuel Thom'e --- see LICENSE file */
#include "cado.h"

#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <stdexcept>
#ifdef  HAVE_OPENMP
#include <omp.h>
#endif
#include <cassert>
#include <fstream>
#include <sstream>

#include "portability.h"
#include "macros.h"
#include "utils.h"
#include "memusage.h"
#include "lingen_expected_pi_length.hpp"


#ifdef SELECT_MPFQ_LAYER_u64k1
#include "mpfq_fake.hpp"
#include "lingen_matpoly_binary.hpp"
#include "lingen_matpoly_ft.hpp"
#include "lingen_qcode_binary.hpp"
#else
/* Really, this should be always on. XXX FIXME */
#define ENABLE_MPI_LINGEN
#include "mpfq_layer.h"
/* lingen-matpoly is the default code. */
#include "lingen_matpoly.hpp"
#ifdef ENABLE_MPI_LINGEN
#include "lingen_bigmatpoly.hpp"
#include "lingen_bigmatpoly_ft.hpp"
#endif
#include "lingen_qcode_prime.hpp"
#endif


#include "bw-common.h"		/* Handy. Allows Using global functions
                                 * for recovering parameters */
#include "lingen.hpp"
#include "lingen_bmstatus.hpp"
#include "lingen_tuning.hpp"
#include "logline.h"
#include "tree_stats.hpp"
#include "sha1.h"

/* Call tree for methods within this program:
 *
 * Two separate entry points.
 *
 * bw_biglingen_collective        when need to go collective
 *     |<--> bw_biglingen_recursive , loops to bw_biglingen_collective
 *     \->bw_lingen_single               when it makes sense to do this locally again
 * bw_lingen_single                      when computation can be done locally
 *    |<->bw_lingen_recursive
 *    |->bw_lingen_basecase
 *
 */

#define MPI_MY_SIZE_T   MPI_UNSIGNED_LONG

static unsigned int display_threshold = 10;
static int with_timings = 0;

/* This is an indication of the number of bytes we read at a time for A
 * (input) and F (output) */
static unsigned int io_block_size = 1 << 20;

/* If non-zero, then reading from A is actually replaced by reading from
 * a random generator */
static unsigned int random_input_length = 0;

static int split_input_file = 0;  /* unsupported ; do acollect by ourselves */
static int split_output_file = 0; /* do split by ourselves */

gmp_randstate_t rstate;

static const char * checkpoint_directory;
static unsigned int checkpoint_threshold = 100;
static int save_gathered_checkpoints = 0;

static int allow_zero_on_rhs = 0;

int rank0_exit_code = EXIT_SUCCESS;


int global_flag_ascii = 0;
int global_flag_tune = 0;

void lingen_decl_usage(cxx_param_list & pl)/*{{{*/
{
    param_list_decl_usage(pl, "ascii",
            "read and write data in ascii");
    param_list_decl_usage(pl, "timings",
            "provide timings on all output lines");
    param_list_decl_usage(pl, "tune",
            "activate tuning mode");
    param_list_decl_usage(pl, "allow_zero_on_rhs",
            "do not cry if the generator corresponds to a zero contribution on the RHS vectors");

    /* we must be square ! And thr is not supported. */
    param_list_decl_usage(pl, "mpi", "number of MPI nodes across which the execution will span, with mesh dimensions");
    param_list_decl_usage(pl, "thr", "number of threads (on each node) for the program, with mesh dimensions");

    param_list_decl_usage(pl, "nrhs",
            "number of columns to treat differently, as corresponding to rhs vectors");
    param_list_decl_usage(pl, "rhs",
            "file with rhs vectors (only the header is read)");

    param_list_decl_usage(pl, "afile",
            "input sequence file");
    param_list_decl_usage(pl, "random-input-with-length",
            "use surrogate for input");
    param_list_decl_usage(pl, "split-input-file",
            "work with split files on input");
    param_list_decl_usage(pl, "split-output-file",
            "work with split files on output");
    param_list_decl_usage(pl, "random_seed",
            "seed the random generator");
    param_list_decl_usage(pl, "ffile",
            "output generator file");

    param_list_decl_usage(pl, "checkpoint-directory",
            "where to save checkpoints");
    param_list_decl_usage(pl, "checkpoint-threshold",
            "threshold for saving checkpoints");
    param_list_decl_usage(pl, "display-threshold",
            "threshold for outputting progress lines");
    param_list_decl_usage(pl, "io-block-size",
            "chunk size for reading the input or writing the output");

    param_list_decl_usage(pl, "lingen_mpi_threshold",
            "use MPI matrix operations above this size");
    param_list_decl_usage(pl, "lingen_threshold",
            "use recursive algorithm above this size");
    param_list_decl_usage(pl, "save_gathered_checkpoints",
            "save global checkpoints files, instead of per-job files");

    param_list_configure_switch(pl, "--tune", &global_flag_tune);
    param_list_configure_switch(pl, "--ascii", &global_flag_ascii);
    param_list_configure_switch(pl, "--timings", &with_timings);
    param_list_configure_alias(pl, "seed", "random_seed");

    lingen_tuning_decl_usage(pl);
    tree_stats::declare_usage(pl);
}/*}}}*/

/*}}}*/

/*{{{ avg_matsize */
template<bool over_gf2>
double avg_matsize(abdst_field, unsigned int m, unsigned int n, int ascii);

template<>
double avg_matsize<true>(abdst_field, unsigned int m, unsigned int n, int ascii)
{
    ASSERT_ALWAYS(!ascii);
    ASSERT_ALWAYS((m*n) % 64 == 0);
    return ((m*n)/64) * sizeof(uint64_t);
}

template<>
double avg_matsize<false>(abdst_field ab, unsigned int m, unsigned int n, int ascii)
{
    if (!ascii) {
        /* Easy case first. If we have binary input, then we know a priori
         * that the input data must have size a multiple of the element size.
         */
        size_t elemsize = abvec_elt_stride(ab, 1);
        size_t matsize = elemsize * m * n;
        return matsize;
    }

    /* Ascii is more complicated. We're necessarily fragile here.
     * However, assuming that each coefficient comes with only one space,
     * and each matrix with an extra space (this is how the GPU program
     * prints data -- not that this ends up having a considerable impact
     * anyway...), we can guess the number of bytes per matrix. */

    /* Formula for the average number of digits of an integer mod p,
     * written in base b:
     *
     * (k-((b^k-1)/(b-1)-1)/p)  with b = Ceiling(Log(p)/Log(b)).
     */
    double avg;
    cxx_mpz a;
    double pd = mpz_get_d(abfield_characteristic_srcptr(ab));
    unsigned long k = ceil(log(pd)/log(10));
    unsigned long b = 10;
    mpz_ui_pow_ui(a, b, k);
    mpz_sub_ui(a, a, 1);
    mpz_fdiv_q_ui(a, a, b-1);
    avg = k - mpz_get_d(a) / pd;
    // printf("Expect roughly %.2f decimal digits for integers mod p.\n", avg);
    double matsize = (avg + 1) * m * n + 1;
    // printf("Expect roughly %.2f bytes for each sequence matrix.\n", matsize);
    return matsize;
}

double avg_matsize(abdst_field ab, unsigned int m, unsigned int n, int ascii)
{
    return avg_matsize<matpoly::over_gf2>(ab, m, n, ascii);
}
/*}}}*/

/* {{{ I/O helpers */

/* {{{ matpoly_write
 * writes some of the matpoly data to f, either in ascii or binary
 * format. This can be used to write only part of the data (degrees
 * [k0..k1[). Returns the number of coefficients (i.e., matrices, so at
 * most k1-k0) successfully written, or
 * -1 on error (e.g. when some matrix was only partially written).
 */

#ifndef SELECT_MPFQ_LAYER_u64k1
int matpoly_write(abdst_field ab, std::ostream& os, matpoly const & M, unsigned int k0, unsigned int k1, int ascii, int transpose)
{
    unsigned int m = transpose ? M.n : M.m;
    unsigned int n = transpose ? M.m : M.n;
    ASSERT_ALWAYS(k0 == k1 || (k0 < M.get_size() && k1 <= M.get_size()));
    for(unsigned int k = k0 ; k < k1 ; k++) {
        int err = 0;
        int matnb = 0;
        for(unsigned int i = 0 ; !err && i < m ; i++) {
            for(unsigned int j = 0 ; !err && j < n ; j++) {
                absrc_elt x;
                x = transpose ? M.coeff(j, i, k) : M.coeff(i, j, k);
                if (ascii) {
                    if (j) err = !(os << " ");
                    if (!err) err = !(abcxx_out(ab, os, x));
                } else {
                    err = !(os.write((const char *) x, (size_t) abvec_elt_stride(ab, 1)));
                }
                if (!err) matnb++;
            }
            if (!err && ascii) err = !(os << "\n");
        }
        if (ascii) err = err || !(os << "\n");
        if (err) {
            return (matnb == 0) ? (int) (k - k0) : -1;
        }
    }
    return k1 - k0;
}
#else
int matpoly_write(abdst_field, std::ostream& os, matpoly const & M, unsigned int k0, unsigned int k1, int ascii, int transpose)
{
    unsigned int m = M.m;
    unsigned int n = M.n;
    ASSERT_ALWAYS(k0 == k1 || (k0 < M.get_size() && k1 <= M.get_size()));
    ASSERT_ALWAYS(m % ULONG_BITS == 0);
    ASSERT_ALWAYS(n % ULONG_BITS == 0);
    size_t ulongs_per_mat = m * n / ULONG_BITS;
    std::vector<unsigned long> buf(ulongs_per_mat);
    for(unsigned int k = k0 ; k < k1 ; k++) {
        buf.assign(ulongs_per_mat, 0);
        bool err = false;
        size_t kq = k / ULONG_BITS;
        size_t kr = k % ULONG_BITS;
        unsigned long km = 1UL << kr;
        if (!transpose) {
            for(unsigned int i = 0 ; i < m ; i++) {
                unsigned long * v = &(buf[i * (n / ULONG_BITS)]);
                for(unsigned int j = 0 ; j < n ; j++) {
                    unsigned int jq = j / ULONG_BITS;
                    unsigned int jr = j % ULONG_BITS;
                    unsigned long bit = (M.part(i, j)[kq] & km) != 0;
                    v[jq] |= bit << jr;
                }
            }
        } else {
            for(unsigned int j = 0 ; j < n ; j++) {
                unsigned long * v = &(buf[j * (m / ULONG_BITS)]);
                for(unsigned int i = 0 ; i < m ; i++) {
                    unsigned int iq = i / ULONG_BITS;
                    unsigned int ir = i % ULONG_BITS;
                    unsigned long bit = (M.part(i, j)[kq] & km) != 0;
                    v[iq] |= bit << ir;
                }
            }
        }
        if (ascii) {
            /* do we have an endian-robust wordsize-robust convention for
             * printing bitstrings in hex ?
             *
             * it's not even clear that we should care -- after all, as
             * long as mksol follows a consistent convention too, we
             * should be fine.
             */
            abort();
        } else {
            err = !(os.write((const char*) &buf[0], ulongs_per_mat * sizeof(unsigned long)));
        }
        if (err) return -1;
    }
    return k1 - k0;
}
#endif
/* }}} */


/* fw must be an array of FILE* pointers of exactly the same size as the
 * matrix to be written.
 */
template<typename Ostream>
int matpoly_write_split(abdst_field ab, std::vector<Ostream> & fw, matpoly const & M, unsigned int k0, unsigned int k1, int ascii)
{
    ASSERT_ALWAYS(k0 == k1 || (k0 < M.get_size() && k1 <= M.get_size()));
    for(unsigned int k = k0 ; k < k1 ; k++) {
        int err = 0;
        int matnb = 0;
        for(unsigned int i = 0 ; !err && i < M.m ; i++) {
            for(unsigned int j = 0 ; !err && j < M.n ; j++) {
                std::ostream& os = fw[i*M.n+j];
                absrc_elt x = M.coeff(i, j, k);
                if (ascii) {
                    err = !(abcxx_out(ab, os, x));
                    if (!err) err = !(os << "\n");
                } else {
                    err = !(os.write((const char *) x, (size_t) abvec_elt_stride(ab, 1)));
                }
                if (!err) matnb++;
            }
        }
        if (err) {
            return (matnb == 0) ? (int) (k - k0) : -1;
        }
    }
    return k1 - k0;
}
/* }}} */

/* {{{ matpoly_read
 * reads some of the matpoly data from f, either in ascii or binary
 * format. This can be used to parse only part of the data (degrees
 * [k0..k1[, k1 being an upper bound). Returns the number of coefficients
 * (i.e., matrices, so at most k1-k0) successfully read, or
 * -1 on error (e.g. when some matrix was only partially read).
 *
 * Note that the matrix must *not* be in pre-init state. It must have
 * been already allocated.
 */

#ifndef SELECT_MPFQ_LAYER_u64k1
int matpoly_read(abdst_field ab, FILE * f, matpoly & M, unsigned int k0, unsigned int k1, int ascii, int transpose)
{
    ASSERT_ALWAYS(!M.check_pre_init());
    unsigned int m = transpose ? M.n : M.m;
    unsigned int n = transpose ? M.m : M.n;
    ASSERT_ALWAYS(k0 == k1 || (k0 < M.get_size() && k1 <= M.get_size()));
    for(unsigned int k = k0 ; k < k1 ; k++) {
        int err = 0;
        int matnb = 0;
        for(unsigned int i = 0 ; !err && i < m ; i++) {
            for(unsigned int j = 0 ; !err && j < n ; j++) {
                abdst_elt x;
                x = transpose ? M.coeff(j, i, k)
                              : M.coeff(i, j, k);
                if (ascii) {
                    err = abfscan(ab, f, x) == 0;
                } else {
                    err = fread(x, abvec_elt_stride(ab, 1), 1, f) < 1;
                }
                if (!err) matnb++;
            }
        }
        if (err) return (matnb == 0) ? (int) (k - k0) : -1;
    }
    return k1 - k0;
}
#else
int matpoly_read(abdst_field, FILE * f, matpoly & M, unsigned int k0, unsigned int k1, int ascii, int transpose)
{
    unsigned int m = M.m;
    unsigned int n = M.n;
    ASSERT_ALWAYS(m % ULONG_BITS == 0);
    ASSERT_ALWAYS(n % ULONG_BITS == 0);
    size_t ulongs_per_mat = m * n / ULONG_BITS;
    std::vector<unsigned long> buf(ulongs_per_mat);
    for(unsigned int k = k0 ; k < k1 ; k++) {
        if (ascii) {
            /* do we have an endian-robust wordsize-robust convention for
             * printing bitstrings in hex ?
             *
             * it's not even clear that we should care -- after all, as long as
             * mksol follows a consistent convention too, we should be fine.
             */
            abort();
        } else {
            int rc = fwrite((const char*) &buf[0], sizeof(unsigned long), ulongs_per_mat, f);
            if (rc != (int) ulongs_per_mat)
                return k - k0;
        }
        M.zero_pad(k + 1);
        size_t kq = k / ULONG_BITS;
        size_t kr = k % ULONG_BITS;
        if (!transpose) {
            for(unsigned int i = 0 ; i < m ; i++) {
                unsigned long * v = &(buf[i * (n / ULONG_BITS)]);
                for(unsigned int j = 0 ; j < n ; j++) {
                    unsigned int jq = j / ULONG_BITS;
                    unsigned int jr = j % ULONG_BITS;
                    unsigned long jm = 1UL << jr;
                    unsigned long bit = v[jq] & jm;
                    M.part(i, j)[kq] |= bit << kr;
                }
            }
        } else {
            for(unsigned int j = 0 ; j < n ; j++) {
                unsigned long * v = &(buf[j * (m / ULONG_BITS)]);
                for(unsigned int i = 0 ; i < m ; i++) {
                    unsigned int iq = i / ULONG_BITS;
                    unsigned int ir = i % ULONG_BITS;
                    unsigned long im = 1UL << ir;
                    unsigned long bit = v[iq] & im;
                    M.part(i, j)[kq] |= bit << kr;
                }
            }
        }
    }
    return k1 - k0;
}
#endif
/* }}} */

/* }}} */

/*{{{ Checkpoints */

/* There's much copy-paste here */

struct cp_info {
    bmstatus & bm;
    int level;
    unsigned int t0;
    unsigned int t1;
    int mpi;
    int rank;
    char * auxfile;
    char * sdatafile;
    char * gdatafile;
    const char * datafile;
    /* be sure to change when needed */
    static constexpr unsigned long format = 2;
    FILE * aux;
    FILE * data;
    cp_info(bmstatus & bm, unsigned int t0, unsigned int t1, int mpi);
    ~cp_info();
    bool save_aux_file(size_t pi_size, int done) const;
    bool load_aux_file(size_t & pi_size, int & done);
    int load_data_file(matpoly & pi, size_t pi_size);
    int save_data_file(matpoly const & pi, size_t pi_size);
};

cp_info::cp_info(bmstatus & bm, unsigned int t0, unsigned int t1, int mpi)
    : bm(bm), t0(t0), t1(t1), mpi(mpi)
{
    if (mpi)
        MPI_Comm_rank(bm.com[0], &(rank));
    else
        rank = 0;
    int rc;
    level = bm.depth();
    rc = asprintf(&auxfile, "%s/pi.%d.%u.%u.aux",
            checkpoint_directory, level, t0, t1);
    ASSERT_ALWAYS(rc >= 0);
    rc = asprintf(&gdatafile, "%s/pi.%d.%u.%u.single.data",
            checkpoint_directory, level, t0, t1);
    ASSERT_ALWAYS(rc >= 0);
    rc = asprintf(&sdatafile, "%s/pi.%d.%u.%u.%d.data",
            checkpoint_directory, level, t0, t1, rank);
    ASSERT_ALWAYS(rc >= 0);
    datafile = mpi ? sdatafile : gdatafile;
}

cp_info::~cp_info()
{
    free(sdatafile);
    free(gdatafile);
    free(auxfile);
}

bool cp_info::save_aux_file(size_t pi_size, int done) const /*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    if (rank) return 1;
    std::ofstream os(auxfile);
    os << "format " << format << "\n";
    os << pi_size << "\n";
    for(unsigned int i = 0 ; i < m + n ; i++) os << " " << bm.delta[i];
    os << "\n";
    for(unsigned int i = 0 ; i < m + n ; i++) os << " " << bm.lucky[i];
    os << "\n";
    os << done;
    os << "\n";
    os << bm.hints;
    os << "\n";
    os << bm.stats;
    bool ok = os.good();
    if (!ok)
        unlink(auxfile);
    return ok;
}/*}}}*/

bool cp_info::load_aux_file(size_t & pi_size, int & done)/*{{{*/
{
    bmstatus nbm = bm;
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    if (rank) return 1;
    std::ifstream is(auxfile);
    if (!is.good()) return false;
    std::string hfstring;
    unsigned long hformat;
    is >> hfstring >> hformat;
    if (hfstring != "format") {
        fprintf(stderr, "Warning: checkpoint file cannot be used (version < 1)\n");
        return false;
    }
    if (hformat != format) {
        fprintf(stderr, "Warning: checkpoint file cannot be used (version %lu < %lu)\n", hformat, format);
        return false;
    }

    is >> pi_size;
    for(unsigned int i = 0 ; i < m + n ; i++) {
        is >> nbm.delta[i];
    }
    for(unsigned int i = 0 ; i < m + n ; i++) {
        is >> nbm.lucky[i];
    }
    is >> done;
    is >> nbm.hints;
    for(auto const & x : nbm.hints) {
        if (!x.second.check()) {
            fprintf(stderr, "Warning: checkpoint contains invalid schedule information\n");
            is.setstate(std::ios::failbit);
            return false;
        }
    }

    if (bm.hints != nbm.hints) {
        is.setstate(std::ios::failbit);
        fprintf(stderr, "Warning: checkpoint file cannot be used since it was made for another set of schedules (stats would be incorrect)\n");
        std::stringstream os;
        os << bm.hints;
        fprintf(stderr, "textual description of the schedule set that we expect to find:\n%s\n", os.str().c_str());
        return false;
    }

    if (!(is >> nbm.stats))
        return false;
   
    bm = std::move(nbm);

    return is.good();
}/*}}}*/

/* TODO: adapt for GF(2) */
int cp_info::load_data_file(matpoly & pi, size_t pi_size)/*{{{*/
{
    bw_dimensions & d = bm.d;
    abdst_field ab = d.ab;
    unsigned int m = d.m;
    unsigned int n = d.n;
    FILE * data = fopen(datafile, "rb");
    int rc;
    if (data == NULL) {
        fprintf(stderr, "Warning: cannot open %s\n", datafile);
        return 0;
    }
    pi = matpoly(ab, m+n, m+n, pi_size);
    pi.set_size(pi_size);
    rc = matpoly_read(ab, data, pi, 0, pi.get_size(), 0, 0);
    if (rc != (int) pi.get_size()) { fclose(data); return 0; }
    rc = fclose(data);
    return rc == 0;
}/*}}}*/

/* TODO: adapt for GF(2) */
/* I think we always have pi_size == pi.size, the only questionable
 * situation is when we're saving part of a big matrix */
int cp_info::save_data_file(matpoly const & pi, size_t pi_size)/*{{{*/
{
    abdst_field ab = bm.d.ab;
    std::ofstream data(datafile, std::ios_base::out | std::ios_base::binary);
    int rc;
    if (!data) {
        fprintf(stderr, "Warning: cannot open %s\n", datafile);
        unlink(auxfile);
        return 0;
    }
    rc = matpoly_write(ab, data, pi, 0, pi_size, 0, 0);
    if (rc != (int) pi.get_size()) goto cp_info_save_data_file_bailout;
    if (data.good()) return 1;
cp_info_save_data_file_bailout:
    unlink(datafile);
    unlink(auxfile);
    return 0;
}/*}}}*/

int load_checkpoint_file(bmstatus & bm, matpoly & pi, unsigned int t0, unsigned int t1, int & done)/*{{{*/
{
    if (!checkpoint_directory) return 0;
    if ((t1 - t0) < checkpoint_threshold) return 0;

    cp_info cp(bm, t0, t1, 0);

    ASSERT_ALWAYS(pi.check_pre_init());
    size_t pi_size;
    /* Don't output a message just now, since after all it's not
     * noteworthy if the checkpoint file does not exist. */
    int ok = cp.load_aux_file(pi_size, done);
    if (ok) {
        logline_begin(stdout, SIZE_MAX, "Reading %s", cp.datafile);
        ok = cp.load_data_file(pi, pi_size);
        logline_end(&bm.t_cp_io,"");
        if (!ok)
            fprintf(stderr, "Warning: I/O error while reading %s\n", cp.datafile);
    }
    if (ok) bm.t = t1;
    return ok;
}/*}}}*/

int save_checkpoint_file(bmstatus & bm, matpoly & pi, unsigned int t0, unsigned int t1, int done)/*{{{*/
{
    /* corresponding t is bm.t - E.size ! */
    if (!checkpoint_directory) return 0;
    if ((t1 - t0) < checkpoint_threshold) return 0;
    cp_info cp(bm, t0, t1, 0);
    logline_begin(stdout, SIZE_MAX, "Saving %s%s",
            cp.datafile,
            cp.mpi ? " (MPI, scattered)" : "");
    int ok = cp.save_aux_file(pi.get_size(), done);
    if (ok) ok = cp.save_data_file(pi, pi.get_size());
    logline_end(&bm.t_cp_io,"");
    if (!ok && !cp.rank)
        fprintf(stderr, "Warning: I/O error while saving %s\n", cp.datafile);
    return ok;
}/*}}}*/

#ifdef ENABLE_MPI_LINGEN
int load_mpi_checkpoint_file_scattered(bmstatus & bm, bigmatpoly & xpi, unsigned int t0, unsigned int t1, int & done)/*{{{*/
{
    int size;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (!checkpoint_directory) return 0;
    if ((t1 - t0) < checkpoint_threshold) return 0;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    bw_dimensions & d = bm.d;
    abdst_field ab = d.ab;
    unsigned int m = d.m;
    unsigned int n = d.n;
    cp_info cp(bm, t0, t1, 1);
    ASSERT_ALWAYS(xpi.check_pre_init());
    size_t pi_size;
    int ok = cp.load_aux_file(pi_size, done);
    MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
    MPI_Bcast(&pi_size, 1, MPI_MY_SIZE_T, 0, bm.com[0]);
    MPI_Bcast(&bm.delta[0], m + n, MPI_UNSIGNED, 0, bm.com[0]);
    MPI_Bcast(&bm.lucky[0], m + n, MPI_INT, 0, bm.com[0]);
    MPI_Bcast(&done, 1, MPI_INT, 0, bm.com[0]);
    if (ok) {
        logline_begin(stdout, SIZE_MAX, "Reading %s (MPI, scattered)",
                cp.datafile);
        do {
            FILE * data = fopen(cp.datafile, "rb");
            int rc;
            ok = data != NULL;
            MPI_Allreduce(MPI_IN_PLACE, &ok, 1, MPI_INT, MPI_MIN, bm.com[0]);
            if (!ok) {
                if (!rank)
                    fprintf(stderr, "Warning: cannot open %s\n", cp.datafile);
                if (data) free(data);
                break;
            }
            xpi.finish_init(ab, m+n, m+n, pi_size);
            xpi.set_size(pi_size);
            rc = matpoly_read(ab, data, xpi.my_cell(), 0, xpi.get_size(), 0, 0);
            ok = ok && rc == (int) xpi.get_size();
            rc = fclose(data);
            ok = ok && (rc == 0);
        } while (0);
        MPI_Allreduce(MPI_IN_PLACE, &ok, 1, MPI_INT, MPI_MIN, bm.com[0]);
        logline_end(&bm.t_cp_io,"");
        if (!ok && !rank) {
            fprintf(stderr, "Warning: I/O error while reading %s\n",
                    cp.datafile);
        }
    } else if (!rank) {
        fprintf(stderr, "Warning: I/O error while reading %s\n", cp.datafile);
    }
    if (ok) bm.t = t1;
    return ok;
}/*}}}*/

int save_mpi_checkpoint_file_scattered(bmstatus & bm, bigmatpoly const & xpi, unsigned int t0, unsigned int t1, int done)/*{{{*/
{
    /* corresponding t is bm.t - E.size ! */
    if (!checkpoint_directory) return 0;
    if ((t1 - t0) < checkpoint_threshold) return 0;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    cp_info cp(bm, t0, t1, 1);
    int ok = cp.save_aux_file(xpi.get_size(), done);
    MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
    if (!ok && !rank) unlink(cp.auxfile);
    if (ok) {
        logline_begin(stdout, SIZE_MAX, "Saving %s (MPI, scattered)",
                cp.datafile);
        ok = cp.save_data_file(xpi.my_cell(), xpi.get_size());
        logline_end(&bm.t_cp_io,"");
        MPI_Allreduce(MPI_IN_PLACE, &ok, 1, MPI_INT, MPI_MIN, bm.com[0]);
        if (!ok) {
            if (cp.datafile) unlink(cp.datafile);
            if (!rank) unlink(cp.auxfile);
        }
    }
    if (!ok && !rank) {
        fprintf(stderr, "Warning: I/O error while saving %s\n", cp.datafile);
    }
    return ok;
}/*}}}*/

int load_mpi_checkpoint_file_gathered(bmstatus & bm, bigmatpoly & xpi, unsigned int t0, unsigned int t1, int & done)/*{{{*/
{
    if (!checkpoint_directory) return 0;
    if ((t1 - t0) < checkpoint_threshold) return 0;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    bw_dimensions & d = bm.d;
    abdst_field ab = d.ab;
    unsigned int m = d.m;
    unsigned int n = d.n;
    cp_info cp(bm, t0, t1, 1);
    cp.datafile = cp.gdatafile;
    size_t pi_size;
    int ok = cp.load_aux_file(pi_size, done);
    MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
    MPI_Bcast(&pi_size, 1, MPI_MY_SIZE_T, 0, bm.com[0]);
    MPI_Bcast(&bm.delta[0], m + n, MPI_UNSIGNED, 0, bm.com[0]);
    MPI_Bcast(&bm.lucky[0], m + n, MPI_INT, 0, bm.com[0]);
    MPI_Bcast(&done, 1, MPI_INT, 0, bm.com[0]);
    if (ok) {
        logline_begin(stdout, SIZE_MAX, "Reading %s (MPI, gathered)",
                cp.datafile);
        do {
            FILE * data = NULL;
            if (!rank) ok = (data = fopen(cp.datafile, "rb")) != NULL;
            MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
            if (!ok) {
                if (!rank)
                    fprintf(stderr, "Warning: cannot open %s\n", cp.datafile);
                if (data) free(data);
                break;
            }

            xpi.finish_init(ab, m+n, m+n, pi_size);
            xpi.set_size(pi_size);

            double avg = avg_matsize(ab, m + n, m + n, 0);
            unsigned int B = iceildiv(io_block_size, avg);

            /* This is only temp storage ! */
            matpoly pi(ab, m + n, m + n, B);
            pi.zero_pad(B);

            for(unsigned int k = 0 ; ok && k < xpi.get_size() ; k += B) {
                unsigned int nc = MIN(B, xpi.get_size() - k);
                if (!rank)
                    ok = matpoly_read(ab, data, pi, 0, nc, 0, 0) == (int) nc;
                MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
                xpi.scatter_mat_partial(pi, k, nc);
            }

            if (!rank) {
                int rc = fclose(data);
                ok = ok && (rc == 0);
            }
        } while (0);
        MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
        logline_end(&bm.t_cp_io,"");
        if (!ok && !rank) {
            fprintf(stderr, "Warning: I/O error while reading %s\n",
                    cp.datafile);
        }
    } else if (!rank) {
        fprintf(stderr, "Warning: I/O error while reading %s\n", cp.datafile);
    }
    if (ok) bm.t = t1;
    return ok;
}/*}}}*/

int save_mpi_checkpoint_file_gathered(bmstatus & bm, bigmatpoly const & xpi, unsigned int t0, unsigned int t1, int done)/*{{{*/
{
    if (!checkpoint_directory) return 0;
    if ((t1 - t0) < checkpoint_threshold) return 0;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    bw_dimensions & d = bm.d;
    abdst_field ab = d.ab;
    unsigned int m = d.m;
    unsigned int n = d.n;
    cp_info cp(bm, t0, t1, 1);
    cp.datafile = cp.gdatafile;
    logline_begin(stdout, SIZE_MAX, "Saving %s (MPI, gathered)",
            cp.datafile);
    int ok = cp.save_aux_file(xpi.get_size(), done);
    MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
    if (ok) {
        do {
            std::ofstream data;
            if (!rank) {
                data.open(cp.datafile, std::ios_base::out | std::ios_base::binary);
                ok = (bool) data;
            }
            MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
            if (!ok) {
                if (!rank)
                    fprintf(stderr, "Warning: cannot open %s\n", cp.datafile);
                break;
            }

            double avg = avg_matsize(ab, m + n, m + n, 0);
            unsigned int B = iceildiv(io_block_size, avg);

            /* This is only temp storage ! */
            matpoly pi(ab, m + n, m + n, B);
            pi.zero_pad(B);

            for(unsigned int k = 0 ; ok && k < xpi.get_size() ; k += B) {
                unsigned int nc = MIN(B, xpi.get_size() - k);
                xpi.gather_mat_partial(pi, k, nc);
                if (!rank)
                    ok = matpoly_write(ab, data, pi, 0, nc, 0, 0) == (int) nc;
                MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
            }

            if (!rank) {
                data.close();
                ok = ok && (bool) data;
            }
        } while (0);
        MPI_Bcast(&ok, 1, MPI_INT, 0, bm.com[0]);
        if (!ok && !rank) {
            if (cp.datafile) unlink(cp.datafile);
            unlink(cp.auxfile);
        }
    }
    logline_end(&bm.t_cp_io,"");
    if (!ok && !rank) {
        fprintf(stderr, "Warning: I/O error while saving %s\n", cp.datafile);
    }
    return ok;
}/*}}}*/

int load_mpi_checkpoint_file(bmstatus & bm, bigmatpoly & xpi, unsigned int t0, unsigned int t1, int & done)/*{{{*/
{
    /* read scattered checkpoint with higher priority if available,
     * because we like distributed I/O. Otherwise, read gathered
     * checkpoint if we could find one.
     */
    if (!checkpoint_directory) return 0;
    if ((t1 - t0) < checkpoint_threshold) return 0;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    cp_info cp(bm, t0, t1, 1);
    int ok = 0;
    int aux_ok = rank || access(cp.auxfile, R_OK) == 0;
    int sdata_ok = access(cp.sdatafile, R_OK) == 0;
    int scattered_ok = aux_ok && sdata_ok;
    MPI_Allreduce(MPI_IN_PLACE, &scattered_ok, 1, MPI_INT, MPI_MIN, bm.com[0]);
    if (scattered_ok) {
        ok = load_mpi_checkpoint_file_scattered(bm, xpi, t0, t1, done);
        if (ok) return ok;
    }
    int gdata_ok = rank || access(cp.gdatafile, R_OK) == 0;
    int gathered_ok = aux_ok && gdata_ok;
    MPI_Bcast(&gathered_ok, 1, MPI_INT, 0, bm.com[0]);
    if (gathered_ok) {
        ok = load_mpi_checkpoint_file_gathered(bm, xpi, t0, t1, done);
    }
    return ok;
}/*}}}*/

int save_mpi_checkpoint_file(bmstatus & bm, bigmatpoly const & xpi, unsigned int t0, unsigned int t1, int done)/*{{{*/
{
    if (save_gathered_checkpoints) {
        return save_mpi_checkpoint_file_gathered(bm, xpi, t0, t1, done);
    } else {
        return save_mpi_checkpoint_file_scattered(bm, xpi, t0, t1, done);
    }
}/*}}}*/
#endif  /* ENABLE_MPI_LINGEN */

/*}}}*/

/**********************************************************************/

/*{{{ Main entry points and recursive algorithm (with and without MPI) */

/* Forward declaration, it's used by the recursive version */
int bw_lingen_single(bmstatus & bm, matpoly & pi, matpoly & E);

#ifdef ENABLE_MPI_LINGEN
int bw_biglingen_collective(bmstatus & bm, bigmatpoly & pi, bigmatpoly & E);
#endif

std::string sha1sum(matpoly const & X)
{
    sha1_checksumming_stream S;
    ASSERT_ALWAYS(X.is_tight());
    S.write((const char *) X.data_area(), X.data_size());
    char checksum[41];
    S.checksum(checksum);
    return std::string(checksum);
}

/* The degree of similarity between bw_lingen_recursive and
 * bw_biglingen_recursive is close to 100%. The only unresolved issue is
 * whether we pre-alloc before doing the caching operations.
 *
 * XXX Eventually we'll merge the two.
 */
template<typename fft_type>
int bw_lingen_recursive(bmstatus & bm, matpoly & pi, matpoly & E) /*{{{*/
{
    int depth = bm.depth();
    size_t z = E.get_size();

    /* C0 is a copy. We won't use it for long anyway. We'll take a
     * reference _later_ */
    lingen_call_companion C0 = bm.companion(depth, z);

    tree_stats::sentinel dummy(bm.stats, __func__, z, C0.total_ncalls);

    bm.stats.plan_smallstep("MP", C0.mp.tt);
    bm.stats.plan_smallstep("MUL", C0.mul.tt);

    bw_dimensions & d = bm.d;
    int done;

    /* we have to start with something large enough to get all
     * coefficients of E_right correct */
    size_t half = E.get_size() - (E.get_size() / 2);
    unsigned int pi_expect = expected_pi_length(d, bm.delta, E.get_size());
    unsigned int pi_expect_lowerbound = expected_pi_length_lowerbound(d, E.get_size());
    unsigned int pi_left_expect = expected_pi_length(d, bm.delta, half);
    unsigned int pi_left_expect_lowerbound = expected_pi_length_lowerbound(d, half);
    unsigned int pi_left_expect_used_for_shift = MIN(pi_left_expect, half + 1);

    /* declare an lazy-alloc all matrices */
    matpoly E_left;
    matpoly pi_left;
    matpoly pi_right;
    matpoly E_right;

    E_left = E.truncate_and_rshift(half, half + 1 - pi_left_expect_used_for_shift);

    // this (now) consumes E_left entirely.
    done = bw_lingen_single(bm, pi_left, E_left);

    ASSERT_ALWAYS(pi_left.get_size());

    if (done) {
        pi = std::move(pi_left);
        return done;
    }

    ASSERT_ALWAYS(pi_left.get_size() <= pi_left_expect);
    ASSERT_ALWAYS(done || pi_left.get_size() >= pi_left_expect_lowerbound);

    /* XXX I don't understand why I need to do this. It seems to me that
     * MP(XA, B) and MP(A, B) should be identical whenever deg A > deg B.
     */
    ASSERT_ALWAYS(pi_left_expect_used_for_shift >= pi_left.get_size());
    if (pi_left_expect_used_for_shift != pi_left.get_size()) {
        E.rshift(E, pi_left_expect_used_for_shift - pi_left.get_size());
        /* Don't shrink_to_fit at this point, because we've only made a
         * minor adjustment. */
    }

    logline_begin(stdout, z, "t=%u %*sMP(%zu, %zu) -> %zu",
            bm.t, depth,"",
            E.get_size(), pi_left.get_size(), E.get_size() - pi_left.get_size() + 1);

    {
        E_right = matpoly(d.ab, d.m, d.m+d.n, E.get_size() - pi_left.get_size() + 1);
        lingen_call_companion & C = bm.companion(depth, z);
        matpoly_ft<fft_type>::mp_caching(bm.stats, E_right, E, pi_left, & C.mp);
        E = matpoly();
    }

    logline_end(&bm.t_mp, "");

    unsigned int pi_right_expect = expected_pi_length(d, bm.delta, E_right.get_size());
    unsigned int pi_right_expect_lowerbound = expected_pi_length_lowerbound(d, E_right.get_size());

    done = bw_lingen_single(bm, pi_right, E_right);
    ASSERT_ALWAYS(pi_right.get_size() <= pi_right_expect);
    ASSERT_ALWAYS(done || pi_right.get_size() >= pi_right_expect_lowerbound);

    /* stack is now pi_left, pi_right */

    logline_begin(stdout, z, "t=%u %*sMUL(%zu, %zu) -> %zu",
            bm.t, depth, "",
            pi_left.get_size(), pi_right.get_size(), pi_left.get_size() + pi_right.get_size() - 1);

    {
        pi = matpoly(d.ab, d.m+d.n, d.m+d.n, pi_left.get_size() + pi_right.get_size() - 1);
        lingen_call_companion & C = bm.companion(depth, z);
        matpoly_ft<fft_type>::mul_caching(bm.stats, pi, pi_left, pi_right, & C.mul);
    }

    /* Note that the leading coefficients of pi_left and pi_right are not
     * necessarily full-rank, so that we have to fix potential zeros. If
     * we don't, the degree of pi artificially grows with the recursive
     * level.
     */
    unsigned int pisize = pi.get_size();
#if 1
    /* In fact, it's not entirely impossible that pi grows more than
     * what we had expected on entry, e.g. if we have one early
     * generator. So we can't just do this. Most of the time it will
     * work, but we can't claim that it will always work.
     *
     * One possible sign is when the entry deltas are somewhat even, and
     * the result deltas are unbalanced.
     */
    for(; pisize > pi_expect ; pisize--) {
        /* These coefficients really must be zero */
        ASSERT_ALWAYS(pi.coeff_is_zero(pisize - 1));
    }
    ASSERT_ALWAYS(pisize <= pi_expect);
#endif
    /* Now below pi_expect, it's not impossible to have a few
     * cancellations as well.
     */
    for(; pisize ; pisize--) {
        if (!pi.coeff_is_zero(pisize - 1)) break;
    }
    pi.set_size(pisize);
    ASSERT_ALWAYS(done || pisize >= pi_expect_lowerbound);

    logline_end(&bm.t_mul, "");

    return done;
}/*}}}*/

int bw_lingen_single(bmstatus & bm, matpoly & pi, matpoly & E) /*{{{*/
{
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    ASSERT_ALWAYS(!rank);
    unsigned int t0 = bm.t;
    unsigned int t1 = bm.t + E.get_size();

    int done;

    lingen_call_companion C = bm.companion(bm.depth(), E.get_size());

    if (load_checkpoint_file(bm, pi, t0, t1, done))
        return done;

    // ASSERT_ALWAYS(E.size < bm.lingen_mpi_threshold);

    // fprintf(stderr, "Enter %s\n", __func__);
    if (!bm.recurse(E.get_size())) {
        tree_stats::transition_sentinel dummy(bm.stats, "recursive_threshold", E.get_size(), C.total_ncalls);
        bm.t_basecase -= seconds();
        done = bw_lingen_basecase(bm, pi, E);
        bm.t_basecase += seconds();
    } else {
#ifndef SELECT_MPFQ_LAYER_u64k1
        typedef fft_transform_info fft_type;
#else
        typedef gf2x_cantor_fft_info fft_type;
#endif
        done = bw_lingen_recursive<fft_type>(bm, pi, E);
    }
    // fprintf(stderr, "Leave %s\n", __func__);

    save_checkpoint_file(bm, pi, t0, t1, done);

    return done;
}/*}}}*/

#ifdef ENABLE_MPI_LINGEN
template<typename fft_type>
int bw_biglingen_recursive(bmstatus & bm, bigmatpoly & pi, bigmatpoly & E) /*{{{*/
{
    int depth = bm.depth();
    size_t z = E.get_size();

    /* C0 is a copy. We won't use it for long anyway. We'll take a
     * reference _later_ */
    lingen_call_companion C0 = bm.companion(depth, z);

    tree_stats::sentinel dummy(bm.stats, __func__, z, C0.total_ncalls);

    bm.stats.plan_smallstep("MP", C0.mp.tt);
    bm.stats.plan_smallstep("MUL", C0.mul.tt);

    bw_dimensions & d = bm.d;
    int done;

    /* we have to start with something large enough to get
     * all coefficients of E_right correct */
    size_t half = E.get_size() - (E.get_size() / 2);
    unsigned int pi_expect = expected_pi_length(d, bm.delta, E.get_size());
    unsigned int pi_expect_lowerbound = expected_pi_length_lowerbound(d, E.get_size());
    unsigned int pi_left_expect = expected_pi_length(d, bm.delta, half);
    unsigned int pi_left_expect_lowerbound = expected_pi_length_lowerbound(d, half);
    unsigned int pi_left_expect_used_for_shift = MIN(pi_left_expect, half + 1);

    /* declare an lazy-alloc all matrices */
    bigmatpoly_model const& model(E);
    bigmatpoly E_left(model);
    bigmatpoly E_right(model);
    bigmatpoly pi_left(model);
    bigmatpoly pi_right(model);

    E_left = E.truncate_and_rshift(half, half + 1 - pi_left_expect_used_for_shift);

    done = bw_biglingen_collective(bm, pi_left, E_left);

    ASSERT_ALWAYS(pi_left.get_size());
    E_left = bigmatpoly(model);

    if (done) {
        pi = std::move(pi_left);
        return done;
    }

    ASSERT_ALWAYS(pi_left.get_size() <= pi_left_expect);
    ASSERT_ALWAYS(done || pi_left.get_size() >= pi_left_expect_lowerbound);

    /* XXX I don't understand why I need to do this. It seems to me that
     * MP(XA, B) and MP(A, B) should be identical whenever deg A > deg B.
     */
    ASSERT_ALWAYS(pi_left_expect_used_for_shift >= pi_left.get_size());
    if (pi_left_expect_used_for_shift != pi_left.get_size()) {
        E.rshift(E, pi_left_expect_used_for_shift - pi_left.get_size());
        /* Don't shrink_to_fit at this point, because we've only made a
         * minor adjustment. */
    }

    logline_begin(stdout, z, "t=%u %*sMPI-MP(%zu, %zu) -> %zu",
            bm.t, depth, "",
            E.get_size(), pi_left.get_size(), E.get_size() - pi_left.get_size() + 1);

    {
        ASSERT_ALWAYS(pi_left.ab);
        ASSERT_ALWAYS(E.ab);
        /* XXX should we pre-alloc ? We do that in the non-mpi case, but
         * that seems to be useless verbosity */
        lingen_call_companion & C = bm.companion(depth, z);
        bigmatpoly_ft<fft_type>::mp_caching(bm.stats, E_right, E, pi_left, &C.mp);
        E = bigmatpoly(model);
        ASSERT_ALWAYS(E_right.ab);
        MPI_Barrier(bm.com[0]);
    }

    logline_end(&bm.t_mp, "");

    unsigned int pi_right_expect = expected_pi_length(d, bm.delta, E_right.get_size());
    unsigned int pi_right_expect_lowerbound = expected_pi_length_lowerbound(d, E_right.get_size());

    done = bw_biglingen_collective(bm, pi_right, E_right);
    ASSERT_ALWAYS(pi_right.get_size() <= pi_right_expect);
    ASSERT_ALWAYS(done || pi_right.get_size() >= pi_right_expect_lowerbound);
    
    E_right = bigmatpoly(model);

    logline_begin(stdout, z, "t=%u %*sMPI-MUL(%zu, %zu) -> %zu",
            bm.t, depth, "",
            pi_left.get_size(), pi_right.get_size(), pi_left.get_size() + pi_right.get_size() - 1);

    {
        ASSERT_ALWAYS(pi_left.ab);
        ASSERT_ALWAYS(pi_right.ab);
        /* XXX should we pre-alloc ? We do that in the non-mpi case, but
         * that seems to be useless verbosity */
        lingen_call_companion & C = bm.companion(depth, z);
        bigmatpoly_ft<fft_type>::mul_caching(bm.stats, pi, pi_left, pi_right, &C.mul);
        ASSERT_ALWAYS(pi.ab);
        MPI_Barrier(bm.com[0]);
    }

    /* Note that the leading coefficients of pi_left and pi_right are not
     * necessarily full-rank, so that we have to fix potential zeros. If
     * we don't, the degree of pi artificially grows with the recursive
     * level.
     */
    unsigned int pisize = pi.get_size();
#if 1
    /* In fact, it's not entirely impossible that pi grows more than
     * what we had expected on entry, e.g. if we have one early
     * generator. So we can't just do this. Most of the time it will
     * work, but we can't claim that it will always work.
     *
     * One possible sign is when the entry deltas are somewhat even, and
     * the result deltas are unbalanced.
     */
    for(; pisize > pi_expect ; pisize--) {
        /* These coefficients really must be zero */
        ASSERT_ALWAYS(pi.coeff_is_zero(pisize - 1));
    }
    ASSERT_ALWAYS(pisize <= pi_expect);
#endif
    /* Now below pi_expect, it's not impossible to have a few
     * cancellations as well.
     */
    for(; pisize ; pisize--) {
        if (!pi.coeff_is_zero(pisize - 1)) break;
    }
    pi.set_size(pisize);
    ASSERT_ALWAYS(done || pisize >= pi_expect_lowerbound);

    logline_end(&bm.t_mul, "");

    return done;
}/*}}}*/

int bw_biglingen_collective(bmstatus & bm, bigmatpoly & pi, bigmatpoly & E)/*{{{*/
{
    /* as for bw_lingen_single, we're tempted to say that we're just a
     * trampoline. In fact, it's not really satisfactory: we're really
     * doing stuff here. In a sense though, it's not *that much* of a
     * trouble, because the mpi threshold will be low enough that doing
     * our full job here is not too much of a problem.
     */
    bw_dimensions & d = bm.d;
    abdst_field ab = d.ab;
    unsigned int m = d.m;
    unsigned int n = d.n;
    unsigned int b = m + n;
    int done;
    int rank;
    int size;
    MPI_Comm_rank(bm.com[0], &rank);
    MPI_Comm_size(bm.com[0], &size);
    unsigned int t0 = bm.t;
    unsigned int t1 = bm.t + E.get_size();

    lingen_call_companion C = bm.companion(bm.depth(), E.get_size());
    bool go_mpi = C.go_mpi;
    // bool go_mpi = E.get_size() >= bm.lingen_mpi_threshold;

    if (load_mpi_checkpoint_file(bm, pi, t0, t1, done))
        return done;

    // fprintf(stderr, "Enter %s\n", __func__);
    if (go_mpi) {
        typedef fft_transform_info fft_type;
        done = bw_biglingen_recursive<fft_type>(bm, pi, E);
    } else {
        /* Fall back to local code */
        /* This entails gathering E locally, computing pi locally, and
         * dispathing it back. */

        tree_stats::transition_sentinel dummy(bm.stats, "mpi_threshold", E.get_size(), C.total_ncalls);

        matpoly sE(ab, m, b, E.get_size());
        matpoly spi;

        double expect0 = bm.hints.tt_gather_per_unit * E.get_size();
        bm.stats.plan_smallstep("gather(L+R)", expect0);
        bm.stats.begin_smallstep("gather(L+R)");
        E.gather_mat(sE);
        bm.stats.end_smallstep();

        /* Only the master node does the local computation */
        if (!rank)
            done = bw_lingen_single(bm, spi, sE);

        double expect1 = bm.hints.tt_scatter_per_unit * E.get_size();
        bm.stats.plan_smallstep("scatter(L+R)", expect1);
        bm.stats.begin_smallstep("scatter(L+R)");
        pi = bigmatpoly(ab, E.get_model(), b, b, 0);
        pi.scatter_mat(spi);
        MPI_Bcast(&done, 1, MPI_INT, 0, bm.com[0]);
        MPI_Bcast(&bm.delta[0], b, MPI_UNSIGNED, 0, bm.com[0]);
        MPI_Bcast(&bm.lucky[0], b, MPI_UNSIGNED, 0, bm.com[0]);
        MPI_Bcast(&(bm.t), 1, MPI_UNSIGNED, 0, bm.com[0]);
        /* Don't forget to broadcast delta from root node to others ! */
        bm.stats.end_smallstep();
    }
    // fprintf(stderr, "Leave %s\n", __func__);

    save_mpi_checkpoint_file(bm, pi, t0, t1, done);

    MPI_Barrier(bm.com[0]);

    return done;
}/*}}}*/
#endif  /* ENABLE_MPI_LINGEN */

/*}}}*/

/**********************************************************************/

/**********************************************************************/
/* {{{ reading A and writing F ... */
struct bm_io {/*{{{*/
    bmstatus & bm;
    unsigned int t0 = 0;
    FILE ** fr = NULL; /* array of n files when split_input_file is supported
                   (which is not the case as of now),
                   or otherwise just a 1-element array */
    char * iobuf = NULL;
    const char * input_file = NULL;
    const char * output_file = NULL;
#ifdef SELECT_MPFQ_LAYER_u64k1
    static constexpr const unsigned int simd = ULONG_BITS;
#else
    static constexpr const unsigned int simd = 1;
#endif
    int ascii = 0;
    /* This is only a rolling window ! */
    matpoly A, F;
    unsigned int (*fdesc)[2] = NULL;
    /* This k is the coefficient in A(X) div X of the next coefficient to
     * be read. This is thus the total number of coefficients of A(X) div
     * X which have been read so far.
     * In writing mode, k is the number of coefficients of F which have
     * been written so far.
     */
    unsigned int next_coeff_to_fetch_from_source = 0;   // ROOT ONLY!
    unsigned int next_coeff_to_consume = 0;     // ROOT ONLY!

    unsigned int guessed_length = 0;

    bool leader() const {
        int rank;
        MPI_Comm_rank(bm.com[0], &rank);
        return rank == 0;
    }
    unsigned int set_write_behind_size();
    void zero1(unsigned int deg);
    unsigned int fetch_more_from_source(unsigned int io_window, unsigned int batch);
    bm_io(bm_io const&)=delete;
    bm_io(bmstatus & bm, const char * input_file, const char * output_file, int ascii);
    ~bm_io();
    void begin_read();
    void end_read();
    void guess_length();
    void compute_initial_F() ;

    template<class Consumer, class Sink>
        void compute_final_F(Sink & S, Consumer& pi);
    template<class Producer>
        void compute_E(Producer& E, unsigned int expected, unsigned int allocated);
    template<typename T, typename Sink>
        void output_flow(T & pi);
};
/*}}}*/

/* The reading mode of bm_io is streaming, but with a look-back
 * functionality: we want to be able to access coefficients a few places
 * earlier, so we keep them in memory.
 *
 * The writing mode has a write-ahead feature. Coefficients of the result
 * are written at various times, some earlier than others. The time span
 * is the same as for the reading mode.
 */


/* We write the coefficients of the reversed polynomial \hat{F*\pi}, in
 * increasing degree order. Thus the first coefficients written
 * corresponds to high degree coefficients in \pi.
 * This is mostly for historical reasons, since in fact, we would prefer
 * having the coefficients in the reverse order.
 */

/* let mindelta and maxdelta be (as their name suggests) the minimum and
 * maximum over all deltas corresponding to solution columns.
 *
 * For 0<=i<n, coeff i,j,k of pi becomes coeff i,j,k' of the final f,
 * with k'=k-(maxdelta-delta[j]).
 *
 * For 0<=i<m, coeff n+i,j,k of pi becomes coeff c[i],j,k' of the final f,
 * with k'=k-(maxdelta-delta[j])-(t0-e[j]).
 *
 * Therefore the maximum write-behind distance is (maxdelta-mindelta)+t0.
 * We need one coeff more (because offset goes from 0 to maxoffset).
 */
unsigned int bm_io::set_write_behind_size()/*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int mindelta, maxdelta;
    std::tie(mindelta, maxdelta) = bm.get_minmax_delta_on_solutions();
    unsigned int window = maxdelta - mindelta + t0 + 1;
    if (d.nrhs) {
        /* in sm-outside-matrix mode for DLP, we form the matrix F
         * slightly differently, as some *rows* are shifted out before
         * writing */
        window++;
    }
    window = simd * iceildiv(window, simd);
    if (leader()) {
        // F.realloc(window);
        F.zero_pad(window);
        // F.set_size(window);
    }
    return window;
}/*}}}*/

/* {{{ bm_output_* classes: the link from the in-memory structures to the
 * filesystem.
 * Note that coefficients are written transposed */
struct bm_output_singlefile {/*{{{*/
    bm_io & aa;
    matpoly const & P;
    std::ofstream f;
    char * iobuf;
    char * filename;
    bm_output_singlefile(bm_io &aa, matpoly const & P, const char * suffix = "")
        : aa(aa), P(P)
    {
        if (!aa.leader()) return;
        int rc = asprintf(&filename, "%s%s", aa.output_file, suffix);
        ASSERT_ALWAYS(rc >= 0);
        std::ios_base::openmode mode = std::ios_base::out;
        if (!aa.ascii) mode |= std::ios_base::binary;  
        f.open(filename, mode);
        DIE_ERRNO_DIAG(!f, "fopen", filename);
        iobuf = (char*) malloc(2 * io_block_size);
        f.rdbuf()->pubsetbuf(iobuf, 2 * io_block_size);
    }
    ~bm_output_singlefile()
    {
        if (!aa.leader()) return;
        printf("Saved %s\n", filename);
        free(filename);
        f.close();
        free(iobuf);
    }
    void write1(unsigned int deg)
    {
        if (!aa.leader()) return;
        deg = deg % P.capacity();
        matpoly_write(aa.bm.d.ab, f, P, deg, deg + 1, aa.ascii, 1);
    }
};/*}}}*/
struct bm_output_splitfile {/*{{{*/
    bm_io & aa;
    matpoly const & P;
    std::vector<std::ofstream> fw;
    bm_output_splitfile(bm_io & aa, matpoly const & P, const char * suffix = "")
        : aa(aa), P(P)
    {
        if (!aa.leader()) return;
        std::ios_base::openmode mode = std::ios_base::out;
        if (!aa.ascii) mode |= std::ios_base::binary;  
        for(unsigned int i = 0 ; i < P.m ; i++) {
            for(unsigned int j = 0 ; j < P.n ; j++) {
                char * str;
                int rc = asprintf(&str, "%s.sols%d-%d.%d-%d%s",
                        aa.output_file,
                        j, j + 1,
                        i, i + 1, suffix);
                ASSERT_ALWAYS(rc >= 0);
                fw.emplace_back(std::ofstream { str, mode } );
                DIE_ERRNO_DIAG(!fw.back(), "fopen", str);
                free(str);
            }
        }
        /* Do we want specific caching bufs here ? I doubt it */
        /*
           iobuf = (char*) malloc(2 * io_block_size);
           setbuffer(f, iobuf, 2 * io_block_size);
           */
    }
    ~bm_output_splitfile() {
    }
    void write1(unsigned int deg)
    {
        if (!aa.leader()) return;
        deg = deg % P.capacity();
        matpoly_write_split(aa.bm.d.ab, fw, P, deg, deg + 1, aa.ascii);
    }

};/*}}}*/
struct bm_output_checksum {/*{{{*/
    bm_io & aa;
    matpoly const & P;
    sha1_checksumming_stream f;
    const char * suffix;
    bm_output_checksum(bm_io & aa, matpoly const & P, const char * suffix = NULL)
        : aa(aa), P(P), suffix(suffix) { }
    ~bm_output_checksum() {
        char out[41];
        f.checksum(out);
        if (suffix)
            printf("checksum(%s): %s\n", suffix, out);
        else
            printf("checksum: %s\n", out);
    }
    void write1(unsigned int deg)
    {
        if (!aa.leader()) return;
        deg = deg % P.capacity();
        matpoly_write(aa.bm.d.ab, f, P, deg, deg + 1, aa.ascii, 1);
    }
};/*}}}*/
/* }}} */

void bm_io::zero1(unsigned int deg)/*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int n = d.n;
    deg = deg % F.capacity();
    for(unsigned int j = 0 ; j < n ; j++) {
        for(unsigned int i = 0 ; i < n ; i++) {
#ifndef SELECT_MPFQ_LAYER_u64k1
            abset_zero(d.ab, F.coeff(i, j, deg));
#else
            F.coeff_accessor(i, j, deg) += F.coeff(i, j, deg);
#endif
        }
    }
}/*}}}*/

/* Transfers of matrix entries, from memory to memory ; this is possibly
 * almost a transparent layer, and the matpoly_* instances really _are_
 * transparent layers. However we want a unified interface for the case
 * where we read an MPI-scattered matrix by chunks.
 *
 * Given that we are talking memory-to-memory, the classes below are used
 * as follows. The *_write_task classes are at the beginning of the
 * computation, where the matrix A is read from disk, and the matrix E is
 * written to (possibly distributed) memory. The one which matters is E
 * here. Symmetrically, the *_read_task are for reading the computed
 * matrix pi from (possibly distributed) memory, and writing the final
 * data F to disk.
 *
 * Pay attention to the fact that the calls to these structures are
 * collective calls.
 */
#ifdef ENABLE_MPI_LINGEN
class bigmatpoly_consumer_task { /* {{{ */
    /* This reads a bigmatpoly, by chunks, so that the memory footprint
     * remains reasonable. */
    size_t simd;
    bigmatpoly const & xpi;
    matpoly pi; /* This is only temp storage ! */
    unsigned int B;
    unsigned int k0;
    int rank;

    public:
    bigmatpoly_consumer_task(bm_io & aa, bigmatpoly const & xpi) : simd(aa.simd), xpi(xpi) {
        bmstatus & bm = aa.bm;
        bw_dimensions & d = bm.d;
        unsigned int m = d.m;
        unsigned int n = d.n;
        unsigned int b = m + n;
        MPI_Comm_rank(bm.com[0], &rank);

        /* Decide on the temp storage size */
        double avg = avg_matsize(d.ab, n, n, aa.ascii);
        B = iceildiv(io_block_size, avg);
        B = simd * iceildiv(B, simd);
        if (!rank && !random_input_length) {
            printf("Writing F to %s\n", aa.output_file);
            printf("Writing F by blocks of %u coefficients"
                    " (%.1f bytes each)\n", B, avg);
        }
        pi = matpoly(d.ab, b, b, B);

        k0 = UINT_MAX;
    }

    inline unsigned int chunk_size() const { return B; }

    inline unsigned int size() { return xpi.get_size(); }

    /* locked means that we don't want to change the current access window */
    inline absrc_elt coeff_const_locked(unsigned int i, unsigned int j, unsigned int k) {
        ASSERT(!rank);
        ASSERT(k0 != UINT_MAX && k - k0 < B);
        /* Note that only the const variant is ok to use in the binary case */
        return pi.coeff(i, j, k - k0);
    }

    /* Here, we adjust the access window so as to access coefficient k */
    absrc_elt coeff_const(unsigned int i, unsigned int j, unsigned int k) {
        if (k0 == UINT_MAX || k - k0 >= B) {
            k0 = k - (k % B);
            pi.zero();
            pi.set_size(B);
            xpi.gather_mat_partial(pi, k0, MIN(xpi.get_size() - k0, B));
        }
        if (rank) return NULL;
        return coeff_const_locked(i, j, k);
    }
};      /* }}} */
class bigmatpoly_producer_task { /* {{{ */
    /* This writes a bigmatpoly, by chunks, so that the memory footprint
     * remains reasonable.
     * Note that in any case, the coefficient indices must be progressive
     * in the write.
     */
    size_t simd;
    bigmatpoly & xE;
    matpoly E; /* This is only temp storage ! */
    unsigned int B;
    unsigned int k0;
    int rank;

    /* forbid copies */
    bigmatpoly_producer_task(bigmatpoly_producer_task const &) = delete;

    public:
    bigmatpoly_producer_task(bm_io & aa, bigmatpoly & xE) : simd(aa.simd), xE(xE) {
        bmstatus & bm = aa.bm;
        bw_dimensions & d = bm.d;
        unsigned int m = d.m;
        unsigned int n = d.n;
        unsigned int b = m + n;
        MPI_Comm_rank(aa.bm.com[0], &rank);


        /* Decide on the MPI chunk size */
        double avg = avg_matsize(d.ab, m, n, 0);
        B = iceildiv(io_block_size, avg);
        B = simd * iceildiv(B, simd);

        if (!rank) {
            /* TODO: move out of here */
            if (aa.input_file)
                printf("Reading A from %s\n", aa.input_file);
            printf("Computing E by chunks of %u coefficients (%.1f bytes each)\n",
                    B, avg);
        }

        E = matpoly(d.ab, m, b, B);

        /* Setting E.size is rather artificial, since we use E essentially
         * as an area where we may write coefficients freely. The only aim is
         * to escape some safety checks involving ->size in matpoly_part */
        E.zero();

        k0 = UINT_MAX;
    }

    inline unsigned int chunk_size() const { return B; }

    // inline unsigned int size() { return xE.size; }
    inline void set_size(unsigned int s) {
        xE.set_size(s);
    }

    inline abdst_elt coeff_locked(unsigned int i, unsigned int j, unsigned int k) {
        ASSERT(k0 != UINT_MAX && k - k0 < B);
        ASSERT(!rank);

        E.set_size(E.get_size() + (E.get_size() == k - k0));
        ASSERT(E.get_size() == k - k0 + 1);

        return E.coeff(i, j, k - k0);
    }

    abdst_elt coeff(unsigned int i, unsigned int j, unsigned int k) {
        if (k0 == UINT_MAX) {
            E.zero();
            E.set_size(0);
            ASSERT(k == 0);
            k0 = 0;
        } else {
            if (k >= (k0 + B)) {
                /* We require progressive reads */
                ASSERT(k == k0 + B);
                xE.scatter_mat_partial(E, k0, B);
                E.zero();
                k0 += B;
            }
        }
        ASSERT(k0 != UINT_MAX && k - k0 < B);
        if (rank) return NULL;
        return coeff_locked(i, j, k);
    }

    void finalize(unsigned int length) {
        if (length > k0) {
            /* Probably the last chunk hasn't been dispatched yet */
            ASSERT(length < k0 + B);
            xE.scatter_mat_partial(E, k0, length - k0);
        }
        set_size(length);
    }
    friend void matpoly_extract_column(
        bigmatpoly_producer_task& dst, unsigned int jdst, unsigned int kdst,
        matpoly & src, unsigned int jsrc, unsigned int ksrc);
};

void matpoly_extract_column(
        bigmatpoly_producer_task& dst, unsigned int jdst, unsigned int kdst,
        matpoly & src, unsigned int jsrc, unsigned int ksrc)
{
    dst.coeff_locked(0, jdst, kdst);
    dst.E.extract_column(jdst, kdst - dst.k0, src, jsrc, ksrc);
}

/* }}} */
#endif  /* ENABLE_MPI_LINGEN */
class matpoly_consumer_task {/*{{{*/
    /* This does the same, but for a simple matpoly. Of course this is
     * much simpler ! */
    matpoly const & pi;

    public:
    matpoly_consumer_task(bm_io & aa, matpoly const & pi) :
            pi(pi)
    {
        if (!random_input_length) {
            printf("Writing F to %s\n", aa.output_file);
        }
    }

    inline unsigned int chunk_size() const { return 1; }

    inline unsigned int size() { return pi.get_size(); }

    absrc_elt coeff_const_locked(unsigned int i, unsigned int j, unsigned int k) {
        return pi.coeff(i, j, k);
    }
    inline absrc_elt coeff_const(unsigned int i, unsigned int j, unsigned int k) {
        return coeff_const_locked(i, j, k);
    }

    ~matpoly_consumer_task() { }
};/*}}}*/
class matpoly_producer_task { /* {{{ */
    size_t simd;
    matpoly & E;

    /* forbid copies */
    matpoly_producer_task(matpoly_producer_task const &) = delete;

    public:
    matpoly_producer_task(bm_io & aa, matpoly & E) : simd(aa.simd), E(E)
    {
        /* TODO: move out of here */
        if (aa.input_file)
            printf("Reading A from %s\n", aa.input_file);
    }

    inline unsigned int chunk_size() const { return simd; }
    inline void set_size(unsigned int s) {
        E.set_size(s);
    }
    // inline unsigned int size() { return E.size; }

#if 0
    abdst_elt coeff_locked(unsigned int i, unsigned int j, unsigned int k) {
        E.size += (E.size == k);
        ASSERT(E.size == k + 1);
        return E.coeff(i, j, k);
    }
    inline abdst_elt coeff(unsigned int i, unsigned int j, unsigned int k) {
        return coeff_locked(i, j, k);
    }
#endif
    void mark_coeff_as_read(unsigned int, unsigned int, unsigned int k) {
        E.set_size(E.get_size() + (E.get_size() == k));
        ASSERT(E.get_size() == k + 1);
    }
    inline absrc_elt coeff(unsigned int i, unsigned int j, unsigned int k) {
        mark_coeff_as_read(i, j, k);
        // note that for the binary case, this is a const accessor.
        return E.coeff(i, j, k);
    }
    void finalize(unsigned int length) { set_size(length); }
    ~matpoly_producer_task() { }
    friend void matpoly_extract_column(
        matpoly_producer_task& dst, unsigned int jdst, unsigned int kdst,
        matpoly & src, unsigned int jsrc, unsigned int ksrc);
};
void matpoly_extract_column(
        matpoly_producer_task& dst, unsigned int jdst, unsigned int kdst,
        matpoly & src, unsigned int jsrc, unsigned int ksrc)
{
    dst.E.extract_column(jdst, kdst, src, jsrc, ksrc);
}

/* }}} */

template<class Consumer, class Sink>
void bm_io::compute_final_F(Sink & S, Consumer& pi)/*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    abdst_field ab = d.ab;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    int leader = rank == 0;


    /* We are not interested by pi.size, but really by the number of
     * coefficients for the columns which give solutions. */
    unsigned int maxdelta = bm.get_max_delta_on_solutions();

    if (leader) printf("Final f(X)=f0(X)pi(X) has degree %u\n", maxdelta);

    unsigned int window = F.capacity();

    /* Which columns of F*pi will make the final generator ? */
    std::vector<unsigned int> sols(n, 0);
    for(unsigned int j = 0, jj=0 ; j < m + n ; j++) {
        if (bm.lucky[j] <= 0)
            continue;
        sols[jj++]=j;
    }

    double tt0 = wct_seconds();
    double next_report_t = tt0 + 10;
    unsigned next_report_s = pi.size() / 100;

    /*
     * first compute the rhscontribs. Use that to decide on a renumbering
     * of the columns, because we'd like to get the "primary" solutions
     * first, in a sense. Those are the ones with fewer zeroes in the
     * rhscontrib part. So we would want that matrix to have its columns
     * sorted in decreasing weight order
     *
     * An alternative, possibly easier, is to have a function which
     * decides the solution ordering precisely based on the inspection of
     * this rhscoeffs matrix (?). But how should we spell that info when
     * we give it to mksol ??
     */

    /* This **modifies** the "sols" array */
    if (d.nrhs) {
        /* The data is only gathered at rank 0. So the stuff we compute
         * can only be meaningful there. On the other hand, it is
         * necessary that we call *collectively* the pi.coeff() routine,
         * because we need synchronisation of all ranks.
         */

        matpoly rhs(ab, d.nrhs, n, 1);
        rhs.zero_pad(1);

        {
            Sink Srhs(*this, rhs, ".rhs");

            /* adding the contributions to the rhs matrix. */
            for(unsigned int ipi = 0 ; ipi < m + n ; ipi++) {
                for(unsigned int jF = 0 ; jF < n ; jF++) {
                    unsigned int jpi = sols[jF];
                    unsigned int iF, offset;
                    if (ipi < n) {
                        iF = ipi;
                        offset = 0;
                    } else {
                        iF = fdesc[ipi-n][1];
                        offset = t0 - fdesc[ipi-n][0];
                    }
                    if (iF >= d.nrhs) continue;
                    ASSERT_ALWAYS(bm.delta[jpi] >= offset);
                    unsigned kpi = bm.delta[jpi] - offset;

                    ASSERT_ALWAYS(d.nrhs);
                    ASSERT_ALWAYS(iF < d.nrhs);
                    ASSERT_ALWAYS(jF < n);
                    absrc_elt src = pi.coeff_const(ipi, jpi, kpi);

                    if (leader) {
                        rhs.coeff_accessor(iF, jF, 0) += src;
                    }
                }
            }

            if (leader) {
                /* Now comes the time to prioritize the different solutions. Our
                 * goal is to get the unessential solutions last ! */
                std::vector<std::array<int, 2>> sol_score(n, {{0, 0}});
                /* score per solution is the number of non-zero coefficients,
                 * that's it. Since we have access to lexcmp2, we want to use it.
                 * Therefore, desiring the highest scoring solutions first, we
                 * negate the hamming weight.
                 */
                for(unsigned int jF = 0 ; jF < n ; jF++) {
                    sol_score[jF][1] = jF;
                    for(unsigned int iF = 0 ; iF < d.nrhs ; iF++) {
                        int z = !abis_zero(ab, rhs.coeff(iF, jF, 0));
                        sol_score[jF][0] -= z;
                    }
                }
                std::sort(sol_score.begin(), sol_score.end());

                if (leader) {
                    printf("Reordered solutions:\n");
                    for(unsigned int i = 0 ; i < n ; i++) {
                        printf(" %d (col %d in pi, weight %d on rhs vectors)\n", sol_score[i][1], sols[sol_score[i][1]], -sol_score[i][0]);
                    }
                }

                /* We'll now modify the sols[] array, so that we get a reordered
                 * F, too (and mksol/gather don't have to care about our little
                 * tricks */
                {
                    matpoly rhs2(ab, d.nrhs, n, 1);
                    rhs2.zero_pad(1);
                    for(unsigned int i = 0 ; i < n ; i++) {
                        rhs2.extract_column(i, 0, rhs, sol_score[i][1], 0);
                    }
                    rhs = std::move(rhs2);
                    if (sol_score[0][0] == 0) {
                        if (allow_zero_on_rhs) {
                            printf("Note: all solutions have zero contribution on the RHS vectors -- we will just output right kernel vectors (ok because of allow_zero_on_rhs=1)\n");
                        } else {
                            fprintf(stderr, "ERROR: all solutions have zero contribution on the RHS vectors -- we will just output right kernel vectors (maybe use allow_zero_on_rhs ?)\n");
                            rank0_exit_code = EXIT_FAILURE;
                        }
                    }
                    /* ugly: use sol_score[i][0] now to provide the future
                     * "sols" array. We'll get rid of sol_score right afterwards
                     * anyway.
                     */
                    for(unsigned int i = 0 ; i < n ; i++) {
                        sol_score[i][0] = sols[sol_score[i][1]];
                    }
                    for(unsigned int i = 0 ; i < n ; i++) {
                        sols[i] = sol_score[i][0];
                    }
                }
                Srhs.write1(0);
            }
        }
    }

    /* we need to read pi backwards. The number of coefficients in pi is
     * pilen = maxdelta + 1 - t0. Hence the first interesting index is
     * maxdelta - t0. However, for notational ease, we'll access
     * coefficients from index pi.size downwards. The latter is always
     * large enough.
     */

    ASSERT_ALWAYS(pi.size() >= maxdelta + 1 - t0);

    for(unsigned int s = 0 ; s < pi.size() ; s++) {
        unsigned int kpi = pi.size() - 1 - s;

        /* as above, we can't just have ranks > 0 do nothing. We need
         * synchronization of the calls to pi.coeff()
         */

        /* This call is here only to trigger the gather call. This one
         * must therefore be a collective call. Afterwards we'll use a
         * _locked call which is okay to call only at rank 0.
         */
        pi.coeff_const(0, 0, kpi);

        if (rank) continue;

        /* Coefficient kpi + window of F has been totally computed,
         * because of previous runs of this loop (which reads the
         * coefficients of pi).
         */
        if (kpi + window == maxdelta) {
            /* Avoid writing zero coefficients. This can occur !
             * Example:
             * tt=(2*4*1200) mod 1009, a = (tt cat tt cat * tt[0..10])
             */
            for(unsigned int j = 0 ; j < n ; j++) {
                int z = 1;
                for(unsigned int i = 0 ; z && i < n ; i++) {
                    absrc_elt src = F.coeff(i, j, 0);
                    z = abis_zero(ab, src);
                }

                if (z) {
                    /* This is a bit ugly. Given that we're going to
                     * shift one column of F, we'll have a
                     * potentially deeper write-back buffer. Columns
                     * which seemed to be ready still are, but they
                     * will now be said so only at the next step.
                     */
                    printf("Reduced solution column #%u from"
                            " delta=%u to delta=%u\n",
                            sols[j], bm.delta[sols[j]], bm.delta[sols[j]]-1);
                    window++;
                    F.realloc(window);
                    F.set_size(window);
                    bm.delta[sols[j]]--;
                    /* shift this column */
                    for(unsigned int k = 1 ; k < window ; k++) {
                        F.extract_column(j, k-1, F, j, k);
                    }
                    F.zero_column(j, window - 1);
                    break;
                }
            }
        }
        /* coefficient of degree maxdelta-kpi-window is now complete */
        if (kpi + window <= maxdelta) {
            S.write1((maxdelta-kpi) - window);
            zero1((maxdelta-kpi) - window);
        }
        /* Now use our coefficient. This might tinker with
         * coefficients up to degree kpi-(window-1) in the file F */

        if (kpi > maxdelta + t0 ) {
            /* this implies that we always have kF > delta[jpi],
             * whence we expect a zero contribution */
            for(unsigned int i = 0 ; i < m + n ; i++) {
                for(unsigned int jF = 0 ; jF < n ; jF++) {
                    unsigned int jpi = sols[jF];
                    absrc_elt src = pi.coeff_const_locked(i, jpi, kpi);
                    ASSERT_ALWAYS(abis_zero(ab, src));
                }
            }
            continue;
        }

        for(unsigned int ipi = 0 ; ipi < m + n ; ipi++) {
            for(unsigned int jF = 0 ; jF < n ; jF++) {
                unsigned int jpi = sols[jF];
                unsigned int iF, offset;
                if (ipi < n) {
                    /* Left part of the initial F is x^0 times
                     * identity. Therefore, the first n rows of pi
                     * get multiplied by this identity matrix, this
                     * is pretty simple.
                     */
                    iF = ipi;
                    offset = 0;
                } else {
                    /* next m rows of the initial F are of the form
                     * x^(some value) times some canonical basis
                     * vector. Therefore, the corresponding row in pi
                     * ends up contributing to some precise row in F,
                     * and with an offset which is dictated by the
                     * exponent of x.
                     */
                    iF = fdesc[ipi-n][1];
                    offset = t0 - fdesc[ipi-n][0];
                }
                unsigned int subtract = maxdelta - bm.delta[jpi] + offset;
                ASSERT(subtract < window);
                if (maxdelta < kpi + subtract) continue;
                unsigned int kF = (maxdelta - kpi) - subtract;
                unsigned int kF1 = kF - (iF < d.nrhs);
                if (kF1 == UINT_MAX) {
                    /* this has been addressed in the first pass,
                     * earlier.
                     */
                    continue;
                }
                absrc_elt src = pi.coeff_const_locked(ipi, jpi, kpi);
                ASSERT_ALWAYS(kF <= bm.delta[jpi] || abis_zero(ab, src));
                F.coeff_accessor(iF, jF, kF1 % window) += src;
            }
        }

        if (leader && s > next_report_s) {
            double tt = wct_seconds();
            if (tt > next_report_t) {
                printf( "Written %u coefficients (%.1f%%) in %.1f s\n",
                        s, 100.0 * s / pi.size(), tt-tt0);
                next_report_t = tt + 10;
                next_report_s = s + pi.size() / 100;
            }
        }
    }
    /* flush the pipe */
    if (leader && window <= maxdelta) {
        for(unsigned int s = window ; s-- > 0 ; )
            S.write1(maxdelta - s);
    }
}/*}}}*/

/* read 1 (or (batch)) coefficient(s) into the sliding window of input
 * coefficients of the input series A. The io_window parameter controls
 * the size of the sliding window. There are in fact two behaviours:
 *  - io_window == 0: there is no sliding window, really, and the new
 *    coefficient is appended as the last coefficient of A.
 *  - io_window > 0: there, we really have a sliding window. Coeffs
 *    occupy places in a circular fashion within the buffer.
 */
unsigned int bm_io::fetch_more_from_source(unsigned int io_window, unsigned int batch)/*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    static unsigned int generated_random_coefficients = 0;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    ASSERT_ALWAYS(rank == 0);

    unsigned int pos = next_coeff_to_fetch_from_source;
    ASSERT_ALWAYS(A.get_size() % simd == 0);
    ASSERT_ALWAYS(next_coeff_to_fetch_from_source % simd == 0);
    ASSERT_ALWAYS(batch % simd == 0);
    ASSERT_ALWAYS(n % simd == 0);
    if (io_window) {
        pos = pos % io_window;
        ASSERT_ALWAYS(A.get_size() == io_window);
        ASSERT_ALWAYS(pos / io_window == (pos + batch - 1) / io_window);
    } else {
        ASSERT_ALWAYS(A.get_size() == next_coeff_to_fetch_from_source);
        if (next_coeff_to_fetch_from_source >= A.capacity())
            A.realloc(A.capacity() + batch);
        ASSERT_ALWAYS(next_coeff_to_fetch_from_source < A.capacity());
        A.set_size(A.get_size() + batch);
    }
    if (random_input_length) {
        if (generated_random_coefficients >= random_input_length)
            return 0;

        for (unsigned int i = 0; i < m ; i++) {
            for (unsigned int j = 0; j < n ; j++) {
                for (unsigned int b = 0; b < batch ; b += simd) {
#ifdef SELECT_MPFQ_LAYER_u64k1
                    unsigned int sq = (pos + b) / simd;
                    A.coeff(i, j)[sq] = gmp_urandomb_ui(rstate, simd);
#else
                    abrandom(d.ab, A.coeff(i, j, pos + b), rstate);
#endif
                }
            }
        }
        generated_random_coefficients += batch;
        next_coeff_to_fetch_from_source += batch;
        return batch;
    }

    for (unsigned int i = 0; i < m ; i++) {
        for (unsigned int j = 0; j < n ; j++) {
#ifdef SELECT_MPFQ_LAYER_u64k1
            memset(A.part(i, j) + (pos / simd), 0, abvec_elt_stride(d.ab, batch));
#else
            memset(A.coeff(i, j, pos), 0, abvec_elt_stride(d.ab, batch));
#endif
        }
    }

    for (unsigned int b = 0; b < batch ; b++ ) {
        for (unsigned int i = 0; i < m ; i++) {
            for (unsigned int j = 0; j < n ; j+= simd) {
                int rc;
#ifdef SELECT_MPFQ_LAYER_u64k1
                if (ascii)
                    abort();
                unsigned long data = 0;
                rc = fread(&data, sizeof(unsigned long), 1, fr[0]);
                rc = rc == 1;
                unsigned int sq = (pos + b) / simd;
                unsigned int sr = b % simd;
                for(unsigned int jr = 0 ; jr < simd ; jr++) {
                    unsigned long bit = (data & (1UL << jr)) != 0;
                    A.coeff(i, j + jr)[sq] ^= bit << sr;
                }
#else
                abdst_elt x = A.coeff(i, j, pos + b);
                if (ascii) {
                    rc = abfscan(d.ab, fr[0], x);
                    /* rc is the number of bytes read -- non-zero on success */
                } else {
                    size_t elemsize = abvec_elt_stride(d.ab, 1);
                    rc = fread(x, elemsize, 1, fr[0]);
                    rc = rc == 1;
                    abnormalize(d.ab, x);
                }
#endif
                if (!rc) {
                    if (i == 0 && j == 0) {
                        next_coeff_to_fetch_from_source += b;
                        return b;
                    }
                    fprintf(stderr,
                            "Parse error while reading coefficient (%d,%d,%d)%s\n",
                            i, j, 1 + next_coeff_to_fetch_from_source,
                            ascii ? "" : " (forgot --ascii?)");
                    exit(EXIT_FAILURE);
                }
            }
        }
    }
    next_coeff_to_fetch_from_source += batch;
    return batch;
}/*}}}*/

bm_io::bm_io(bmstatus & bm, const char * input_file, const char * output_file, int ascii)/*{{{*/
    : bm(bm)
    , input_file(input_file)
    , output_file(output_file)
    , ascii(ascii)
    , A(bm.d.ab, bm.d.m, bm.d.n, 1)
{
}/*}}}*/

bm_io::~bm_io()/*{{{*/
{
    if (fdesc) free(fdesc);
}/*}}}*/

void bm_io::begin_read()/*{{{*/
{
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    if (rank) return;

    if (random_input_length) {
        /* see below. I think it would be a bug to not do that */
        fetch_more_from_source(0, simd);
        next_coeff_to_consume++;
        return;
    }

    if (split_input_file) {
        fprintf(stderr, "--split-input-file not supported yet\n");
        exit(EXIT_FAILURE);
    }
    fr = (FILE**) malloc(sizeof(FILE*));
    fr[0] = fopen(input_file, ascii ? "r" : "rb");

    DIE_ERRNO_DIAG(fr[0] == NULL, "fopen", input_file);
    iobuf = (char*) malloc(2 * io_block_size);
    setbuffer(fr[0], iobuf, 2 * io_block_size);

    /* read the first coefficient ahead of time. This is because in most
     * cases, we'll discard it. Only in the DL case, we will consider the
     * first coefficient as being part of the series. Which means that
     * the coefficient reads in the I/O loop will sometimes correspond to
     * the coefficient needed at that point in time, while we will also
     * (in the DL case) need data from the previous read.
     */
    if (fetch_more_from_source(0, simd) < simd) {
        fprintf(stderr, "Read error from %s\n", input_file);
        exit(EXIT_FAILURE);
    }
    next_coeff_to_consume++;
}/*}}}*/

void bm_io::end_read()/*{{{*/
{
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);
    if (rank) return;
    if (random_input_length) return;
    fclose(fr[0]);
    free(fr);
    fr = NULL;
    free(iobuf);
    iobuf = 0;
}/*}}}*/

void bm_io::guess_length()/*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    abdst_field ab = d.ab;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);

    if (random_input_length) {
        guessed_length = random_input_length;
        return;
    }

    if (!rank) {
        struct stat sbuf[1];
        int rc = fstat(fileno(fr[0]), sbuf);
        if (rc < 0) {
            perror(input_file);
            exit(EXIT_FAILURE);
        }

        size_t filesize = sbuf->st_size;

        if (!ascii) {
            size_t avg = avg_matsize(ab, m, n, ascii);
            if (filesize % avg) {
                fprintf(stderr, "File %s has %zu bytes, while its size should be amultiple of %zu bytes (assuming binary input; perhaps --ascii is missing ?).\n", input_file, filesize, avg);
                exit(EXIT_FAILURE);
            }
            guessed_length = filesize / avg;
        } else {
            double avg = avg_matsize(ab, m, n, ascii);
            double expected_length = filesize / avg;
            if (!rank)
                printf("# Expect roughly %.2f items in the sequence.\n", expected_length);

            /* First coefficient is always lighter, so we add a +1. */
            guessed_length = 1 + expected_length;
        }
    }
    MPI_Bcast(&(guessed_length), 1, MPI_UNSIGNED, 0, bm.com[0]);
}/*}}}*/

void bm_io::compute_initial_F() /*{{{ */
{
    bw_dimensions & d = bm.d;
    abdst_field ab = d.ab;
    unsigned int m = d.m;
    unsigned int n = d.n;
    int rank;
    fdesc = (unsigned int(*)[2])malloc(2 * m * sizeof(unsigned int));
    MPI_Comm_rank(bm.com[0], &rank);
    if (!rank) {
        /* read the first few coefficients. Expand A accordingly as we are
         * doing the read */

        ASSERT(A.m == m);
        ASSERT(A.n == n);

        abelt tmp MAYBE_UNUSED;
        abinit(ab, &tmp);

        /* First try to create the initial F matrix */
        printf("Computing t0\n");

        /* We want to create a full rank m*m matrix M, by extracting columns
         * from the first coefficients of A */

        matpoly M(ab, m, m, 1);
        M.zero_pad(1);

        /* For each integer i between 0 and m-1, we have a column, picked
         * from column cnum[i] of coeff exponent[i] of A which, once reduced modulo
         * the other ones, has coefficient at row pivots[i] unequal to zero.
         */
        std::vector<unsigned int> pivots(m, 0);
        std::vector<unsigned int> exponents(m, 0);
        std::vector<unsigned int> cnum(m, 0);
        unsigned int r = 0;

        for (unsigned int k = 1; r < m ; k++) {
            /*
             * k is the candidate for becoming the value t0.
             *
             * coefficient of degree t0 of the initial A*F is in fact
             * coefficient of degree t0 of A'*F, where column j of A' is
             * column j of A, divided by X if j >= bm.d.nrhs.
             *
             * (recall that 0<=bm.d.nrhs<=n)
             *
             * Therefore, for j >= bm.d.nrhs, the contribution to
             * coefficient of degree k (tentative t0) of A'*F comes from
             * the following data from A:
             *  for cols of A with j < bm.d.rhs:  coeffs 0 <= deg <= k-1 
             *  for cols of A with j >= bm.d.rhs: coeffs 1 <= deg <= k
             *
             * This means that here, we're going to read data from the
             * following coefficient of A
             *  k   if bm.d.nrhs < n
             *  k-1 if bm.d.nrhs = n
             */
            unsigned int k_access = k - (bm.d.nrhs == n);

            if (rank == 0) {
                if (k_access >= next_coeff_to_consume)
                    next_coeff_to_consume++;
                ASSERT_ALWAYS(k_access < next_coeff_to_consume);
                if (k_access >= next_coeff_to_fetch_from_source) {
                    /* read a new coefficient into A */
                    fetch_more_from_source(0, simd);
                }
                ASSERT_ALWAYS(k_access <= next_coeff_to_fetch_from_source);
            }

            for (unsigned int j = 0; r < m && j < n; j++) {
                /* Extract a full column into M (column j, degree k in A) */
                /* adjust the coefficient degree to take into account the
                 * fact that for SM columns, we might in fact be
                 * interested by the _previous_ coefficient */
                M.extract_column(r, 0, A, j, k - (j < bm.d.nrhs));

                /* Now reduce it modulo all other columns */
                for (unsigned int v = 0; v < r; v++) {
                    unsigned int u = pivots[v];
                    /* the v-th column in the M is known to
                     * kill coefficient u (more exactly, to have a -1 as u-th
                     * coefficient, and zeroes for the other coefficients
                     * referenced in the pivots[0] to pivots[v-1] indices).
                     */
                    /* add M[u,r]*column v of M to column r of M */
                    for(unsigned int i = 0 ; i < m ; i++) {
#ifndef SELECT_MPFQ_LAYER_u64k1
                        abmul(ab, tmp, M.coeff(i, v, 0), M.coeff(u, r, 0));
                        abadd(ab, M.coeff(i, r, 0), M.coeff(i, r, 0), tmp);
#else
                        if (i == u) continue;
                        abelt x = { M.coeff(i, v, 0)[0] & M.coeff(u, r, 0)[0] };
                        M.coeff_accessor(i, r, 0) += x;
#endif
                    }
#ifndef SELECT_MPFQ_LAYER_u64k1
                    abset_zero(ab, M.coeff(u, r, 0));
#endif
                }
                unsigned int u = 0;
                for( ; u < m ; u++) {
                    if (!abis_zero(ab, M.coeff(u, r, 0)))
                        break;
                }
                if (u == m) {
                    printf("[X^%d] A, col %d does not increase rank (still %d)\n",
                           k - (j < bm.d.nrhs), j, r);

                    /* we need at least m columns to get as starting matrix
                     * with full rank. Given that we have n columns per
                     * coefficient, this means at least m/n matrices.
                     */

                    if (k * n > m + 40) {
                        printf("The choice of starting vectors was bad. "
                               "Cannot find %u independent cols within A\n", m);
                        exit(EXIT_FAILURE);
                    }
                    continue;
                }

                /* Bingo, it's a new independent col. */
                pivots[r] = u;
                cnum[r] = j;
                exponents[r] = k - 1;

                /* Multiply the column so that the pivot becomes -1 */
#ifndef SELECT_MPFQ_LAYER_u64k1
                /* this is all trivial in characteristic two, of course
                 */
                int rc = abinv(ab, tmp, M.coeff(u, r, 0));
                if (!rc) {
                    fprintf(stderr, "Error, found a factor of the modulus: ");
                    abfprint(ab, stderr, tmp);
                    fprintf(stderr, "\n");
                    exit(EXIT_FAILURE);
                }
                abneg(ab, tmp, tmp);
                for(unsigned int i = 0 ; i < m ; i++) {
                    abmul(ab, M.coeff(i, r, 0),
                              M.coeff(i, r, 0),
                              tmp);
                }
#endif

                r++;

                // if (r == m)
                    printf
                        ("[X^%d] A, col %d increases rank to %d (head row %d)%s\n",
                         k - (j < bm.d.nrhs), j, r, u,
                         (j < bm.d.nrhs) ? " (column not shifted because of the RHS)":"");
            }
        }

        if (r != m) {
            printf("This amount of data is insufficient. "
                   "Cannot find %u independent cols within A\n", m);
            exit(EXIT_FAILURE);
        }

        /* t0 is the k value for the loop index when we exited the loop.
         */
        t0 = exponents[r - 1] + 1;

        /* Coefficients of degree up to t0-1 of A' are read. Unless
         * bm.d.nrhs == n, for at least one of the columns of A, this
         * means up to degree t0.
         */
        if (rank == 0)
            ASSERT_ALWAYS(bm_io::next_coeff_to_consume == t0 + (bm.d.nrhs < n));

        printf("Found satisfactory init data for t0=%d\n", t0);

        /* We've also got some adjustments to make: room for one extra
         * coefficient is needed in A. Reading of further coefficients will
         * pickup where they stopped, and will always leave the last
         * t0+1+simd coefficients readable. */
        unsigned int window = simd + simd * iceildiv(t0 + 1, simd);
        A.realloc(window);
        A.zero_pad(window);

        for(unsigned int j = 0 ; j < m ; j++) {
            fdesc[j][0] = exponents[j];
            fdesc[j][1] = cnum[j];
            ASSERT_ALWAYS(exponents[j] < t0);
        }
        // free(pivots);
        // free(exponents);
        // free(cnum);
        // matpoly_clear(ab, M);
        abclear(ab, &tmp);
    }
    MPI_Bcast(fdesc, 2*m, MPI_UNSIGNED, 0, bm.com[0]);
    MPI_Bcast(&(t0), 1, MPI_UNSIGNED, 0, bm.com[0]);
    bm.set_t0(t0);
}				/*}}} */

template<class Writer>
void bm_io::compute_E(Writer& E, unsigned int expected, unsigned int allocated)/*{{{*/
{
    // F0 is exactly the n x n identity matrix, plus the
    // X^(s-exponent)e_{cnum} vectors. fdesc has the (exponent, cnum)
    // pairs
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    abdst_field ab = d.ab;
    int rank;
    MPI_Comm_rank(bm.com[0], &rank);

    unsigned int window = simd + simd * iceildiv(t0 + 1, simd);

    double tt0 = wct_seconds();
    double next_report_t = tt0 + 10;
    unsigned next_report_k = expected / 100;

    int over = 0;
    unsigned int final_length = allocated - t0;

    /* For the moment, despite the simd nature of the data, we'll compute
     * E one coeff at a time.
     */
    for(unsigned int kE = 0 ; kE + t0 < allocated ; kE++) {
        /* See the discussion in compute_initial_F ; to form coefficient
         * of degree kE of E, which is coefficient of degree t0+kE of
         * A'*F, we need to access the following coefficient of A:
         *
         *      t0+kE+1   if bm.d.nrhs < n
         *      t0+kE     if bm.d.nrhs = n
         *
         * more specifically, multiplying by row j of F will access
         * column j of A, and only coefficients of deg <= t0+kE+(j>=bm.d.nrhs)
         */
        
        unsigned int k_access = kE + t0 + (bm.d.nrhs < n);

        if (k_access % E.chunk_size() == 0 || kE + t0 >= expected) {
            /* check periodically */
            MPI_Bcast(&over, 1, MPI_INT, 0, bm.com[0]);
            if (over) break;
        }

        if (rank == 0) {
            if (!over && k_access >= next_coeff_to_fetch_from_source) {
                unsigned int b = fetch_more_from_source(window, simd);
                over = b < simd;
                if (over) {
                    printf("EOF met after reading %u coefficients\n", next_coeff_to_fetch_from_source);
                    final_length = kE + b;
                }
            }
            if (!over) {
                if (k_access >= next_coeff_to_consume) next_coeff_to_consume++;
                ASSERT_ALWAYS(k_access < next_coeff_to_consume);
                ASSERT_ALWAYS(k_access < next_coeff_to_fetch_from_source);
            }
        }

        /* This merely makes sure that coefficient E is writable: this
         * call may change the view window for E, and in
         * the case of an MPI run, this view window will be eventually
         * pushed to other nodes
         */
        E.coeff(0, 0, kE);

        if (rank || kE >= final_length)
            continue;

        if (kE + t0 > allocated) {
            fprintf(stderr, "Going way past guessed length%s ???\n", ascii ? " (more than 5%%)" : "");
        }

        for(unsigned int j = 0 ; j < n ; j++) {
            /* If the first columns of F are the identity matrix, then
             * in E we get data from coefficient kE+t0 in A', i.e.
             * coefficient of degree kE+t0+(j>=nrhs) in column j of A. More
             * generally, if it's x^q*identity, we read
             * coeficient of index kE + t0 + (j>=nrhs) - q.
             *
             * Note that we prefer to take q=0 anyway, since a
             * choice like q=t0 would create duplicate rows in E,
             * and that would be bad.
             */
            unsigned int kA = kE + t0 + (j >= bm.d.nrhs);
            ASSERT_ALWAYS(!rank || kA < next_coeff_to_consume);
            matpoly_extract_column(E, j, kE, A, j, kA % window);
        }

        for(unsigned int jE = n ; jE < m + n ; jE++) {
            unsigned int jA = fdesc[jE-n][1];
            unsigned int offset = fdesc[jE-n][0];
            unsigned int kA = kE + offset + (jA >= bm.d.nrhs);
            ASSERT_ALWAYS(!rank || kA < next_coeff_to_consume);
            matpoly_extract_column(E, jE, kE, A, jA, kA % window);
        }
        ASSERT_ALWAYS(!rank || k_access + 1 ==  next_coeff_to_consume);
        if (k_access > next_report_k) {
            double tt = wct_seconds();
            if (tt > next_report_t) {
                printf(
                        "Read %u coefficients (%.1f%%)"
                        " in %.1f s (%.1f MB/s)\n",
                        k_access, 100.0 * k_access / expected,
                        tt-tt0, k_access * avg_matsize(ab, m, n, ascii) / (tt-tt0)/1.0e6);
                next_report_t = tt + 10;
                next_report_k = k_access + expected / 100;
            }
        }
    }
    MPI_Bcast(&final_length, 1, MPI_UNSIGNED, 0, bm.com[0]);
    E.finalize(final_length);
}/*}}}*/

template<typename T> struct matpoly_factory {};

#ifdef ENABLE_MPI_LINGEN
template<> struct matpoly_factory<bigmatpoly> {
    typedef bigmatpoly T;
    typedef bigmatpoly_producer_task producer_task;
    typedef bigmatpoly_consumer_task consumer_task;
    bigmatpoly_model model;
    matpoly_factory(MPI_Comm * comm, unsigned int m, unsigned int n) : model(comm, m, n) {}
    T init(abdst_field ab, unsigned int m, unsigned int n, int len) {
        return bigmatpoly(ab, model, m, n, len);
    }
    static int bw_lingen(bmstatus & bm, T & pi, T & E) {
        return bw_biglingen_collective(bm, pi, E);
    }
    static size_t capacity(T const & p) { return p.my_cell().capacity(); }
};
#endif

template<> struct matpoly_factory<matpoly> {
    typedef matpoly T;
    typedef matpoly_producer_task producer_task;
    typedef matpoly_consumer_task consumer_task;
    matpoly_factory() {}
    T init(abdst_field ab, unsigned int m, unsigned int n, int len) {
        return matpoly(ab, m, n, len);
    }
    static int bw_lingen(bmstatus & bm, T & pi, T & E) {
        return bw_lingen_single(bm, pi, E);
    }
    static size_t capacity(T const & p) { return p.capacity(); }
};

template<typename T, typename Sink>
void bm_io::output_flow(T & pi)
{
    unsigned int n = bm.d.n;

    matpoly::memory_guard dummy(SIZE_MAX);

    /* This object will store the rolling coefficients of the resulting
     * F. Since we compute coefficients on the fly, using F0 which has
     * degree t0, we need a memory of t0+1 coefficients in order to
     * always have one correct coefficient.
     *
     * Also, in the binary case where we want to store coefficients in
     * windows of 64, we need some extra room.
     */
    F = matpoly(bm.d.ab, n, n, t0 + 1);

    set_write_behind_size();

    Sink S(*this, F);

    typename matpoly_factory<T>::consumer_task pi_consumer(*this, pi);

    compute_final_F(S, pi_consumer);

    /* We need this because we want all our deallocation to happen before
     * the guard's dtor gets called */
    F = matpoly();
}


/*}}}*/

void usage()
{
    fprintf(stderr, "Usage: ./lingen [options, to be documented]\n");
    fprintf(stderr,
            "General information about bwc options is in the README file\n");
    exit(EXIT_FAILURE);
}

unsigned int count_lucky_columns(bmstatus & bm)/*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    unsigned int b = m + n;
    int luck_mini = expected_pi_length(d);
    MPI_Bcast(&bm.lucky[0], b, MPI_UNSIGNED, 0, bm.com[0]);
    unsigned int nlucky = 0;
    for(unsigned int j = 0 ; j < b ; nlucky += bm.lucky[j++] >= luck_mini) ;
    return nlucky;
}/*}}}*/

int check_luck_condition(bmstatus & bm)/*{{{*/
{
    bw_dimensions & d = bm.d;
    unsigned int m = d.m;
    unsigned int n = d.n;
    unsigned int nlucky = count_lucky_columns(bm);

    int rank;
    MPI_Comm_rank(bm.com[0], &rank);

    if (!rank) {
        printf("Number of lucky columns: %u (%u wanted)\n", nlucky, n);
    }

    if (nlucky == n)
        return 1;

    if (!rank) {
        fprintf(stderr, "Could not find the required set of solutions (nlucky=%u)\n", nlucky);
    }
    if (random_input_length) {
        static int once=0;
        if (once++) {
            if (!rank) {
                fprintf(stderr, "Solution-faking loop crashed\n");
            }
            MPI_Abort(bm.com[0], EXIT_FAILURE);
        }
        if (!rank) {
            printf("Random input: faking successful computation\n");
        }
        for(unsigned int j = 0 ; j < n ; j++) {
            bm.lucky[(j * 1009) % (m+n)] = expected_pi_length(d);
        }
        return check_luck_condition(bm);
    }

    return 0;
}/*}}}*/

void print_node_assignment(MPI_Comm comm)/*{{{*/
{
    int rank;
    int size;
    MPI_Comm_size(comm, &size);
    MPI_Comm_rank(comm, &rank);

    struct utsname me[1];
    int rc = uname(me);
    if (rc < 0) { perror("uname"); MPI_Abort(comm, 1); }
    size_t sz = 1 + sizeof(me->nodename);
    char * global = (char*) malloc(size * sz);
    memset(global, 0, size * sz);
    memcpy(global + rank * sz, me->nodename, sizeof(me->nodename));

    MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
            global, sz, MPI_BYTE, comm);
    if (rank == 0) {
        char name[80];
        int len=80;
        MPI_Comm_get_name(comm, name, &len);
        name[79]=0;
        for(int i = 0 ; i < size ; i++) {
            printf("# %s rank %d: %s\n", name, i, global + i * sz);
        }
    }
    free(global);
}/*}}}*/

/* Counting memory usage in the recursive algorithm.
 *
 * The recursive algorithm is designed to allow the allocated memory for
 * the input to be reused for placing the output. Some memory might have
 * been saved by upper layers. We also have some local allocation.
 *
 * Notations: The algorithm starts at depth 0 with an
 * input length L, and the notation \ell_i denotes L/2^(i+1). We have
 * \ell_i=2\ell_{i+1}. The notation \alpha denotes m/(m+n). Note that the
 * input has size \alpha*(1-\alpha)*L times (m+n)^2*\log_2(p) (divided by
 * r^2 if relevant).
 *
 * We define five quantities. All are understood as multiples of
 * (m+n)^2*\log_2(p).
 *
 * MP(i) is the extra storage needed for the MP operation at depth i.
 *
 * MUL(i) is the extra storage needed for the MUL operation at depth i.
 *
 * IO(i) is the common size of the input and output data of the call at
 *       depth i. We have
 *              IO(i) = 2\alpha\ell_i
 *
 * ST(i) is the storage *at all levels above the current one* (i.e. with
 *    depth strictly less than i) for the data that is still live and
 *    need to exist until after we return. This count is maximized in the
 *    leftmost branch, where chopped E at all levels must be kept.
 *    chopped E at depth i (not counted in ST(i) !) is:
 *          \alpha(1+\alpha) \ell_i
 *    (counted as the degree it takes to make the necessary data that
 *    we want to use to compute E_right),
 *    so the cumulated cost above is twice the depth 0 value, minus the
 *    depth i value, i.e.
 *              ST(i) = \alpha(1+\alpha)(L-2\ell_i).
 * SP(i) is the "spike" at depth i: not counting allocation that is
 *    already reserved for IO or ST, this is the amount of extra memory
 *    that is required by the call at depth i. We have:
 *      SP(i) = max {
 *              \alpha\ell_i,
 *              \alpha\ell_i-2\alpha\ell_i+\alpha(1+\alpha)\ell_i+SP(i+1),
 *             2\alpha\ell_i-2\alpha\ell_i+\alpha(1+\alpha)\ell_i+MP(i),
 *             2\alpha\ell_i-2\alpha\ell_i+SP(i+1)
 *             4\alpha\ell_i-2\alpha\ell_i+MUL(i)
 *             }
 *            = max {
 *              \alpha\ell_i-2\alpha\ell_i+\alpha(1+\alpha)\ell_i+SP(i+1),
 *             2\alpha\ell_i-2\alpha\ell_i+\alpha(1+\alpha)\ell_i+MP(i),
 *             4\alpha\ell_i-2\alpha\ell_i+MUL(i)
 *                           }
 * 
 * Combining this together, and using
 * ST(i)+\alpha(1+\alpha)\ell_i=ST(i+1), we have:
 *
 * IO(i)+ST(i)+SP(i) = max {
 *              IO(i+1)+ST(i+1)+SP(i+1),
 *              ST(i) + 2\alpha\ell_i+\alpha(1+\alpha)\ell_i+MP(i),
 *              ST(i) + 4\alpha\ell_i+MUL(i)
 *                      }
 *                   = max {
 *              IO(i+1)+ST(i+1)+SP(i+1),
 *              \alpha(1+\alpha)(L-\ell_i)  + 2\alpha\ell_i + MP(i),
 *              \alpha(1+\alpha)(L-2\ell_i) + 4\alpha\ell_i + MUL(i)
 *                      }
 *                   = max {
 *              IO(i+1)+ST(i+1)+SP(i+1),
 *              \alpha((1+\alpha)L+(1-\alpha)\ell_i) + MP(i),
 *              \alpha((1+\alpha)L+2(1-\alpha)\ell_i) + MUL(i),
 *                      }
 *
 * Let RMP(i) be the amount of memory that is reserved while we are doing
 * the MP operation, and define RMUL similarly. We have:
 *      RMP(i)  = \alpha(1+\alpha)(L-\ell_i)  + 2\alpha\ell_i
 *      RMUL(i) = \alpha(1+\alpha)(L-2\ell_i) + 4\alpha\ell_i
 * whence:
 *      RMP(i) = \alpha((1+\alpha)L+(1-\alpha)\ell_i)
 *      RMUL(i) = \alpha((1+\alpha)L+2(1-\alpha)\ell_i)
 *
 * We have RMP(i) <= RMUL(i) <= RMP(0) <= RMUL(0) = 2\alpha*L. We'll use
 * the un-simplified expression later.
 *
 * Furthermore IO(infinity)=SP(infinity)=0, and ST(infinity)=\alpha(1+\alpha)L
 *
 * So that eventually, the amount of reserved memory for the whole
 * algorithm is RMUL(0)=2\alpha*L (which is 2/(1-\alpha)=2*(1+m/n) times
 * the input size). On top of that we have the memory required
 * for the transforms.
 *
 *
 * When going MPI, matrices may be rounded with some inaccuracy.
 * Splitting in two a 3x3 matrix leads to a 2x2 chunk, which is 1.77
 * times more than the simplistic proportionality rule.
 *
 * Therefore it makes sense to distinguish between matrices of size
 * m*(m+n) and (m+n)*(m+n). If we recompute RMUL(i) by taking this into
 * account, we obtain:
 *      [m/r][(m+n)/r][(1+\alpha)(L-2\ell_i)] + [(m+n)/r]^2*[4\alpha\ell_i]
 * where we only paid attention to the rounding issues with dimensions,
 * as those are more important than for degrees. Bottom line, the max is
 * expected to be for i=0, and that will be made only of pi matrices.
 */

/* Some of the early reading must be done before we even start, since
 * the code that we run depends on the input size.
 */
template<typename T>
void lingen_main_code(matpoly_factory<T> & F, abdst_field ab, bm_io & aa)
{
    int rank;
    bmstatus & bm(aa.bm);
    MPI_Comm_rank(bm.com[0], &rank);
    unsigned int m = bm.d.m;
    unsigned int n = bm.d.n;
    unsigned int guess = aa.guessed_length;
    size_t safe_guess = aa.ascii ? ceil(1.05 * guess) : guess;

    /* c0 is (1+m/n) times the input size */
    size_t c0 = abvec_elt_stride(bm.d.ab,
                iceildiv(m+n, bm.mpi_dims[0]) *
                iceildiv(m+n, bm.mpi_dims[1]) *
                iceildiv(m*safe_guess, m+n));
    matpoly::memory_guard main(2*c0);
    if (!rank) {
        char buf[20];
        printf("# Estimated memory for JUST transforms (per node): %s\n",
                size_disp(2*c0, buf));
        printf("# Estimated peak total memory (per node): max at depth %d: %s\n",
                bm.hints.ipeak,
                size_disp(bm.hints.peak, buf));
    }


    T E  = F.init(ab, m, m+n, safe_guess);
    T pi = F.init(ab, 0, 0, 0);   /* pre-init for now */

    typename matpoly_factory<T>::producer_task E_producer(aa, E);

    aa.compute_E(E_producer, guess, safe_guess);
    aa.end_read();

    matpoly_factory<T>::bw_lingen(bm, pi, E);
    bm.stats.final_print();

    bm.display_deltas();
    if (!rank) printf("(pi.alloc = %zu)\n", matpoly_factory<T>::capacity(pi));

    if (check_luck_condition(bm)) {
        if (random_input_length) {
            aa.output_flow<T, bm_output_checksum>(pi);
        } else if (split_output_file) {
            aa.output_flow<T, bm_output_splitfile>(pi);
        } else {
            aa.output_flow<T, bm_output_singlefile>(pi);
        }
    }

    /* clear everything */
}

/* We don't have a header file for this one */
extern "C" void check_for_mpi_problems();

int main(int argc, char *argv[])
{
    cxx_param_list pl;

    bw_common_init(bw, &argc, &argv);

    bw_common_decl_usage(pl);
    lingen_decl_usage(pl);
    logline_decl_usage(pl);

    bw_common_parse_cmdline(bw, pl, &argc, &argv);

    bw_common_interpret_parameters(bw, pl);
    /* {{{ interpret our parameters */
    gmp_randinit_default(rstate);

    int rank;
    int size;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    logline_init_timer();

    param_list_parse_int(pl, "allow_zero_on_rhs", &allow_zero_on_rhs);
    param_list_parse_uint(pl, "random-input-with-length", &random_input_length);
    param_list_parse_int(pl, "split-output-file", &split_output_file);
    param_list_parse_int(pl, "split-input-file", &split_input_file);

    const char * afile = param_list_lookup_string(pl, "afile");

    if (bw->m == -1) {
	fprintf(stderr, "no m value set\n");
	exit(EXIT_FAILURE);
    }
    if (bw->n == -1) {
	fprintf(stderr, "no n value set\n");
	exit(EXIT_FAILURE);
    }
    if (!global_flag_tune && !(afile || random_input_length)) {
        fprintf(stderr, "No afile provided\n");
        exit(EXIT_FAILURE);
    }

    /* we allow ffile and ffile to be both NULL */
    const char * tmp = param_list_lookup_string(pl, "ffile");
    char * ffile = NULL;
    if (tmp) {
        ffile = strdup(tmp);
    } else if (afile) {
        int rc = asprintf(&ffile, "%s.gen", afile);
        ASSERT_ALWAYS(rc >= 0);
    }
    ASSERT_ALWAYS((afile==NULL) == (ffile == NULL));

    bmstatus bm(bw->m, bw->n);
    bw_dimensions & d = bm.d;

    const char * rhs_name = param_list_lookup_string(pl, "rhs");
    if (!global_flag_tune && !random_input_length) {
        if (!rhs_name) {
            fprintf(stderr, "When using lingen, you must either supply --random-input-with-length, or provide a rhs, or possibly provide rhs=none\n");
        } else if (strcmp(rhs_name, "none") == 0) {
            rhs_name = NULL;
        }
    }
    if ((rhs_name != NULL) && param_list_parse_uint(pl, "nrhs", &(bm.d.nrhs))) {
        fprintf(stderr, "the command line arguments rhs= and nrhs= are incompatible\n");
        exit(EXIT_FAILURE);
    }
    if (rhs_name && strcmp(rhs_name, "none") != 0) {
        if (!rank)
            get_rhs_file_header(rhs_name, NULL, &(bm.d.nrhs), NULL);
        MPI_Bcast(&bm.d.nrhs, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);
    }

    abfield_init(d.ab);
    abfield_specify(d.ab, MPFQ_PRIME_MPZ, bw->p);

    bm.lingen_threshold = 10;
    bm.lingen_mpi_threshold = 1000;
    param_list_parse_uint(pl, "lingen_threshold", &(bm.lingen_threshold));
    param_list_parse_uint(pl, "display-threshold", &(display_threshold));
    param_list_parse_uint(pl, "lingen_mpi_threshold", &(bm.lingen_mpi_threshold));
    param_list_parse_uint(pl, "io-block-size", &(io_block_size));
    gmp_randseed_ui(rstate, bw->seed);
    if (bm.lingen_mpi_threshold < bm.lingen_threshold) {
        bm.lingen_mpi_threshold = bm.lingen_threshold;
        fprintf(stderr, "Argument fixing: setting lingen_mpi_threshold=%u (because lingen_threshold=%u)\n",
                bm.lingen_mpi_threshold, bm.lingen_threshold);
    }
    checkpoint_directory = param_list_lookup_string(pl, "checkpoint-directory");
    param_list_parse_uint(pl, "checkpoint-threshold", &checkpoint_threshold);
    param_list_parse_int(pl, "save_gathered_checkpoints", &save_gathered_checkpoints);



#if defined(FAKEMPI_H_)
    bm.lingen_mpi_threshold = UINT_MAX;
#endif

    /* }}} */

    /* TODO: we should rather use lingen_platform.
     */
    /* {{{ Parse MPI args. Make bm.com[0] a better mpi communicator */
    bm.mpi_dims[0] = 1;
    bm.mpi_dims[1] = 1;
    param_list_parse_intxint(pl, "mpi", bm.mpi_dims);
    {
        /* Display node index wrt MPI_COMM_WORLD */
        print_node_assignment(MPI_COMM_WORLD);

        /* Reorder all mpi nodes so that each node gets the given number
         * of jobs, but close together.
         */
        int mpi[2] = { bm.mpi_dims[0], bm.mpi_dims[1], };
        int thr[2] = {1,1};
#ifdef  HAVE_OPENMP
        if (param_list_parse_intxint(pl, "thr", thr)) {
            if (!rank)
                printf("# Limiting number of openmp threads to %d\n",
                        thr[0] * thr[1]);
            omp_set_num_threads(thr[0] * thr[1]);
        }
#else
        if (param_list_parse_intxint(pl, "thr", thr)) {
            if (thr[0]*thr[1] != 1) {
                if (!rank) {
                    fprintf(stderr, "This program only wants openmp for multithreading. Ignoring thr argument.\n");
                }
                param_list_add_key(pl, "thr", "1x1", PARAMETER_FROM_CMDLINE);
            }
        }
#endif

#ifdef  FAKEMPI_H_
        if (mpi[0]*mpi[1] > 1) {
            fprintf(stderr, "non-trivial option mpi= can't be used with fakempi. Please do an MPI-enabled build (MPI=1)\n");
            exit(EXIT_FAILURE);
        }
#endif
        if (!rank)
            printf("# size=%d mpi=%dx%d thr=%dx%d\n", size, mpi[0], mpi[1], thr[0], thr[1]);
        ASSERT_ALWAYS(size == mpi[0] * mpi[1]);
        if (bm.mpi_dims[0] != bm.mpi_dims[1]) {
            if (!rank)
                fprintf(stderr, "The current lingen code is limited to square splits ; here, we received a %d x %d split, which will not work\n",
                    bm.mpi_dims[0], bm.mpi_dims[1]);
            abort();
        }
        int irank = rank / mpi[1];
        int jrank = rank % mpi[1];
        bm.com[0] = MPI_COMM_WORLD;
        /* MPI Api has some very deprecated prototypes */
        MPI_Comm_set_name(bm.com[0], (char*) "world");

        char commname[32];
        snprintf(commname, sizeof(commname), "row%d\n", irank);
        MPI_Comm_split(MPI_COMM_WORLD, irank, jrank, &(bm.com[1]));
        MPI_Comm_set_name(bm.com[1], commname);

        snprintf(commname, sizeof(commname), "col%d\n", jrank);
        MPI_Comm_split(MPI_COMM_WORLD, jrank, irank, &(bm.com[2]));
        MPI_Comm_set_name(bm.com[2], commname);

        print_node_assignment(bm.com[0]);
    }
    /* }}} */


    /* lingen tuning accepts some arguments. We look them up so as to
     * avoid failures down the line */
    lingen_tuning_lookup_parameters(pl);
    
    tree_stats::interpret_parameters(pl);
    logline_interpret_parameters(pl);

    if (param_list_warn_unused(pl)) {
        int rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        if (!rank) param_list_print_usage(pl, bw->original_argv[0], stderr);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    /* TODO: read the a files in scattered mode */

    /* This is our estimate of the *global amount of memory* that the
     * program will use.
     */
    size_t exp_lenA = 2 + iceildiv(bm.d.m, bm.d.n);
    matpoly::memory_guard main_memory(
            /* bm_io */
            abvec_elt_stride(bm.d.ab,bm.d.m*bm.d.n*exp_lenA) +   /* bm_io A */
            abvec_elt_stride(bm.d.ab,bm.d.m*bm.d.m) +   /* bm_io M */
            0);

    /* We now have a protected structure for a_reading task which does
     * the right thing concerning parallelism among MPI nodes (meaning
     * that non-root nodes essentially do nothing while the master job
     * does the I/O stuff) */
    bm_io aa(bm, afile, ffile, global_flag_ascii);
    aa.begin_read();
    aa.guess_length();

    /* run the mpi problem detection only if we're certain that we're at
     * least close to the ballpark where this sort of checks make sense.
     */
    if ((size_t) aa.guessed_length * (size_t) (bm.d.m + bm.d.n) * (size_t) abvec_elt_stride(bm.d.ab, 1) >= (1 << 28)) {
        check_for_mpi_problems();
    }

    {
        matpoly::memory_guard blanket(SIZE_MAX);
#ifndef SELECT_MPFQ_LAYER_u64k1
        typename matpoly_ft<fft_transform_info>::memory_guard blanket_ft(SIZE_MAX);
#else
        typename matpoly_ft<gf2x_cantor_fft_info>::memory_guard blanket_ft(SIZE_MAX);
#endif
        try {
            /* iceildiv(m,n) is t0. We subtract 1 because we usually work
             * with shifted inputs in order to deal with homogenous
             * systems. If rhs==n however, the input isn't shifted. Cf
             * bm_compute_E.
             */
            size_t L = aa.guessed_length - iceildiv(bm.d.m, bm.d.n) - (bm.d.n != bm.d.nrhs);
            bm.hints = lingen_tuning(bm.d, L, bm.com[0], pl);
        } catch (std::overflow_error const & e) {
            fputs(e.what(), stderr);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }

    if (global_flag_tune) {
        MPI_Finalize();
        exit(EXIT_SUCCESS);
    }

    aa.compute_initial_F();

    int go_mpi = aa.guessed_length >= bm.lingen_mpi_threshold;

    if (go_mpi && !rank) {
        if (size > 1) {
            printf("Expected length %u exceeds MPI threshold %u, going MPI now.\n", aa.guessed_length, bm.lingen_mpi_threshold);
        } else {
            printf("Expected length %u exceeds MPI threshold %u, but the process is not running in an MPI context.\n", aa.guessed_length, bm.lingen_mpi_threshold);
        }
    }

    if (go_mpi && size > 1) {
#ifdef ENABLE_MPI_LINGEN
        matpoly_factory<bigmatpoly> F(bm.com, bm.mpi_dims[0], bm.mpi_dims[1]);
        lingen_main_code(F, d.ab, aa);
#else
        /* The ENABLE_MPI_LINGEN flag should be turned on for a proper
         * MPI run.
         */
        ASSERT_ALWAYS(0);
#endif
    } else if (!rank) {
        /* We don't want to bother with memory problems in the non-mpi
         * case when the tuning was done for MPI: this is because the
         * per-transform ram was computed in the perspective of an MPI
         * run, and not for a plain run.
         */
        matpoly_factory<matpoly> F;
        if (size > 1) {
#ifndef SELECT_MPFQ_LAYER_u64k1
            typename matpoly_ft<fft_transform_info>::memory_guard blanket_ft(SIZE_MAX);
#else
            typename matpoly_ft<gf2x_cantor_fft_info>::memory_guard blanket_ft(SIZE_MAX);
#endif
            lingen_main_code(F, d.ab, aa);
        } else {
            /* on the other hand, plain non-mpi code should benefit from
             * that safety net, since the tuning is expected to have
             * computed the needed ram correctly.
             */
            lingen_main_code(F, d.ab, aa);
        }
    } else {
        /* we have go_mpi == 0 and rank > 0 : all we have to do is
         * wait...
         */
    }

    if (!rank && random_input_length) {
        printf("t_basecase = %.2f\n", bm.t_basecase);
        printf("t_mp = %.2f\n", bm.t_mp);
        printf("t_mul = %.2f\n", bm.t_mul);
        printf("t_cp_io = %.2f\n", bm.t_cp_io);
        long peakmem = PeakMemusage();
        if (peakmem > 0)
            printf("# PeakMemusage (MB) = %ld (VmPeak: can be misleading)\n", peakmem >> 10);
    }

    abfield_clear(d.ab);
    if (ffile) free(ffile);

    gmp_randclear(rstate);

    bw_common_clear(bw);

    return rank0_exit_code;
}

/* vim:set sw=4 sta et: */
