#include "cado.h" // IWYU pragma: keep
#include <cerrno>      // for errno
#include <cstdlib>     // for exit, free, strtoul, EXIT_FAILURE
#include <cstring>     // for strdup
#include <cctype>      // for isspace
#include <cstdint>     // for uint64_t
#include <cstdio>      // for fprintf, NULL, stderr, fclose, fgets, fopen, FILE
#include "las-dlog-base.hpp"
#include "las-multiobj-globals.hpp"
#include "macros.h"    // for ASSERT_ALWAYS
#include "typedefs.h"  // for index_t
#include "utils.h"     // for cxx_param_list, param_list_decl_usage, verbose...

void las_dlog_base::declare_usage(cxx_param_list & pl)
{
    if (!dlp_descent) return;
    param_list_decl_usage(pl, "renumber", "renumber table (for the descent)");
    param_list_decl_usage(pl, "log", "log table, as built by reconstructlog");
    /* These belong to las-siever-config of course. But we do a lookup
     * from here as well.
     */
    param_list_decl_usage(pl, "lpb0", "set large prime bound on side 0 to 2^lpb0");
    param_list_decl_usage(pl, "lpb1", "set large prime bound on side 1 to 2^lpb1");
}

las_dlog_base::las_dlog_base(cxx_param_list & pl)
{
    if (!dlp_descent) return;
    renumberfilename = NULL;
    logfilename = NULL;
    renumber_init_for_reading(renumber_table);
    const char * tmp;
    if ((tmp = param_list_lookup_string(pl, "renumber")) != NULL) {
        renumberfilename = strdup(tmp);
    }
    if ((tmp = param_list_lookup_string(pl, "log")) != NULL) {
        logfilename = strdup(tmp);
    }
    if (!logfilename != !renumberfilename) {
        fprintf(stderr, "In descent mode, want either renumber+log, or none\n");
        exit(EXIT_FAILURE);
    }
    if (!param_list_parse_ulong(pl, "lpb0", &(lpb[0]))) {
        fprintf(stderr, "In descent mode, want lpb0 for the final descent\n");
        exit(EXIT_FAILURE);
    }
    if (!param_list_parse_ulong(pl, "lpb1", &(lpb[1]))) {
        fprintf(stderr, "In descent mode, want lpb1 for the final descent\n");
        exit(EXIT_FAILURE);
    }
    read();
}

las_dlog_base::~las_dlog_base()
{
    if (!dlp_descent) return;
    renumber_clear (renumber_table);
    free(renumberfilename);
    free(logfilename);
}

bool las_dlog_base::is_known(int side, uint64_t p, uint64_t r) const
{
    ASSERT_ALWAYS(dlp_descent);
    // if p is above large prime bound,  its log is not known.
    if (lpb[side] >= 64)
        return false;
    if (p >> lpb[side]) {
        return false;
    }
    if (renumberfilename) {
        /* For now we assume that we know the log of all bad ideals */
        /* If we want to be able to do a complete lookup for bad ideals
         * too, then we need the badidealinfo file, as well as a piece of
         * code which is currently in dup2 */
        if (renumber_is_bad (NULL, NULL, renumber_table, p, r, side))
            return true;
        index_t h = renumber_get_index_from_p_r(renumber_table, p, r, side);
        return known_logs[h];
    }
    return true;
}


void las_dlog_base::read()
{
    ASSERT_ALWAYS(dlp_descent);
    if (!renumberfilename) {
        verbose_output_print(0, 1, "# Descent: no access to renumber table given, using lpb(%lu/%lu) to decide what are the supposedly known logs\n",
                lpb[0], lpb[1]);
        return;
    }

    verbose_output_print(0, 1, "# Descent: will get list of known logs from %s, using also %s for mapping\n", logfilename, renumberfilename);

    renumber_read_table(renumber_table, renumberfilename);

    for(int side = 0 ; side < 2 ; side++) {
        if (lpb[side] != renumber_table->lpb[side]) {
            fprintf(stderr, "lpb%d=%lu different from lpb%d=%lu stored in renumber table, probably a bug\n", side, lpb[side], side, renumber_table->lpb[side]);
            exit(EXIT_FAILURE);
        }
    }

    uint64_t nprimes = renumber_table->size;
    known_logs.assign(nprimes + 32, false);
    /* 32 is because the SM columns are here, too ! We would like to
     * avoid reallocation, so let's be generous (anyway we'll
     * reallocate if needed)
     */

    /* format of the log table: there are FIVE different line types.
     *
     * [index] added column [log]
     * [index] bad ideals [log]
     * [index] [p] [side] rat [log]
     * [index] [p] [side] [r] [log]
     * [index] SM col [i] [log]
     * where by default side=0 means the rational side, side=1 the algebraic one
     * r is the root of f(x) mod p, where f is the algebraic polynomial
     * log is the virtual logarithm
     * (see https://lists.gforge.inria.fr/pipermail/cado-nfs-discuss/2019-February/000998.html)
     *
     * Here we care only about the index anyway. By the way, this index
     * is written in hex.
     */
    FILE * f = fopen(logfilename, "r");
    ASSERT_ALWAYS(f != NULL);
    size_t nlogs=0;
    for(int lnum = 0 ; ; lnum++) {
        char line[1024];
        char * x = fgets(line, sizeof(line), f);
        if (x == NULL) break;
        for( ; *x && isspace(*x) ; x++) ;
        if (*x == '#') continue;
        if (!*x) continue;
        errno=0;
        unsigned long z = strtoul(x, &x, 16);
        if (errno) {
            fprintf(stderr, "Parse error at line %d in %s: %s\n", lnum, logfilename, line);
            break;
        }
        if (z >= known_logs.size()) {
            /* happens for SM columns: we have more than the size of the
             * renumber table ! */
            known_logs.resize(z + 1);
        }
	nlogs+=!known_logs[z];
        known_logs[z] = true;
    }
    fclose(f);
    verbose_output_print(0, 1, "# Got %zu known logs from %s\n", nlogs, logfilename);
}


