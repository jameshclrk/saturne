/*============================================================================
 * Definition of the calculation mesh.
 *
 * Mesh-related user functions (called in this order):
 *   Manage the exchange of data between Code_Saturne and the pre-processor
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
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "bft_error.h"
#include "bft_mem.h"
#include "bft_printf.h"

#include "fvm_defs.h"
#include "fvm_selector.h"

#include "cs_base.h"
#include "cs_join.h"
#include "cs_join_perio.h"
#include "cs_mesh.h"
#include "cs_mesh_quantities.h"
#include "cs_mesh_bad_cells.h"
#include "cs_mesh_extrude.h"
#include "cs_mesh_smoother.h"
#include "cs_mesh_thinwall.h"
#include "cs_mesh_warping.h"
#include "cs_parall.h"
#include "cs_post.h"
#include "cs_preprocessor_data.h"
#include "cs_selector.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_prototypes.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*----------------------------------------------------------------------------*/
/*!
 * \file cs_user_mesh.c
 *
 * \brief Mesh input definition and mesh saving examples.
 *
 * See \subpage cs_user_mesh for examples.
 */
/*----------------------------------------------------------------------------*/

/*============================================================================
 * User function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Define mesh files to read and optional associated transformations.
 */
/*----------------------------------------------------------------------------*/

void
cs_user_mesh_input(void)
{
  BEGIN_EXAMPLE_SCOPE

  /*! [mesh_input_1] */

  /* Determine list of files to add */
  /*--------------------------------*/

  /* Read input mesh with no modification */

  cs_preprocessor_data_add_file("mesh_input/mesh_01", 0, NULL, NULL);

  /*! [mesh_input_1] */

  END_EXAMPLE_SCOPE

  BEGIN_EXAMPLE_SCOPE

  /*! [mesh_input_2] */

  /* Add same mesh with transformations */
  const char *renames[] = {"Inlet", "Injection_2",
                           "Group_to_remove", NULL};
  const double transf_matrix[3][4] = {{1., 0., 0., 5.},
                                      {0., 1., 0., 0.},
                                      {0., 0., 1., 0.}};

  cs_preprocessor_data_add_file("mesh_input/mesh_02",
                                2, renames,
                                transf_matrix);
  /*! [mesh_input_2] */

  END_EXAMPLE_SCOPE
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Enable or disable mesh saving.
 *
 * By default, mesh is saved when modified.
 *
 * \param[in,out] mesh  pointer to a cs_mesh_t structure
*/
/*----------------------------------------------------------------------------*/

void
cs_user_mesh_save(cs_mesh_t  *mesh)
{
  BEGIN_EXAMPLE_SCOPE

  /*! [mesh_save] */

  /* Mark mesh as not modified (0) to disable saving;
     Mark it as modified (> 0) to force saving */

  mesh->modified = 0;

  /*! [mesh_save] */

  END_EXAMPLE_SCOPE
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
