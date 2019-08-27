/* Recomputes the Murphy-E value with a larger value of ALPHA_BOUND
   on the top polynomials found after the rootsieve:

   polyselect3 -poly cxxx.poly -num 10 -Bf ... -Bg ... -area ...

   will process cxxx.poly.0, cxxx.poly.1, ..., cxxx.poly.9
   and add the new Murphy-E value at the end of each file.
*/

#include "cado.h"
/* The following avoids to put #ifdef HAVE_OPENMP ... #endif around each
   OpenMP pragma. It should come after cado.h, which sets -Werror=all. */
#ifdef  __GNUC__
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include "utils.h"
#include "murphyE.h"
#include "auxiliary.h"
#ifdef HAVE_OPENMP
#include "omp.h"
#endif

static void
declare_usage (param_list pl)
{
  param_list_decl_usage(pl, "poly", "polynomial prefix");
  param_list_decl_usage(pl, "t", "number of threads");
  param_list_decl_usage(pl, "num", "number of files to process");
  param_list_decl_usage(pl, "Bf", "factor base bound on the algebraic side");
  param_list_decl_usage(pl, "Bg", "factor base bound on the linear side");
  param_list_decl_usage(pl, "area", "sieving area");
  verbose_decl_usage(pl);
}

int
main (int argc, char *argv[])
{
  param_list pl;
  FILE *f;
  char *argv0 = argv[0];
  double Bf, Bg, area;
  int nthreads = 1;
  int num = 1; /* number of files to process */

  param_list_init(pl);
  declare_usage(pl);

  argv++, argc--;
  for( ; argc ; ) {
      if (param_list_update_cmdline(pl, &argc, &argv)) { continue; }

      /* Could also be a file */
      if ((f = fopen(argv[0], "r")) != NULL) {
          param_list_read_stream(pl, f, 0);
          fclose(f);
          argv++,argc--;
          continue;
      }

      fprintf(stderr, "Unhandled parameter %s\n", argv[0]);
      param_list_print_usage(pl, argv0, stderr);
      exit (EXIT_FAILURE);
  }
  verbose_interpret_parameters (pl);
  param_list_print_command_line (stdout, pl);

  param_list_parse_int (pl, "t", &nthreads);
#ifdef HAVE_OPENMP
  omp_set_num_threads (nthreads);
#endif
  param_list_parse_int (pl, "num", &num);

  const char * filename;
  if ((filename = param_list_lookup_string (pl, "poly")) == NULL)
    {
      fprintf (stderr, "Error: parameter -poly is mandatory\n");
      param_list_print_usage (pl, argv0, stderr);
      exit (EXIT_FAILURE);
    }

  if (param_list_parse_double (pl, "Bf", &Bf) == 0)
    {
      fprintf (stderr, "Error: parameter -Bf is mandatory\n");
      param_list_print_usage (pl, argv0, stderr);
      exit (EXIT_FAILURE);
    }

  if (param_list_parse_double (pl, "Bg", &Bg) == 0)
    {
      fprintf (stderr, "Error: parameter -Bg is mandatory\n");
      param_list_print_usage (pl, argv0, stderr);
      exit (EXIT_FAILURE);
    }

  if (param_list_parse_double (pl, "area", &area) == 0)
    {
      fprintf (stderr, "Error: parameter -area is mandatory\n");
      param_list_print_usage (pl, argv0, stderr);
      exit (EXIT_FAILURE);
   }

#pragma omp parallel for
  for (int i = 0; i < num; i++)
    {
      cado_poly cpoly;
      char s[1024];
      FILE *fp;

      cado_poly_init (cpoly);
      sprintf (s, "%s.%d", filename, i);
      if (!cado_poly_read (cpoly, s))
        {
          fprintf (stderr, "Error reading polynomial file %s\n", s);
          exit (EXIT_FAILURE);
        }

      double e = MurphyE_chi2 (cpoly, Bf, Bg, area, MURPHY_K, 10 * ALPHA_BOUND);
      fp = fopen (s, "a");
      fprintf (fp, "# MurphyF (Bf=%.3e,Bg=%.3e,area=%.3e) = %.3e\n",
               Bf, Bg, area, e);
      fclose (fp);
      cado_poly_clear (cpoly);
    }

  param_list_clear(pl);

  return 0;
}
