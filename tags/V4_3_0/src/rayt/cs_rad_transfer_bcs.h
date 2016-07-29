#ifndef __CS_RAD_TRANSFER_BCS_H__
#define __CS_RAD_TRANSFER_BCS_H__

/*============================================================================
 * Radiation solver boundary conditions treatment.
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

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Local Macro definitions
 *============================================================================*/

/*============================================================================
 * Type definition
 *============================================================================*/

/*============================================================================
 *  Global variables
 *============================================================================*/

/*============================================================================
 * Public function prototypes for Fortran API
 *============================================================================*/

/*=============================================================================
 * Public function prototypes
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute wall temperature for radiative transfer, and update BCs.
 *
 *   1) Compute wall temperature for radiative transfer
 *
 *   2) Update BCs for the energy computation
 *
 *   \param[in]     nvarcl        total number of variable BC's
 *   \param[in,out] icodcl        face boundary condition code:
 *                                 - 1 Dirichlet
 *                                 - 2 Radiative outlet
 *                                 - 3 Neumann
 *                                 - 4 sliding and
 *                                   \f$ \vect{u} \cdot \vect{n} = 0 \f$
 *                                 - 5 smooth wall and
 *                                   \f$ \vect{u} \cdot \vect{n} = 0 \f$
 *                                 - 6 rough wall and
 *                                   \f$ \vect{u} \cdot \vect{n} = 0 \f$
 *                                 - 9 free inlet/outlet
 *                                   (input mass flux blocked to 0)
 *                                 - 13 Dirichlet for the advection operator and
 *                                      Neumann for the diffusion operator
 *   \param[in]     bc_type       face boundary condition type
 *   \param[in]     izfrad        zone index for boundary faces
 *                                 and reference face index
 *   \param[in]     nozppm        max number of boundary conditions zone
 *   \param[in]     dt            time step (per cell)
 *   \param[in,out] rcodcl        boundary condition values:
 *                                 - rcodcl(1) value of the dirichlet
 *                                 - rcodcl(2) value of the exterior exchange
 *                                   coefficient (infinite if no exchange)
 *                                 - rcodcl(3) value flux density
 *                                   (negative if gain) in w/m2 or roughness
 *                                   in m if icodcl=6
 *                                   -# for the velocity \f$ (\mu+\mu_T)
 *                                      \gradv \vect{u} \cdot \vect{n}  \f$
 *                                   -# for the pressure \f$ \Delta t
 *                                      \grad P \cdot \vect{n}  \f$
 *                                   -# for a scalar \f$ cp \left( K +
 *                                       \dfrac{K_T}{\sigma_T} \right)
 *                                       \grad T \cdot \vect{n} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_rad_transfer_bcs(int         nvarcl,
                    int         bc_type[],
                    int         icodcl[],
                    int         izfrad[],
                    int        *nozppm,
                    cs_real_t   dt[],
                    cs_real_t   rcodcl[]);

/*----------------------------------------------------------------------------*/
/*!
 * \brief Boundary conditions for DO and P-1 models
 *
 * 1. Boundary conditions for the radiative intensity (DO model)
 * --------------------------------------------------------------
 *      The array coefap stores the intensity for each boundary faces,
 *        depending of the natur of the boundary (Dirichlet condition).
 *      The intensity of radiation is defined as the rate of emitted
 *        energy from unit surface area through unit solid angle.
 *
 *   1/ Gray wall: isotropic radiation field.
 *                                   4
 *                     eps.sig.tparoi         (1-eps).qincid
 *       coefap   =    --------------    +    --------------
 *                           pi                     pi
 *   wall intensity     wall emission           reflecting flux.
 *      (eps=1: black wall; eps=0: reflecting wall)
 *   2/ Free boundary: entering intensity is fixed to zero
 *         coefap   =   0.d0
 *     (if the user has more information, he can do something better)
 *
 * 2. Boundary conditions for the P-1 model
 * ----------------------------------------
 *
 * \param[in]  bc_type         boundary face types
 * \param[out] izfrdp          boundary faces -> zone number
 * \param[out] coefap, coefbp  boundary conditions for intensity or P-1 model
 *             cofafp, cofbfp
 * \param[in]  tparoi          inside current wall temperature (K)
 * \param[in]  ckmel           coeff d'absorption du melange
 *                               gaz-particules de charbon
 * \param[in]  abo             Wnights of the i-th gray gas at boundaries
 * \param[in]  iband           number of the i-th grey gas
 */
/*----------------------------------------------------------------------------*/

void
cs_rad_transfer_bc_coeffs(int        bc_type[],
                          cs_real_t  coefap[],
                          cs_real_t  coefbp[],
                          cs_real_t  cofafp[],
                          cs_real_t  cofbfp[],
                          cs_real_t  tparoi[],
                          cs_real_t  ckmel[],
                          cs_real_t  abo[],
                          int        iband);

/*----------------------------------------------------------------------------*/

END_C_DECLS

#endif /* __CS_RAD_TRANSFER_BCS_H__ */
