/*============================================================================
 * Lagrangian module logging
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2016 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <limits.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <float.h>
#include <assert.h>

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "bft_printf.h"
#include "bft_error.h"
#include "bft_mem.h"

#include "cs_log.h"

#include "cs_math.h"

#include "cs_mesh.h"
#include "cs_parall.h"

#include "cs_field.h"

#include "cs_lagr.h"
#include "cs_lagr_tracking.h"
#include "cs_lagr_post.h"
#include "cs_lagr_stat.h"

#include "cs_lagr_prototypes.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_lagr_log.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Additional doxygen documentation
 *============================================================================*/

/*!
  \file cs_lagr_listing.c
*/

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*============================================================================
 * Static global variables
 *============================================================================*/

const char *_astat[2] = {N_("off"), N_("on")};

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Return string indicating on/off depending on integer value
 *
 * \param  i  input integer
 *
 * \return  status string, possibly translated
 */
/*----------------------------------------------------------------------------*/

static const char *
_status(int i)
{
  return (i > 0) ? _(_astat[1]) : _(_astat[0]);
}

/*----------------------------------------------------------------*/
/*!
 *\brief  Calcul des valeurs min max et moyenne pour les statistiques aux frontieres.
 *
 * Parameters:
 * \param[in]  ivar      numero de la variable a integrer
 * \param[out] gmin      valeurs  min
 * \param[out] gmax      valeurs  max
 * \param[in]  unsnbr    inverse du nombre interaction frontiere pour moyenne
 * \param[in]  unsnbrfou inverse du nombre interaction frontiere (avec fouling) pour moyenne
 */
 /*----------------------------------------------------------------*/

static void
_lagstf(int         ivar,
        cs_real_t  *gmin,
        cs_real_t  *gmax,
        cs_real_t   unsnbr[],
        cs_real_t   unsnbrfou[])
{
  cs_lagr_boundary_interactions_t *lagr_bd_i = cs_glob_lagr_boundary_interactions;
  cs_lnum_t nfabor = cs_glob_mesh->n_b_faces;

  /* initializations */
  cs_lnum_t nbrfac  = 0;
  *gmax  = -cs_math_big_r;
  *gmin  =  cs_math_big_r;

  cs_real_t  seuilf = cs_glob_lagr_stat_options->threshold;

  if (lagr_bd_i->imoybr[ivar] == 3) {

    for (cs_lnum_t ifac = 0; ifac < nfabor; ifac++) {

      if (bound_stat[ifac + nfabor * lagr_bd_i->iencnb] > seuilf) {

        nbrfac++;
        *gmax = CS_MAX(*gmax, bound_stat[ifac + nfabor * ivar] / unsnbrfou[ifac]);
        *gmin = CS_MIN(*gmin, bound_stat[ifac + nfabor * ivar] / unsnbrfou[ifac]);

      }

    }

  }
  else if (lagr_bd_i->imoybr[ivar] == 2) {

    for (cs_lnum_t ifac = 0; ifac < nfabor; ifac++) {

      if (bound_stat[ifac + nfabor * lagr_bd_i->inbr] > seuilf) {

        nbrfac++;
        *gmax = CS_MAX(*gmax, bound_stat[ifac + nfabor * ivar] * unsnbr[ifac]);
        *gmin = CS_MIN(*gmin, bound_stat[ifac + nfabor * ivar] * unsnbr[ifac]);

      }

    }

  }
  else if (lagr_bd_i->imoybr[ivar] == 1) {

    for (cs_lnum_t ifac = 0; ifac < nfabor; ifac++) {

      if (bound_stat[ifac + nfabor * lagr_bd_i->inbr] > seuilf) {

        nbrfac++;
        *gmax = CS_MAX(*gmax, bound_stat[ifac + nfabor * ivar] / lagr_bd_i->tstatp);
        *gmin = CS_MIN(*gmin, bound_stat[ifac + nfabor * ivar] / lagr_bd_i->tstatp);

      }

    }

  }
  else if (lagr_bd_i->imoybr[ivar] == 0) {

    for (cs_lnum_t ifac = 0; ifac < nfabor; ifac++) {

      if (bound_stat[ifac + nfabor * lagr_bd_i->inbr] > seuilf) {

        nbrfac++;
        *gmax = CS_MAX(*gmax, bound_stat[ifac + nfabor * ivar]);
        *gmin = CS_MIN(*gmin, bound_stat[ifac + nfabor * ivar]);

      }

    }

  }

  if (nbrfac == 0) {

    *gmax     = 0.0;
    *gmin     = 0.0;

  }
}

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Log Lagrangian module output in the setup file.
 */
/*----------------------------------------------------------------------------*/

void
cs_lagr_log_setup(void)
{
  /* Check if this call is needed */

  if (cs_glob_lagr_time_scheme == NULL)
    return;

  if (cs_glob_lagr_time_scheme->iilagr < 1)
    return;

  /* Now add Lagrangian setup info */

  cs_log_printf(CS_LOG_SETUP,
                _("\n"
                  "Lagrangian model options\n"
                  "------------------------\n"));

  cs_log_printf
    (CS_LOG_SETUP,
     _("  Continuous phase:\n"
       "    iilagr:                 %3d  (0: Lagrangian deactivated\n"
       "                                  1: one way coupling\n"
       "                                  2: two way coupling\n"
       "                                  3: on frozen fields)\n"
       "    restart: %s\n"
       "    statistics/return source terms restart: %s\n\n"
       "  Specific physics associated with particles\n"
       "    physical_model:         %3d  (0: no additional equations\n"
       "                                  1: equations on Dp Tp Mp\n"
       "                                  2: coal particles)\n"),
     cs_glob_lagr_time_scheme->iilagr,
     _status(cs_glob_lagr_time_scheme->isuila),
     _status(cs_glob_lagr_stat_options->isuist),
     cs_glob_lagr_model->physical_model);

  if (cs_glob_lagr_model->physical_model == 1)
    cs_log_printf
      (CS_LOG_SETUP,
       _("    idpvar:                 %3d  (1: eqn diameter Dp,    or 0)\n"
         "    itpvar:                 %3d  (1: eqn temperature Tp, or 0)\n"
         "    impvar:                 %3d  (1: eqn mass Mp,        or 0)\n"),
       cs_glob_lagr_specific_physics->idpvar,
       cs_glob_lagr_specific_physics->itpvar,
       cs_glob_lagr_specific_physics->impvar);

  cs_log_printf
    (CS_LOG_SETUP,
     _("\n  Global parameters:\n"
       "    user particle variables: %2d\n"
       "    isttio:                 %3d  (1: steady carrier phase)\n"),
     cs_glob_lagr_model->n_user_variables,
     cs_glob_lagr_time_scheme->isttio);

  if (cs_glob_lagr_model->physical_model == 2) {

    cs_log_printf
      (CS_LOG_SETUP,
       _("\n  Coal options:\n"
         "    fouling: %s\n"),
       _status(cs_glob_lagr_model->fouling));

    const cs_lagr_extra_module_t *extra = cs_get_lagr_extra_module();

    for (int i = 0; i < extra->ncharb; i++)
      cs_log_printf
        (CS_LOG_SETUP,
         _("    tprenc[%3d]:    %11.5e (threshold T for coal fouling %d)\n"),
         i, cs_glob_lagr_encrustation->tprenc[i], i);

    for (int i = 0; i < extra->ncharb; i++)
      cs_log_printf
        (CS_LOG_SETUP,
         _("    visref[%3d]:    %11.5e (critical coal viscosity %d)\n"),
         i, cs_glob_lagr_encrustation->visref[i], i);

  }

  if (cs_glob_lagr_model->physical_model == 2) {

    cs_log_printf
      (CS_LOG_SETUP,
       _("\n  Return coupling options:\n"
         "    start iteration for time average:  %d\n"
         "    dynamic return coupling:           %s\n"
         "    mass return coupling:              %s\n"
         "    thermal return coupling:           %s\n"),
       cs_glob_lagr_source_terms->nstits,
       _status(cs_glob_lagr_source_terms->ltsdyn),
       _status(cs_glob_lagr_source_terms->ltsmas),
       _status(cs_glob_lagr_source_terms->ltsthe));
  }

  cs_log_printf
    (CS_LOG_SETUP,
     _("\n"
       "  Statistics options:\n"
       "  starting iteration for statistics:        %d\n"
       "  starting iteration for steady statistics: %d\n"
       "  threshold for statistical meaning:        %11.3e\n"),
     cs_glob_lagr_stat_options->idstnt,
     cs_glob_lagr_stat_options->nstist,
     cs_glob_lagr_stat_options->threshold);

  cs_log_printf
    (CS_LOG_SETUP,
     _("\n  Turbulent dispersion options:\n"
       "    lagrangian turbulent dispersion:              %s\n"
       "      identical to fluid turbulent diffusion:     %s\n"
       "    apply complete model from time step:          %d\n"),
     _status(cs_glob_lagr_time_scheme->idistu),
     _status(cs_glob_lagr_time_scheme->idiffl),
     cs_glob_lagr_time_scheme->modcpl);

  if (cs_glob_lagr_time_scheme->modcpl) {
    const char c_dir[] = "xyze";
    int _idirla = cs_glob_lagr_time_scheme->modcpl;
    cs_log_printf
      (CS_LOG_SETUP,
       _("    complete model main flow direction: %c\n"),
       c_dir[_idirla]);
  }

  cs_log_printf
    (CS_LOG_SETUP,
     _("\n  Numerical options:\n"
       "    trajectory time scheme order:                 %d\n"
       "    Poisson correction for particle velocity:     %s\n"),
     cs_glob_lagr_time_scheme->t_order,
     _status(cs_glob_lagr_time_scheme->ilapoi));

  cs_log_printf
    (CS_LOG_SETUP,
     _("\n  Trajectory/particle postprocessing options:\n"
       "    fluid velocity seen:                          %s\n"
       "    velocity:                                     %s\n"
       "    residence time:                               %s\n"
       "    diameter:                                     %s\n"
       "    temperature:                                  %s\n"
       "    mass:                                         %s\n"),
     _status(cs_glob_lagr_post_options->ivisv1),
     _status(cs_glob_lagr_post_options->ivisv2),
     _status(cs_glob_lagr_post_options->ivistp),
     _status(cs_glob_lagr_post_options->ivisdm),
     _status(cs_glob_lagr_post_options->iviste),
     _status(cs_glob_lagr_post_options->ivismp));

  if (cs_glob_lagr_model->physical_model == 2)
    cs_log_printf
      (CS_LOG_SETUP,
       _("    shrinking core diameter:                      %s\n"
         "    moisture mass:                                %s\n"
         "    active coal mass:                             %s\n"
         "    coke mass:                                    %s\n"),
       _status(cs_glob_lagr_post_options->ivisdk),
       _status(cs_glob_lagr_post_options->iviswat),
       _status(cs_glob_lagr_post_options->ivisch),
       _status(cs_glob_lagr_post_options->ivisck));

  cs_log_printf
    (CS_LOG_SETUP,
     _("\n  Statistics for particles/boundary interaction:\n"
       "    compute wall statistics: %s\n"),
     _status(cs_glob_lagr_post_options->iensi3));

  if (cs_glob_lagr_post_options->iensi3)
    cs_log_printf
      (CS_LOG_SETUP,
       _("    number of interactions:                       %s\n"
         "    particle mass flow:                           %s\n"
         "    impact angle:                                 %s\n"
         "    impact velocity:                              %s\n"
         "    interactions with fouling:                    %s\n"
         "    fouling coal mass flux:                       %s\n"
         "    fouling coal diameter:                        %s\n"
         "    fouling coal coke fraction:                   %s\n"
         "    number of additional user statistics:         %d\n"),
       _status(cs_glob_lagr_boundary_interactions->inbrbd),
       _status(cs_glob_lagr_boundary_interactions->iflmbd),
       _status(cs_glob_lagr_boundary_interactions->iangbd),
       _status(cs_glob_lagr_boundary_interactions->ivitbd),
       _status(cs_glob_lagr_boundary_interactions->iencnbbd),
       _status(cs_glob_lagr_boundary_interactions->iencmabd),
       _status(cs_glob_lagr_boundary_interactions->iencdibd),
       _status(cs_glob_lagr_boundary_interactions->iencckbd),
       cs_glob_lagr_boundary_interactions->nusbor);

  /* Volumic statistics   */

  cs_log_printf(CS_LOG_SETUP,
                _("\n"
                  "Lagrangian statistics\n"
                  "---------------------\n\n"));

  cs_log_printf
    (CS_LOG_SETUP,
     _("  Start of calculation from absolute iteration n°: %10d\n"),
     cs_glob_lagr_stat_options->idstnt);

  if (cs_glob_time_step->nt_cur >= cs_glob_lagr_stat_options->idstnt) {

    if (cs_glob_lagr_time_scheme->isttio == 1) {
      cs_log_printf
        (CS_LOG_SETUP,
         _("  Start of steady-state statistics from Lagrangian "
           "iteration n°: %10d)\n"),
         cs_glob_lagr_stat_options->nstist);

    }
    cs_log_printf(CS_LOG_SETUP, "\n");

  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief  Log Lagrangian module output in the main listing file.
 */
/*----------------------------------------------------------------------------*/

void
cs_lagr_log_iteration(void)
{
  /* Check if this call is needed */

  if (cs_glob_lagr_time_scheme == NULL)
    return;

  if (cs_glob_lagr_time_scheme->iilagr < 1)
    return;

  const cs_real_t  *b_stats = bound_stat;

  cs_log_printf(CS_LOG_DEFAULT,
                _("   ** INFORMATION ON THE LAGRANGIAN CALCULATION\n"));
  cs_log_separator(CS_LOG_DEFAULT);

  /* Make sure counters are up-to-date */

  const cs_lagr_particle_counter_t *pc = cs_lagr_update_particle_counter();

  /* Number of particles  */
  cs_log_printf(CS_LOG_DEFAULT,
                _("\n"));
  cs_log_printf(CS_LOG_DEFAULT,
                _("   Current number of particles "
                  "(with and without statistical weight) :\n"));
  cs_log_printf(CS_LOG_DEFAULT,
                _("\n"));
  cs_log_printf
    (CS_LOG_DEFAULT,
     _("ln  newly injected                           %8llu   %14.5E\n"),
     (unsigned long long)(pc->n_g_new),
     pc->w_new);

  if (cs_glob_lagr_model->physical_model == 2 && cs_glob_lagr_model->fouling == 1)
    cs_log_printf
      (CS_LOG_DEFAULT,
       _("ln  coal particles fouled                    %8llu   %14.5E\n"),
       (unsigned long long)(pc->n_g_fouling), pc->w_fouling);

  cs_log_printf
    (CS_LOG_DEFAULT,
     _("ln  out, or deposited and eliminated         %8llu   %14.5E\n"),
     (unsigned long long)(pc->n_g_exit - pc->n_g_failed),
     pc->w_exit - pc->w_failed);
  cs_log_printf
    (CS_LOG_DEFAULT,
     _("ln  deposited                                %8llu   %14.5E\n"),
     (unsigned long long)(pc->n_g_deposited), pc->w_deposited);

  if (cs_glob_lagr_model->resuspension > 0)
    cs_log_printf
      (CS_LOG_DEFAULT,
       _("ln  resuspended                              %8llu   %14.5E\n"),
       (unsigned long long)(pc->n_g_resuspended),
       pc->w_resuspended);

  cs_log_printf
    (CS_LOG_DEFAULT,
     _("ln  lost in the location stage               %8llu   %14.5E\n"),
     (unsigned long long)(pc->n_g_failed), pc->w_failed);
  cs_log_printf
    (CS_LOG_DEFAULT,
     _("ln  total number at the end of the time step %8llu   %14.5E\n"),
     (unsigned long long)pc->n_g_total, pc->w_total);

  if (pc->n_g_cumulative_total > 0)
    cs_log_printf(CS_LOG_DEFAULT,
                  _("%% of lost particles (restart(s) included): %13.4E\n"),
                  pc->n_g_failed * 100.e0
                  / pc->n_g_cumulative_total);
      cs_log_separator(CS_LOG_DEFAULT);

  /* Flow rate for each zone   */
  cs_log_printf(CS_LOG_DEFAULT,
                _("   Zone     Mass flow rate(kg/s)      Boundary type\n"));

  int nbfr = 0;
  cs_lagr_bdy_condition_t *bdy_cond = cs_lagr_get_bdy_conditions();

  /* TODO log for internal zone */
  cs_lagr_internal_condition_t *internal_cond = cs_lagr_get_internal_conditions();

  for (cs_lnum_t ii = 0; ii < bdy_cond->n_b_zones; ii++) {

    if (bdy_cond->b_zone_id[ii] >= nbfr-1)
      nbfr = bdy_cond->b_zone_id[ii] + 1;

  }

  if (cs_glob_rank_id >= 0)
    cs_parall_counter_max(&nbfr, 1);

  cs_real_t debloc[2];
  for (cs_lnum_t nb = 0; nb < nbfr; nb++) {

    debloc[0] = 0.0;
    debloc[1] = 0.0;

    for (cs_lnum_t ii = 0; ii < bdy_cond->n_b_zones; ii++) {

      if (bdy_cond->b_zone_id[ii] == nb) {
        debloc[0] = 1.0;
        debloc[1] = bdy_cond->particle_flow_rate[nb];
      }

    }

    cs_parall_sum(2, CS_REAL_TYPE, debloc);

    if (debloc[0] > 0.5) {

      char const *chcond;

      if (bdy_cond->b_zone_natures[nb] == CS_LAGR_INLET)
        chcond = "INLET";

      else if (bdy_cond->b_zone_natures[nb] == CS_LAGR_REBOUND)
        chcond = "REBOUND";

      else if (bdy_cond->b_zone_natures[nb] == CS_LAGR_OUTLET)
        chcond = "OUTLET";

      else if (   bdy_cond->b_zone_natures[nb] == CS_LAGR_DEPO1
               || bdy_cond->b_zone_natures[nb] == CS_LAGR_DEPO2)
        chcond = "DEPOSITION";

      else if (bdy_cond->b_zone_natures[nb] == CS_LAGR_FOULING)
        chcond = "FOULING";

      else if (bdy_cond->b_zone_natures[nb] == CS_LAGR_DEPO_DLVO)
        chcond = "DLVO CONDITIONS";

      else if (bdy_cond->b_zone_natures[nb] == CS_LAGR_SYM)
        chcond = "SYMMETRY";

      else
        chcond = "USER";

      cs_log_printf(CS_LOG_DEFAULT,
                    "  %3d          %12.5E         %s\n",
                    nb + 1,
                    debloc[1]/cs_glob_lagr_time_step->dtp,
                    chcond);

    }

  }

  cs_log_separator(CS_LOG_DEFAULT);

  /* Boundary statistics  */
  if (cs_glob_lagr_post_options->iensi3 == 1) {

    cs_log_printf(CS_LOG_DEFAULT,
                  _("   Boundary statistics :\n"));
    cs_log_printf(CS_LOG_DEFAULT,
                  "\n");

    if (cs_glob_lagr_time_scheme->isttio == 1) {

      if (cs_glob_time_step->nt_cur >= cs_glob_lagr_stat_options->nstist)
        cs_log_printf(CS_LOG_DEFAULT,
                      _("Number of iterations in steady-state statistics: %10d\n"),
                      cs_glob_lagr_boundary_interactions->npstf);

      else
        cs_log_printf(CS_LOG_DEFAULT,
                      _("Start of steady-state statistics from time step n°: %8d\n"),
                      cs_glob_lagr_stat_options->nstist);

    }

    cs_log_printf(CS_LOG_DEFAULT,
                  _("Total number of iterations in the statistics:%10d\n"),
                  cs_glob_lagr_boundary_interactions->npstft);
    cs_log_printf(CS_LOG_DEFAULT,
                  "\n");

    if (cs_glob_lagr_dim->nvisbr > 0) {

      cs_log_printf(CS_LOG_DEFAULT,
                    _("                           Min value    Max value    \n"));

      const cs_real_t _threshold = 1.e-30;

      cs_real_t *tabvr = NULL;
      cs_real_t *tabvrfou = NULL;

      if (cs_glob_lagr_boundary_interactions->inbrbd == 1) {

        /* Allocate a work array     */
        BFT_MALLOC(tabvr, cs_glob_mesh->n_b_faces, cs_real_t);

        const cs_lnum_t s_id = cs_glob_lagr_boundary_interactions->inbr;

        for (cs_lnum_t ifac = 0; ifac < cs_glob_mesh->n_b_faces; ifac++) {

          if (b_stats[ifac + cs_glob_mesh->n_b_faces * s_id] > _threshold)
            tabvr[ifac] = 1.0 / b_stats[ifac + cs_glob_mesh->n_b_faces * s_id];
          else
            tabvr[ifac] = 0.0;

        }

      }

      if (cs_glob_lagr_boundary_interactions->iencnbbd == 1) {

        BFT_MALLOC(tabvrfou, cs_glob_mesh->n_b_faces, cs_real_t);

        const cs_lnum_t s_id = cs_glob_lagr_boundary_interactions->iencnb;

        for (cs_lnum_t ifac = 0; ifac < cs_glob_mesh->n_b_faces; ifac++) {

          if (b_stats[ifac + cs_glob_mesh->n_b_faces * s_id] > _threshold)
            tabvrfou[ifac] = 1.0 / b_stats[ifac + cs_glob_mesh->n_b_faces * s_id];
          else
            tabvrfou[ifac] = 0.0;

        }
      }

      for (int ivf = 0; ivf < cs_glob_lagr_dim->nvisbr; ivf++) {

        cs_real_t gmin = cs_math_big_r;
        cs_real_t gmax =-cs_math_big_r;

        _lagstf(ivf, &gmin, &gmax, tabvr, tabvrfou);

        cs_parall_min(1, CS_REAL_TYPE, &gmin);
        cs_parall_max(1, CS_REAL_TYPE, &gmax);

        cs_log_printf(CS_LOG_DEFAULT,
                      "lp  %20s  %12.5E  %12.5E\n",
                      cs_glob_lagr_boundary_interactions->nombrd[ivf],
                      gmin,
                      gmax);

      }

      /* Free memory     */

      if (tabvr != NULL)
        BFT_FREE(tabvr);
      if (tabvrfou != NULL)
        BFT_FREE(tabvrfou);

      cs_log_separator(CS_LOG_DEFAULT);

    }

  }

  /* Information about two-way coupling  */

  if (cs_glob_lagr_time_scheme->iilagr == 2) {

    if (cs_glob_lagr_time_scheme->isttio == 0) {

      cs_log_printf(CS_LOG_DEFAULT,
                    _("   Unsteady two-way coupling source terms:\n"));
      cs_log_separator(CS_LOG_DEFAULT);

    }
    else if (cs_glob_lagr_time_scheme->isttio == 1) {

      cs_log_printf(CS_LOG_DEFAULT,
                    _("   Two-way coupling source terms:\n"));
      cs_log_separator(CS_LOG_DEFAULT);

      if (cs_glob_time_step->nt_cur < cs_glob_lagr_source_terms->nstits)
        cs_log_printf(CS_LOG_DEFAULT,
                      _("Reset of the source terms (Start of steady-state at:): %10d\n"),
                      cs_glob_lagr_source_terms->nstits);

      else if (cs_glob_time_step->nt_cur >= cs_glob_lagr_stat_options->nstist)
        cs_log_printf(CS_LOG_DEFAULT,
                      _("Number of iterations for the steady-state source terms:%10d\n"),
                      cs_glob_lagr_source_terms->npts);

    }

    cs_log_printf(CS_LOG_DEFAULT,
                  _("Maximum particle volume fraction : %14.5E\n"),
                  cs_glob_lagr_source_terms->vmax);
    cs_log_printf(CS_LOG_DEFAULT,
                  _("Maximum particle mass fraction :  %14.5E\n"),
                  cs_glob_lagr_source_terms->tmamax);
    cs_log_printf(CS_LOG_DEFAULT,
                  _("Number of cells with a part. volume fraction greater than 0.8 :%10d\n"),
                  cs_glob_lagr_source_terms->ntxerr);
    cs_log_separator(CS_LOG_DEFAULT);

  }

  return;

}

/*----------------------------------------------------------------------------*/

END_C_DECLS
