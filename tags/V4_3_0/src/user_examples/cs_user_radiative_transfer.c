/*============================================================================
 * Radiation solver operations.
 *============================================================================*/

/* VERS */

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

#include <assert.h>
#include <string.h>
#include <math.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"
#include "bft_error.h"
#include "bft_printf.h"

#include "fvm_writer.h"

#include "cs_base.h"
#include "cs_field.h"
#include "cs_field_pointer.h"
#include "cs_field_operator.h"
#include "cs_math.h"
#include "cs_mesh.h"
#include "cs_mesh_location.h"
#include "cs_mesh_quantities.h"
#include "cs_halo.h"
#include "cs_log.h"
#include "cs_parall.h"
#include "cs_parameters.h"
#include "cs_physical_constants.h"
#include "cs_physical_model.h"
#include "cs_prototypes.h"
#include "cs_restart.h"
#include "cs_rotation.h"
#include "cs_time_step.h"
#include "cs_selector.h"
#include "cs_rad_transfer.h"

#include "cs_post.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_prototypes.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Additional Doxygen documentation
 *============================================================================*/

/*! \file cs_user_radiative_transfer.c
 *
 * \brief User subroutine for input of radiative transfer parameters : absorption
 *  coefficient and net radiation flux.
 *
 *  See \subpage cs_user_radiative_transfer for examples.
 */

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*=============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief User function for input of radiative transfer module options.
 */
/*----------------------------------------------------------------------------*/

void
cs_user_radiative_transfer_parameters(void)
{
  /*! [cs_user_radiative_transfer_parameters] */

  /* indicate whether the radiation variables should be
     initialized (=0) or read from a restart file (=1) */

  cs_glob_rad_transfer_params->restart = (cs_restart_present()) ? 1 : 0;

  /* period of the radiation module */

  cs_glob_rad_transfer_params->nfreqr = 1;

  /* Quadrature Sn (n(n+2) directions)

     1: S4 (24 directions)
     2: S6 (48 directions)
     3: S8 (80 directions)

     Quadrature Tn (8n^2 directions)

     4: T2 (32 directions)
     5: T4 (128 directions)
     6: Tn (8*ndirec^2 directions)
  */

  cs_glob_rad_transfer_params->i_quadrature = 4;

  cs_glob_rad_transfer_params->ndirec = 3;

  /* Method used to calculate the radiative source term:
     - 0: semi-analytic calculation (required with transparent media)
     - 1: conservative calculation
     - 2: semi-analytic calculation corrected
          in order to be globally conservative
     (If the medium is transparent, the choice has no effect) */

  cs_glob_rad_transfer_params->idiver = 2;

  /* Verbosity level in the listing concerning the calculation of
     the wall temperatures (0, 1 or 2) */

  cs_glob_rad_transfer_params->iimpar = 1;

  /* Verbosity mode for the Luminance (0, 1 or 2) */

  cs_glob_rad_transfer_params->iimlum = 0;

  /* Compute the absorption coefficient through Modak (if 1 or 2),
     or do not use Modak (if 0).
     Useful ONLY when gas or coal combustion is activated
     - imodak = 1: ADF model with 8 wave length intervals
     - imodak = 2: ADF model with 50 wave length intervals */

  cs_glob_rad_transfer_params->imodak = 2;

  /* Compute the absorption coefficient via ADF model
     Useful ONLY when coal combustion is activated
     imoadf = 0: switch off the ADF model
     imoadf = 1: switch on the ADF model (with 8 bands ADF08)
     imoadf = 2: switch on the ADF model (with 50 bands ADF50) */

  cs_glob_rad_transfer_params->imoadf = 1;

  /* Compute the absorption coefficient through FSCK model (if 1)
     Useful ONLY when coal combustion is activated
     imfsck = 1: activated
     imfsck = 0: not activated */

  cs_glob_rad_transfer_params->imfsck = 1;

  /*! [cs_user_radiative_transfer_parameters] */
}

/*-------------------------------------------------------------------------------*/
/*!
 * \brief Absorption coefficient for radiative module
 *
 * It is necessary to define the value of the fluid's absorption coefficient Ck.
 *
 * This value is defined automatically for specific physical models, such
 * as gas and coal combustion, so this function should not be used with
 * these models.
 *
 * For a transparent medium, the coefficient should be set to 0.
 *
 * In the case of the P-1 model, we check that the optical length is at
 * least of the order of 1.
 *
 * \param[in]     bc_type       boundary face types
 * \param[in]     izfrdp        zone number for boundary faces
 * \param[in]     dt            time step (per cell)
 * \param[out]    ck            medium's absorption coefficient
 *                              (zero if transparent)
 */
/*-------------------------------------------------------------------------------*/

void
cs_user_rad_transfer_absorption(const int         bc_type[],
                                const int         izfrdp[],
                                const cs_real_t   dt[],
                                cs_real_t         ck[])
{
  /*< [abso_coeff] */
  /*
   * Absorption coefficient of the medium (m-1)
   *
   * In the case of specific physics (gas/coal/fuel combustion, elec),
   * Ck must not be defined here (it is determined automatically, possibly
   * from the parametric file)
   *
   * In other cases, Ck must be defined (it is zero by default)
   */

  if (cs_glob_physical_model_flag[CS_PHYSICAL_MODEL_FLAG] <= 1) {
    for (cs_lnum_t cell_id = 0; cell_id < cs_glob_mesh->n_cells; cell_id++)
      ck[cell_id] = 0.;
  }

  /*< [abso_coeff]*/
}

/*-------------------------------------------------------------------------------*/
/*!
 * \brief Compute the net radiation flux.
 *
 * The density of net radiation flux must be calculated
 * consistently with the boundary conditions of the intensity.
 * The density of net flux is the balance between the radiative
 * emiting part of a boudary face (and not the reflecting one)
 * and the radiative absorbing part.
 *
 * \param[in]   bc_type   boundary face types
 * \param[in]   izfrdp    boundary faces -> zone number
 * \param[in]   dt        time step (per cell)
 * \param[in]   coefap    boundary condition work array for the luminance
 *                         (explicit part)
 * \param[in]   coefbp    boundary condition work array for the luminance
 *                         (implicit part)
 * \param[in]   cofafp    boundary condition work array for the diffusion
 *                        of the luminance (explicit part)
 * \param[in]   cofbfp    boundary condition work array for the diffusion
 *                        of the luminance (implicit part)
 * \param[in]   twall     inside current wall temperature (K)
 * \param[in]   qincid    radiative incident flux  (W/m2)
 * \param[in]   xlamp     conductivity (W/m/K)
 * \param[in]   epap      thickness (m)
 * \param[in]   epsp      emissivity (>0)
 * \param[in]   ck        absorption coefficient
 * \param[out]  net_flux  net flux (W/m2)
 */
/*-------------------------------------------------------------------------------*/

void
cs_user_rad_transfer_net_flux(const int        itypfb[],
                              const int        izfrdp[],
                              const cs_real_t  dt[],
                              const cs_real_t  coefap[],
                              const cs_real_t  coefbp[],
                              const cs_real_t  cofafp[],
                              const cs_real_t  cofbfp[],
                              const cs_real_t  twall[],
                              const cs_real_t  qincid[],
                              const cs_real_t  xlam[],
                              const cs_real_t  epa[],
                              const cs_real_t  eps[],
                              const cs_real_t  ck[],
                              cs_real_t        net_flux[])
{
  /*< [loc_var_dec_2]*/
  cs_real_t  stephn = 5.6703e-8;
  /*< [loc_var_dec_2]*/

  /*< [init]*/
  /* Initializations */

  /* Stop indicator (forgotten boundary faces)     */
  int iok = 0;
  /*< [init]*/

  /*< [net_flux]*/
  /* Net flux dendity for the boundary faces
   * The provided examples are sufficient in most of cases.*/

  /* If the boundary conditions given above have been modified
   *   it is necessary to change the way in which density is calculated from
   *   the net radiative flux consistently.*/

  /* The rule is:
   *   the density of net flux is a balance between the emitting energy from a
   *   boundary face (and not the reflecting energy) and the absorbing radiative
   *   energy. Therefore if a wall heats the fluid by radiative transfer, the
   *   net flux is negative */

  for (cs_lnum_t ifac = 0; ifac < cs_glob_mesh->n_b_faces; ifac++) {

    /* Wall faces */
    if (   itypfb[ifac] == CS_SMOOTHWALL
        || itypfb[ifac] == CS_ROUGHWALL)
      net_flux[ifac] = eps[ifac] * (qincid[ifac] - stephn * pow(twall[ifac], 4));

    /* Symmetry   */
    else if (itypfb[ifac] == CS_SYMMETRY)
      net_flux[ifac] = 0.0;

    /* Inlet/Outlet    */
    else if (   itypfb[ifac] == CS_INLET
             || itypfb[ifac] == CS_CONVECTIVE_INLET
             || itypfb[ifac] == CS_OUTLET
             || itypfb[ifac] == CS_FREE_INLET) {
      if (cs_glob_rad_transfer_params->iirayo == 1)
        net_flux[ifac] = qincid[ifac] - cs_math_pi * coefap[ifac];
      else if (cs_glob_rad_transfer_params->iirayo == 2)
        net_flux[ifac] = 0.0;
    }
    /* Stop if there are forgotten faces   */
    else
      bft_error
        (__FILE__, __LINE__, 0,
         "In %s:\n"
         "  non-handled boundary faces for net flux calculation\n\n"
         "  Last face: %10d; zone = %d; nature = %d\n",
         __func__,
         ifac,
         izfrdp[ifac],
         itypfb[ifac]);

  }
  /*< [net_flux]*/
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
