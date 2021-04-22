// Copyright (C) 2005, 2010 International Business Machines and others.
// All Rights Reserved.
// This code is published under the Eclipse Public License.
//
// Authors:  Carl Laird, Andreas Waechter     IBM    2005-03-17
//
//           Olaf Schenk                      Univ of Basel 2005-09-20
//                  - changed options, added PHASE_ flag

/* some useful links:
 * MKL documentation: https://software.intel.com/en-us/intel-mkl/documentation
 * API differences MKL vs Basel PARDISO: https://software.intel.com/content/www/us/en/develop/articles/summary-of-the-api-differences-between-university-of-basel-ub-pardiso-and-intel-mkl-pardiso.html
 */

#include "IpoptConfig.h"
#include "IpPardisoMKLSolverInterface.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Prototypes for MKL Pardiso's subroutines */
extern "C"
{
void IPOPT_LAPACK_FUNC(pardisoinit,PARDISOINIT)(
   void*         PT,
   const ipfint* MTYPE,
   ipfint*       IPARM
);

void IPOPT_LAPACK_FUNC(pardiso,PARDISO)(
   void**          PT,
   const ipfint*   MAXFCT,
   const ipfint*   MNUM,
   const ipfint*   MTYPE,
   const ipfint*   PHASE,
   const ipfint*   N,
   const ipnumber* A,
   const ipfint*   IA,
   const ipfint*   JA,
   const ipfint*   PERM,
   const ipfint*   NRHS,
   ipfint*         IPARM,
   const ipfint*   MSGLVL,
   ipnumber*       B,
   ipnumber*       X,
   ipfint*         E,
   ipnumber*       DPARM
);
}

namespace Ipopt
{
#if IPOPT_VERBOSITY > 0
static const Index dbg_verbosity = 0;
#endif

PardisoMKLSolverInterface::PardisoMKLSolverInterface()
   : a_(NULL),
     negevals_(-1),
     initialized_(false),
     MAXFCT_(1),
     MNUM_(1),
     MTYPE_(-2),
     MSGLVL_(0),
     debug_last_iter_(-1)
{
   DBG_START_METH("PardisoMKLSolverInterface::PardisoMKLSolverInterface()", dbg_verbosity);

   PT_ = new void* [64];
   IPARM_ = new ipfint[64];
   DPARM_ = new Number[64];
}

PardisoMKLSolverInterface::~PardisoMKLSolverInterface()
{
   DBG_START_METH("PardisoMKLSolverInterface::~PardisoMKLSolverInterface()",
                  dbg_verbosity);

   // Tell Pardiso to release all memory
   if( initialized_ )
   {
      ipfint PHASE = -1;
      ipfint N = dim_;
      ipfint NRHS = 0;
      ipfint ERROR;
      ipfint idmy;
      Number ddmy;
      IPOPT_LAPACK_FUNC(pardiso,PARDISO)(PT_, &MAXFCT_, &MNUM_, &MTYPE_, &PHASE, &N, &ddmy, &idmy, &idmy, &idmy, &NRHS, IPARM_, &MSGLVL_, &ddmy,
                                         &ddmy, &ERROR, DPARM_);
      DBG_ASSERT(ERROR == 0);
   }

   delete[] PT_;
   delete[] IPARM_;
   delete[] DPARM_;
   delete[] a_;
}

void PardisoMKLSolverInterface::RegisterOptions(
   SmartPtr<RegisteredOptions> roptions
)
{
   roptions->AddStringOption3(
      "pardisomkl_matching_strategy",
      "Matching strategy to be used by Pardiso",
      "complete+2x2",
      "complete", "Match complete (IPAR(13)=1)",
      "complete+2x2", "Match complete+2x2 (IPAR(13)=2)",
      "constraints", "Match constraints (IPAR(13)=3)",
      "This is IPAR(13) in Pardiso manual.");
   roptions->AddStringOption2(
      "pardisomkl_redo_symbolic_fact_only_if_inertia_wrong",
      "Toggle for handling case when elements were perturbed by Pardiso.",
      "no",
      "no", "Always redo symbolic factorization when elements were perturbed",
      "yes", "Only redo symbolic factorization when elements were perturbed if also the inertia was wrong",
      "",
      true);
   roptions->AddStringOption2(
      "pardisomkl_repeated_perturbation_means_singular",
      "Interpretation of perturbed elements.",
      "no",
      "no", "Don't assume that matrix is singular if elements were perturbed after recent symbolic factorization",
      "yes", "Assume that matrix is singular if elements were perturbed after recent symbolic factorization",
      "",
      true);
   //roptions->AddLowerBoundedIntegerOption(
   //  "pardisomkl_out_of_core_power",
   //  "Enables out-of-core variant of Pardiso",
   //  0, 0,
   //  "Setting this option to a positive integer k makes Pardiso work in the "
   //  "out-of-core variant where the factor is split in 2^k subdomains.  This "
   //  "is IPARM(50) in the Pardiso manual.  This option is only available if "
   //  "Ipopt has been compiled with Pardiso.");
   roptions->AddLowerBoundedIntegerOption(
      "pardisomkl_msglvl",
      "Pardiso message level",
      0,
      0,
      "This determines the amount of analysis output from the Pardiso solver. "
      "This is MSGLVL in the Pardiso manual.");
   roptions->AddStringOption2(
      "pardisomkl_skip_inertia_check",
      "Always pretend inertia is correct.",
      "no",
      "no", "check inertia",
      "yes", "skip inertia check",
      "Setting this option to \"yes\" essentially disables inertia check. "
      "This option makes the algorithm non-robust and easily fail, but it might give some insight into the necessity of inertia control.",
      true);
   roptions->AddIntegerOption(
      "pardisomkl_max_iterative_refinement_steps",
      "Limit on number of iterative refinement steps.",
      // ToDo: Decide how many iterative refinement steps in Pardiso.
      //       For MKL Pardiso, it seems that setting it to 1 makes it more
      //       robust and just a little bit slower.
      //       Setting it to 1 should decrease the number of iterative refinement
      //       steps by 1 in case that perturbed pivots have been used, and increase
      //       it by 1 otherwise.
      1,
      "The solver does not perform more than the absolute value of this value steps of iterative refinement and "
      "stops the process if a satisfactory level of accuracy of the solution in terms of backward error is achieved. "
      "If negative, the accumulation of the residue uses extended precision real and complex data types. "
      "Perturbed pivots result in iterative refinement. "
      "The solver automatically performs two steps of iterative refinements when perturbed pivots are obtained during the numerical factorization and this option is set to 0.");
   roptions->AddStringOption4(
      "pardisomkl_order",
      "Controls the fill-in reduction ordering algorithm for the input matrix.",
      "metis",
      "amd", "minimum degree algorithm",
      "one", "undocumented",
      "metis", "MeTiS nested dissection algorithm",
      "pmetis", "parallel (OpenMP) version of MeTiS nested dissection algorithm");
}

bool PardisoMKLSolverInterface::InitializeImpl(
   const OptionsList& options,
   const std::string& prefix
)
{
   Index enum_int;
   options.GetEnumValue("pardisomkl_matching_strategy", enum_int, prefix);
   match_strat_ = PardisoMatchingStrategy(enum_int);
   options.GetBoolValue("pardisomkl_redo_symbolic_fact_only_if_inertia_wrong",
                        pardiso_redo_symbolic_fact_only_if_inertia_wrong_, prefix);
   options.GetBoolValue("pardisomkl_repeated_perturbation_means_singular", pardiso_repeated_perturbation_means_singular_,
                        prefix);
   //Index pardiso_out_of_core_power;
   //options.GetIntegerValue("pardiso_out_of_core_power",
   //                        pardiso_out_of_core_power, prefix);
   options.GetBoolValue("pardisomkl_skip_inertia_check", skip_inertia_check_, prefix);
   int pardiso_msglvl;
   options.GetIntegerValue("pardisomkl_msglvl", pardiso_msglvl, prefix);
   int max_iterref_steps;
   options.GetIntegerValue("pardisomkl_max_iterative_refinement_steps", max_iterref_steps, prefix);
   int order;
   options.GetEnumValue("pardisomkl_order", order, prefix);

   // Number value = 0.0;

   // Tell Pardiso to release all memory if it had been used before
   if( initialized_ )
   {
      ipfint PHASE = -1;
      ipfint N = dim_;
      ipfint NRHS = 0;
      ipfint ERROR;
      ipfint idmy;
      Number ddmy;
      IPOPT_LAPACK_FUNC(pardiso,PARDISO)(PT_, &MAXFCT_, &MNUM_, &MTYPE_, &PHASE, &N, &ddmy, &idmy, &idmy, &idmy, &NRHS, IPARM_, &MSGLVL_, &ddmy,
                                         &ddmy, &ERROR, DPARM_);
      DBG_ASSERT(ERROR == 0);
   }

   // Reset all private data
   dim_ = 0;
   nonzeros_ = 0;
   have_symbolic_factorization_ = false;
   initialized_ = false;
   delete[] a_;
   a_ = NULL;

   // Call Pardiso's initialization routine
   memset(PT_, 0, 64); // needs to be initialized to 0 according to MKL Pardiso docu
   IPARM_[0] = 0;  // Tell it to fill IPARM with default values(?)

   IPOPT_LAPACK_FUNC(pardisoinit,PARDISOINIT)(PT_, &MTYPE_, IPARM_);

   // Set some parameters for Pardiso
   IPARM_[0] = 1;  // Don't use the default values
   IPARM_[1] = order;
   // For MKL PARDSIO, the documentation says, "iparm(3) Reserved. Set to zero.", so we don't set IPARM_[2]
   IPARM_[5] = 1;// Overwrite right-hand side
   IPARM_[7] = max_iterref_steps;
   IPARM_[9] = 12;// pivot perturbation (as higher as less perturbation)
   IPARM_[10] = 2;// enable scaling (recommended for interior-point indefinite matrices)
   IPARM_[12] = (int)match_strat_;// enable matching (recommended, as above)
   IPARM_[20] = 3;// bunch-kaufman pivoting
   IPARM_[23] = 1;// parallel fac
   IPARM_[24] = 0;// parallel solve
   //IPARM_[26] = 1; // matrix checker
#ifdef IPOPT_SINGLE
   IPARM_[27] = 1; // Use single precision
#else
   IPARM_[27] = 0; // Use double precision
#endif

   Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                  "Pardiso matrix ordering     (IPARM(2)): %d\n", IPARM_[1]);
   Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                  "Pardiso max. iterref. steps (IPARM(8)): %d\n", IPARM_[7]);
   Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                  "Pardiso matching strategy  (IPARM(13)): %d\n", IPARM_[12]);

   MSGLVL_ = pardiso_msglvl;

   // Option for the out of core variant
   //IPARM_[49] = pardiso_out_of_core_power;

   return true;
}

ESymSolverStatus PardisoMKLSolverInterface::MultiSolve(
   bool         new_matrix,
   const Index* ia,
   const Index* ja,
   Index        nrhs,
   Number*      rhs_vals,
   bool         check_NegEVals,
   Index        numberOfNegEVals
)
{
   DBG_START_METH("PardisoMKLSolverInterface::MultiSolve", dbg_verbosity);
   DBG_ASSERT(!check_NegEVals || ProvidesInertia());
   DBG_ASSERT(initialized_);

   // check if a factorization has to be done
   if( new_matrix )
   {
      // perform the factorization
      ESymSolverStatus retval;
      retval = Factorization(ia, ja, check_NegEVals, numberOfNegEVals);
      if( retval != SYMSOLVER_SUCCESS )
      {
         DBG_PRINT((1, "FACTORIZATION FAILED!\n"));
         return retval;  // Matrix singular or error occurred
      }
   }

   // do the solve
   return Solve(ia, ja, nrhs, rhs_vals);
}

Number* PardisoMKLSolverInterface::GetValuesArrayPtr()
{
   DBG_ASSERT(initialized_);
   DBG_ASSERT(a_);
   return a_;
}

ESymSolverStatus PardisoMKLSolverInterface::InitializeStructure(
   Index        dim,
   Index        nonzeros,
   const Index* ia,
   const Index* ja
)
{
   DBG_START_METH("PardisoMKLSolverInterface::InitializeStructure", dbg_verbosity);
   dim_ = dim;
   nonzeros_ = nonzeros;

   // Make space for storing the matrix elements
   delete[] a_;
   a_ = NULL;
   a_ = new Number[nonzeros_];

   // Do the symbolic factorization
   ESymSolverStatus retval = SymbolicFactorization(ia, ja);
   if( retval != SYMSOLVER_SUCCESS )
   {
      return retval;
   }

   initialized_ = true;

   return retval;
}

ESymSolverStatus PardisoMKLSolverInterface::SymbolicFactorization(
   const Index* /*ia*/,
   const Index* /*ja*/
)
{
   DBG_START_METH("PardisoMKLSolverInterface::SymbolicFactorization",
                  dbg_verbosity);

   // Since Pardiso requires the values of the nonzeros of the matrix
   // for an efficient symbolic factorization, we postpone that task
   // until the first call of Factorize.  All we do here is to reset
   // the flag (in case this interface is called for a matrix with a
   // new structure).

   have_symbolic_factorization_ = false;

   return SYMSOLVER_SUCCESS;
}

static
void write_iajaa_matrix(
   int          N,
   const Index* ia,
   const Index* ja,
   Number*      a_,
   Number*      rhs_vals,
   int          iter_cnt,
   int          sol_cnt
)
{
   if( getenv("IPOPT_WRITE_MAT") )
   {
      /* Write header */
      FILE* mat_file;
      char mat_name[128];
      char mat_pref[32];

      ipfint NNZ = ia[N] - 1;
      ipfint i;

      if( getenv("IPOPT_WRITE_PREFIX") )
      {
         strcpy(mat_pref, getenv("IPOPT_WRITE_PREFIX"));
      }
      else
      {
         strcpy(mat_pref, "mat-ipopt");
      }

      Snprintf(mat_name, 127, "%s_%03d-%02d.iajaa", mat_pref, iter_cnt, sol_cnt);

      // Open and write matrix file.
      mat_file = fopen(mat_name, "w");

      fprintf(mat_file, "%d\n", N);
      fprintf(mat_file, "%d\n", NNZ);

      for( i = 0; i < N + 1; i++ )
      {
         fprintf(mat_file, "%d\n", ia[i]);
      }
      for( i = 0; i < NNZ; i++ )
      {
         fprintf(mat_file, "%d\n", ja[i]);
      }
      for( i = 0; i < NNZ; i++ )
      {
         fprintf(mat_file, "%32.24e\n", a_[i]);
      }

      /* Right hand side. */
      if( rhs_vals )
         for( i = 0; i < N; i++ )
         {
            fprintf(mat_file, "%32.24e\n", rhs_vals[i]);
         }

      fclose(mat_file);
   }
   /* addtional matrix format */
   if( getenv("IPOPT_WRITE_MAT_MTX") )
   {
      /* Write header */
      FILE* mat_file;
      char mat_name[128];
      char mat_pref[32];

      ipfint i;
      ipfint j;

      if( getenv("IPOPT_WRITE_PREFIX") )
      {
         strcpy(mat_pref, getenv("IPOPT_WRITE_PREFIX"));
      }
      else
      {
         strcpy(mat_pref, "mat-ipopt");
      }

      Snprintf(mat_name, 127, "%s_%03d-%02d.mtx", mat_pref, iter_cnt, sol_cnt);

      // Open and write matrix file.
      mat_file = fopen(mat_name, "w");

      for( i = 0; i < N; i++ )
         for( j = ia[i]; j < ia[i + 1] - 1; j++ )
         {
            fprintf(mat_file, " %d %d %32.24e \n", i + 1, ja[j - 1], a_[j - 1]);
         }

      fclose(mat_file);
   }
}

ESymSolverStatus PardisoMKLSolverInterface::Factorization(
   const Index* ia,
   const Index* ja,
   bool         check_NegEVals,
   Index        numberOfNegEVals
)
{
   DBG_START_METH("PardisoMKLSolverInterface::Factorization", dbg_verbosity);

   // Call Pardiso to do the factorization
   ipfint PHASE;
   ipfint N = dim_;
   ipfint PERM;   // This should not be accessed by Pardiso
   ipfint NRHS = 0;
   Number B;  // This should not be accessed by Pardiso in factorization
   // phase
   Number X;  // This should not be accessed by Pardiso in factorization
   // phase
   ipfint ERROR;

   bool done = false;
   bool just_performed_symbolic_factorization = false;

   while( !done )
   {
      if( !have_symbolic_factorization_ )
      {
         if( HaveIpData() )
         {
            IpData().TimingStats().LinearSystemSymbolicFactorization().Start();
         }
         PHASE = 11;

         Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                        "Calling Pardiso for symbolic factorization.\n");
         IPOPT_LAPACK_FUNC(pardiso,PARDISO)(PT_, &MAXFCT_, &MNUM_, &MTYPE_,
                                            &PHASE, &N, a_, ia, ja, &PERM,
                                            &NRHS, IPARM_, &MSGLVL_, &B, &X, &ERROR, DPARM_);
         if( HaveIpData() )
         {
            IpData().TimingStats().LinearSystemSymbolicFactorization().End();
         }
         if( ERROR == -7 )
         {
            Jnlst().Printf(J_MOREDETAILED, J_LINEAR_ALGEBRA,
                           "Pardiso symbolic factorization returns ERROR = %d.  Matrix is singular.\n", ERROR);
            return SYMSOLVER_SINGULAR;
         }
         else if( ERROR != 0 )
         {
            Jnlst().Printf(J_ERROR, J_LINEAR_ALGEBRA,
                           "Error in Pardiso during symbolic factorization phase.  ERROR = %d.\n", ERROR);
            return SYMSOLVER_FATAL_ERROR;
         }
         have_symbolic_factorization_ = true;
         just_performed_symbolic_factorization = true;

         Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                        "Memory in KB required for the symbolic factorization  = %d.\n", IPARM_[14]);
         Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                        "Integer memory in KB required for the numerical factorization  = %d.\n", IPARM_[15]);
         Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                        "Double  memory in KB required for the numerical factorization  = %d.\n", IPARM_[16]);
      }

      PHASE = 22;

      if( HaveIpData() )
      {
         IpData().TimingStats().LinearSystemFactorization().Start();
      }
      Jnlst().Printf(J_MOREDETAILED, J_LINEAR_ALGEBRA,
                     "Calling Pardiso for factorization.\n");
      // Dump matrix to file, and count number of solution steps.
      if( HaveIpData() )
      {
         if( IpData().iter_count() != debug_last_iter_ )
         {
            debug_cnt_ = 0;
         }
         debug_last_iter_ = IpData().iter_count();
         debug_cnt_++;
      }
      else
      {
         debug_cnt_ = 0;
         debug_last_iter_ = 0;
      }

      IPOPT_LAPACK_FUNC(pardiso,PARDISO)(PT_, &MAXFCT_, &MNUM_, &MTYPE_,
                                         &PHASE, &N, a_, ia, ja, &PERM,
                                         &NRHS, IPARM_, &MSGLVL_, &B, &X, &ERROR, DPARM_);
      if( HaveIpData() )
      {
         IpData().TimingStats().LinearSystemFactorization().End();
      }

      if( ERROR == -7 )
      {
         Jnlst().Printf(J_MOREDETAILED, J_LINEAR_ALGEBRA,
                        "Pardiso factorization returns ERROR = %d.  Matrix is singular.\n", ERROR);
         return SYMSOLVER_SINGULAR;
      }
      else if( ERROR == -4 )
      {
         // I think this means that the matrix is singular
         // OLAF said that this will never happen (ToDo)
         return SYMSOLVER_SINGULAR;
      }
      else if( ERROR != 0 )
      {
         Jnlst().Printf(J_ERROR, J_LINEAR_ALGEBRA,
                        "Error in Pardiso during factorization phase.  ERROR = %d.\n", ERROR);
         return SYMSOLVER_FATAL_ERROR;
      }

      negevals_ = Max(IPARM_[22], numberOfNegEVals);
      if( IPARM_[13] != 0 )
      {
         Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                        "Number of perturbed pivots in factorization phase = %d.\n", IPARM_[13]);
         if( !pardiso_redo_symbolic_fact_only_if_inertia_wrong_ || (negevals_ != numberOfNegEVals) )
         {
            if( HaveIpData() )
            {
               IpData().Append_info_string("Pn");
            }
            have_symbolic_factorization_ = false;
            // We assume now that if there was just a symbolic
            // factorization and we still have perturbed pivots, that
            // the system is actually singular, if
            // pardiso_repeated_perturbation_means_singular_ is true
            if( just_performed_symbolic_factorization )
            {
               if( pardiso_repeated_perturbation_means_singular_ )
               {
                  if( HaveIpData() )
                  {
                     IpData().Append_info_string("Ps");
                  }
                  return SYMSOLVER_SINGULAR;
               }
               else
               {
                  done = true;
               }
            }
            else
            {
               done = false;
            }
         }
         else
         {
            if( HaveIpData() )
            {
               IpData().Append_info_string("Pp");
            }
            done = true;
         }
      }
      else
      {
         done = true;
      }
   }

   DBG_ASSERT(IPARM_[21] + IPARM_[22] == dim_);

   // Check whether the number of negative eigenvalues matches the requested
   // count
   if( skip_inertia_check_ )
   {
      numberOfNegEVals = negevals_;
   }

   if( check_NegEVals && (numberOfNegEVals != negevals_) )
   {
      Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                     "Wrong inertia: required are %d, but we got %d.\n", numberOfNegEVals, negevals_);
      return SYMSOLVER_WRONG_INERTIA;
   }

   return SYMSOLVER_SUCCESS;
}

ESymSolverStatus PardisoMKLSolverInterface::Solve(
   const Index* ia,
   const Index* ja,
   Index        nrhs,
   Number*      rhs_vals
)
{
   DBG_START_METH("PardisoMKLSolverInterface::Solve", dbg_verbosity);

   if( HaveIpData() )
   {
      IpData().TimingStats().LinearSystemBackSolve().Start();
   }
   // Call Pardiso to do the solve for the given right-hand sides
   ipfint PHASE = 33;
   ipfint N = dim_;
   ipfint PERM;   // This should not be accessed by Pardiso
   ipfint NRHS = nrhs;
   Number* X = new Number[nrhs * dim_];

   Number* ORIG_RHS = new Number[nrhs * dim_];
   ipfint ERROR;
   // Initialize solution with zero and save right hand side
   for( int i = 0; i < N; i++ )
   {
      X[i] = 0.;
      ORIG_RHS[i] = rhs_vals[i];
   }

   // Dump matrix to file if requested
   Index iter_count = 0;
   if( HaveIpData() )
   {
      iter_count = IpData().iter_count();
   }

   write_iajaa_matrix(N, ia, ja, a_, rhs_vals, iter_count, debug_cnt_);

   for( int i = 0; i < N; i++ )
   {
      rhs_vals[i] = ORIG_RHS[i];
   }
   IPOPT_LAPACK_FUNC(pardiso,PARDISO)(PT_, &MAXFCT_, &MNUM_, &MTYPE_, &PHASE, &N, a_, ia, ja, &PERM, &NRHS, IPARM_, &MSGLVL_, rhs_vals, X,
      &ERROR, DPARM_);

   if( ERROR <= -100 && ERROR >= -102 )
   {
      Jnlst().Printf(J_WARNING, J_LINEAR_ALGEBRA,
         "Iterative solver in Pardiso did not converge (ERROR = %d)\n", ERROR);
      Jnlst().Printf(J_WARNING, J_LINEAR_ALGEBRA,
         "  Decreasing drop tolerances from DPARM_[4] = %e and DPARM_[5] = %e\n", DPARM_[4], DPARM_[5]);
      PHASE = 23;
      DPARM_[4] /= 2.0;
      DPARM_[5] /= 2.0;
      Jnlst().Printf(J_WARNING, J_LINEAR_ALGEBRA,
         "                               to DPARM_[4] = %e and DPARM_[5] = %e\n", DPARM_[4], DPARM_[5]);
      ERROR = 0;
   }

   delete[] X;
   delete[] ORIG_RHS;

   if( IPARM_[6] != 0 )
   {
      Jnlst().Printf(J_DETAILED, J_LINEAR_ALGEBRA,
                     "Number of iterative refinement steps = %d.\n", IPARM_[6]);
      if( HaveIpData() )
      {
         IpData().Append_info_string("Pi");
      }
   }

   if( HaveIpData() )
   {
      IpData().TimingStats().LinearSystemBackSolve().End();
   }
   if( ERROR != 0 )
   {
      Jnlst().Printf(J_ERROR, J_LINEAR_ALGEBRA,
                     "Error in Pardiso during solve phase.  ERROR = %d.\n", ERROR);
      return SYMSOLVER_FATAL_ERROR;
   }
   return SYMSOLVER_SUCCESS;
}

Index PardisoMKLSolverInterface::NumberOfNegEVals() const
{
   DBG_START_METH("PardisoMKLSolverInterface::NumberOfNegEVals", dbg_verbosity);
   DBG_ASSERT(negevals_ >= 0);
   return negevals_;
}

bool PardisoMKLSolverInterface::IncreaseQuality()
{
   // At the moment, I don't see how we could tell Pardiso to do better
   // (maybe switch from IPARM[20]=1 to IPARM[20]=2?)
   return false;
}

} // namespace Ipopt
