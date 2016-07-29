/*============================================================================
 * All-to-all parallel data exchange.
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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"
#include "bft_error.h"
#include "bft_printf.h"

#include "cs_assert.h"
#include "cs_block_dist.h"
#include "cs_crystal_router.h"
#include "cs_log.h"
#include "cs_order.h"
#include "cs_timer.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_all_to_all.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Additional Doxygen documentation
 *============================================================================*/

/*!
  \file cs_all_to_all.c
        All-to-all parallel data exchange.

  \typedef cs_all_to_all_t
        Opaque all-to-all distribution structure

  \enum cs_all_to_all_type_t

  \brief All-to-all algorithm selection

  \var CS_ALL_TO_ALL_MPI_DEFAULT
       Use MPI_Alltoall and MPI_Alltoallv sequences

  \var CS_ALL_TO_ALL_CRYSTAL_ROUTER
       Use crystal router algorithm

  \paragraph all_to_all_flags Using flags
  \parblock

  Flags are defined as a sum (bitwise or) of constants, which may include
  \ref CS_ALL_TO_ALL_ORDER_BY_DEST_ID, \ref CS_ALL_TO_ALL_ORDER_BY_SRC_RANK,
  \ref CS_ALL_TO_ALL_NO_REVERSE, and \ref CS_ALL_TO_ALL_USE_SRC_RANK.

  \endparblock
*/

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*=============================================================================
 * Macro definitions
 *============================================================================*/

/*=============================================================================
 * Local type definitions
 *============================================================================*/

#if defined(HAVE_MPI)

typedef enum {

  CS_ALL_TO_ALL_TIME_TOTAL,
  CS_ALL_TO_ALL_TIME_METADATA,
  CS_ALL_TO_ALL_TIME_EXCHANGE

} cs_all_to_all_timer_t;

typedef struct {

  cs_datatype_t   datatype;          /* associated datatype */
  cs_datatype_t   dest_id_datatype;  /* type of destination id (CS_LNUM_TYPE,
                                        or CS_DATATYPE_NULL) */

  size_t          stride;            /* stride if strided, 0 otherwise */

  size_t          elt_shift;         /* starting byte for element data */
  size_t          comp_size;         /* Composite element size, with padding */

  size_t          send_size;         /* Send buffer element count */
  size_t          recv_size;         /* Receive buffer element count */

  const void     *send_buffer;       /* Send buffer */
  unsigned char  *_send_buffer;      /* Send buffer */

  int            *send_count;        /* Send counts for MPI_Alltoall */
  int            *recv_count;        /* Receive counts for MPI_Alltoall */
  int            *send_displ;        /* Send displs for MPI_Alltoall */
  int            *recv_displ;        /* Receive displs for MPI_Alltoall */

  MPI_Comm        comm;              /* Associated MPI communicator */
  MPI_Datatype    comp_type;         /* Associated MPI datatype */
  int             rank_id;           /* local rank id in comm */

  int             n_ranks;           /* Number of ranks associated with
                                        communicator */

} _mpi_all_to_all_caller_t;

#endif /* defined(HAVE_MPI) */

/* Structure used to redistribute data */

#if defined(HAVE_MPI)

struct _cs_all_to_all_t {

  cs_lnum_t                  n_elts_src;   /* Number of source elements */
  cs_lnum_t                  n_elts_dest;  /* Number of destination elements
                                              (-1 before metadata available) */

  int                        flags;        /* option flags */

  /* Send metadata */

  const int                 *dest_rank;    /* optional element destination
                                              rank (possibly shared) */
  int                       *_dest_rank;   /* dest_rank if owner, or NULL */

  const cs_lnum_t           *dest_id;      /* optional element destination id
                                              (possibly shared) */
  cs_lnum_t                 *_dest_id;     /* dest_id if owner, or NULL */

  /* Receive metadata */

  cs_lnum_t                 *recv_id;      /* received match for dest_id */

  /* Data needed only for Crystal Router reverse communication */

  cs_lnum_t                 *src_id;       /* received match for dest_id */
  int                       *src_rank;     /* received source rank */

  /* Sub-structures */

  _mpi_all_to_all_caller_t  *dc;       /* Default MPI_Alltoall(v) caller */
  cs_crystal_router_t       *cr;       /* associated crystal-router */

  /* MPI data */

  int                        n_ranks;      /* Number of associated ranks */
  MPI_Comm                   comm;         /* associated communicator */

  /* Other metadata */

  cs_all_to_all_type_t       type;         /* Communication protocol */

};

#endif /* defined(HAVE_MPI) */

/*============================================================================
 * Static global variables
 *============================================================================*/

/* Default all-to-all type */

static cs_all_to_all_type_t _all_to_all_type = CS_ALL_TO_ALL_MPI_DEFAULT;

#if defined(HAVE_MPI)

/* Call counter and timer: 0: total, 1: metadata comm, 2: data comm */

static size_t              _all_to_all_calls[3] = {0, 0, 0};
static cs_timer_counter_t  _all_to_all_timers[3];

#endif /* defined(HAVE_MPI) */

/*============================================================================
 * Local function defintions
 *============================================================================*/

#if defined(HAVE_MPI)

/*----------------------------------------------------------------------------
 * Common portion of different all-to-all distributor contructors.
 *
 * arguments:
 *   n_elts <-- number of elements
 *   flags  <-- sum of ordering and metadata flag constants
 *   comm   <-- associated MPI communicator
 *
 * returns:
 *   pointer to new all-to-all distributor
 *----------------------------------------------------------------------------*/

static cs_all_to_all_t *
_all_to_all_create_base(size_t    n_elts,
                        int       flags,
                        MPI_Comm  comm)
{
  cs_all_to_all_t *d;

  /* Initialize timers if required */

  if (_all_to_all_calls[0] == 0) {
    int i;
    int n_timers = sizeof(_all_to_all_timers)/sizeof(_all_to_all_timers[0]);
    for (i = 0; i < n_timers; i++)
      CS_TIMER_COUNTER_INIT(_all_to_all_timers[i]);
  }

  /* Check flags */

  if (   (flags & CS_ALL_TO_ALL_ORDER_BY_DEST_ID)
      && (flags & CS_ALL_TO_ALL_ORDER_BY_SRC_RANK))
    bft_error(__FILE__, __LINE__, 0,
              "%s: flags may not match both\n"
              "CS_ALL_TO_ALL_ORDER_BY_DEST_ID and\n"
              "CS_ALL_TO_ALL_ORDER_BY_SRC_RANK.",
              __func__);

  /* Allocate structure */

  BFT_MALLOC(d, 1, cs_all_to_all_t);

  /* Create associated sub-structure */

  d->n_elts_src = n_elts;
  d->n_elts_dest = -1; /* undetermined as yet */

  d->flags = flags;

  d->dest_rank = NULL;
  d->_dest_rank = NULL;

  d->dest_id = NULL;
  d->_dest_id = NULL;

  d->recv_id = NULL;

  d->src_id = NULL;
  d->src_rank = NULL;

  d->cr = NULL;
  d->dc = NULL;

  d->comm = comm;
  MPI_Comm_size(comm, &(d->n_ranks));

  d->type = _all_to_all_type;

  return d;
}

/*----------------------------------------------------------------------------
 * Compute rank displacement based on count.
 *
 * arguments:
 *   n_ranks <-- number of ranks
 *   count   <-- number of elements per rank (size: n_ranks)
 *   displ   --> element displacement in cumulative array (size: n_ranks)
 *
 * returns:
 *   cumulative count for all ranks
 *----------------------------------------------------------------------------*/

static cs_lnum_t
_compute_displ(int        n_ranks,
               const int  count[],
               int        displ[])
{
  int i;
  cs_lnum_t total_count = 0;

  displ[0] = 0;

  for (i = 0; i < n_ranks; i++)
    displ[i+1] = displ[i] + count[i];

  total_count = displ[n_ranks-1] + count[n_ranks-1];

  return total_count;
}

/*----------------------------------------------------------------------------
 * First stage of creation for an MPI_Alltoall(v) caller for strided data.
 *
 * parameters:
 *   flags     <-- metadata flags
 *   comm      <-- associated MPI communicator
 *---------------------------------------------------------------------------*/

static _mpi_all_to_all_caller_t *
_alltoall_caller_create_meta(int        flags,
                             MPI_Comm   comm)
{
  _mpi_all_to_all_caller_t *dc;

  /* Allocate structure */

  BFT_MALLOC(dc, 1, _mpi_all_to_all_caller_t);

  dc->datatype = CS_DATATYPE_NULL;

  dc->dest_id_datatype = CS_DATATYPE_NULL;

  if (flags & CS_ALL_TO_ALL_ORDER_BY_DEST_ID)
    dc->dest_id_datatype = CS_LNUM_TYPE;

  dc->stride = 0;

  dc->send_size = 0;
  dc->recv_size = 0;

  dc->comm = comm;

  MPI_Comm_rank(comm, &(dc->rank_id));
  MPI_Comm_size(comm, &(dc->n_ranks));

  dc->send_buffer = NULL;
  dc->_send_buffer = NULL;

  BFT_MALLOC(dc->send_count, dc->n_ranks, int);
  BFT_MALLOC(dc->recv_count, dc->n_ranks, int);
  BFT_MALLOC(dc->send_displ, dc->n_ranks + 1, int);
  BFT_MALLOC(dc->recv_displ, dc->n_ranks + 1, int);

  /* Compute data size and alignment */

  if (dc->dest_id_datatype == CS_LNUM_TYPE)
    dc->elt_shift = sizeof(cs_lnum_t);
  else
    dc->elt_shift = 0;

  size_t align_size = sizeof(cs_lnum_t);

  if (dc->elt_shift % align_size)
    dc->elt_shift += align_size - (dc->elt_shift % align_size);

  dc->comp_size = dc->elt_shift;

  dc->comp_type = MPI_BYTE;

  /* Return pointer to structure */

  return dc;
}

/*----------------------------------------------------------------------------
 * Update all MPI_Alltoall(v) caller metadata for strided data.
 *
 * parameters:
 *   dc        <-> distributor caller
 *   datatype  <-- associated datatype
 *   stride    <-- associated stride
 *---------------------------------------------------------------------------*/

static void
_alltoall_caller_update_meta_s(_mpi_all_to_all_caller_t  *dc,
                               cs_datatype_t              datatype,
                               int                        stride)
{
  size_t elt_size = cs_datatype_size[datatype]*stride;

  /* Free previous associated datatype if needed */

  if (dc->comp_type != MPI_BYTE)
    MPI_Type_free(&(dc->comp_type));

  /* Now update metadata */

  dc->datatype = datatype;
  dc->stride = stride;

  /* Recompute data size and alignment */

  size_t align_size = sizeof(cs_lnum_t);
  if (cs_datatype_size[datatype] > align_size)
    align_size = cs_datatype_size[datatype];

  if (dc->dest_id_datatype == CS_LNUM_TYPE)
    dc->elt_shift = sizeof(cs_lnum_t);
  else
    dc->elt_shift = 0;

  if (dc->elt_shift % align_size)
    dc->elt_shift += align_size - (dc->elt_shift % align_size);

  dc->comp_size = dc->elt_shift + elt_size;

  if (elt_size % align_size)
    dc->comp_size += align_size - (elt_size % align_size);;

  /* Update associated MPI datatype */

  MPI_Type_contiguous(dc->comp_size, MPI_BYTE, &(dc->comp_type));
  MPI_Type_commit(&(dc->comp_type));
}

/*----------------------------------------------------------------------------
 * Swap source and destination ranks of all-to-all distributor.
 *
 * parameters:
 *   d <->  pointer to associated all-to-all distributor
 *----------------------------------------------------------------------------*/

static void
_alltoall_caller_swap_src_dest(_mpi_all_to_all_caller_t  *dc)
{
  size_t tmp_size[2] = {dc->send_size, dc->recv_size};
  int *tmp_count = dc->recv_count;
  int *tmp_displ = dc->recv_displ;

  dc->send_size = tmp_size[1];
  dc->recv_size = tmp_size[0];

  dc->recv_count = dc->send_count;
  dc->recv_displ = dc->send_displ;

  dc->send_count = tmp_count;
  dc->send_displ = tmp_displ;
}

/*----------------------------------------------------------------------------
 * Prepare a MPI_Alltoall(v) caller for strided data.
 *
 * parameters:
 *   n_elts           <-- number of elements
 *   stride           <-- number of values per entity (interlaced)
 *   datatype         <-- type of data considered
 *   elt              <-- element values
 *   dest_id          <-- element destination id, or NULL
 *   recv_id          <-- element receive id (for reverse mode), or NULL
 *   dest_rank        <-- destination rank for each element
 *---------------------------------------------------------------------------*/

static void
_alltoall_caller_prepare_s(_mpi_all_to_all_caller_t  *dc,
                           size_t                     n_elts,
                           int                        stride,
                           cs_datatype_t              datatype,
                           bool                       reverse,
                           const void                *data,
                           const cs_lnum_t           *dest_id,
                           const cs_lnum_t           *recv_id,
                           const int                  dest_rank[])
{
  int i;

  size_t elt_size = cs_datatype_size[datatype]*stride;

  unsigned const char *_data = data;

  assert(data != NULL || (n_elts == 0 || stride == 0));

  _alltoall_caller_update_meta_s(dc, datatype, stride);

  /* Allocate send buffer */

  if (reverse && recv_id == NULL) {
    BFT_FREE(dc->_send_buffer);
    dc->send_buffer = data;
  }
  else {
    BFT_REALLOC(dc->_send_buffer, dc->send_size*dc->comp_size, unsigned char);
    dc->send_buffer = dc->_send_buffer;
  }

  /* Copy metadata */

  if (dc->dest_id_datatype == CS_LNUM_TYPE) {
    unsigned const char *_dest_id = (unsigned const char *)dest_id;
    const size_t id_size = sizeof(cs_lnum_t);
    for (size_t j = 0; j < n_elts; j++) {
      size_t w_displ = dc->send_displ[dest_rank[j]]*dc->comp_size;
      size_t r_displ = j*id_size;
      dc->send_displ[dest_rank[j]] += 1;
      for (size_t k = 0; k < id_size; k++)
        dc->_send_buffer[w_displ + k] = _dest_id[r_displ + k];
    }
    /* Reset send_displ */
    for (i = 0; i < dc->n_ranks; i++)
      dc->send_displ[i] -= dc->send_count[i];
  }

  /* Copy data; in case of reverse send with destination ids, the
     matching indirection must be applied here. */

  if (!reverse) {
    for (size_t j = 0; j < n_elts; j++) {
      size_t w_displ =   dc->send_displ[dest_rank[j]]*dc->comp_size
                       + dc->elt_shift;
      size_t r_displ = j*elt_size;
      dc->send_displ[dest_rank[j]] += 1;
      for (size_t k = 0; k < elt_size; k++)
        dc->_send_buffer[w_displ + k] = _data[r_displ + k];
    }
    /* Reset send_displ */
    for (i = 0; i < dc->n_ranks; i++)
      dc->send_displ[i] -= dc->send_count[i];
  }
  else if (recv_id != NULL) { /* reverse here */
    for (size_t j = 0; j < dc->send_size; j++) {
      size_t w_displ = dc->comp_size*j + dc->elt_shift;
      size_t r_displ = recv_id[j]*elt_size;
      for (size_t k = 0; k < elt_size; k++)
        dc->_send_buffer[w_displ + k] = _data[r_displ + k];
    }
  }
  else { /* If revert and no recv_id */
    assert(dc->send_buffer == data);
  }
}

/*----------------------------------------------------------------------------
 * Destroy a MPI_Alltoall(v) caller.
 *
 * parameters:
 *   dc <-> pointer to pointer to MPI_Alltoall(v) caller structure
 *---------------------------------------------------------------------------*/

static void
_alltoall_caller_destroy(_mpi_all_to_all_caller_t **dc)
{
  if (dc != NULL) {
    _mpi_all_to_all_caller_t *_dc = *dc;
    if (_dc->comp_type != MPI_BYTE)
      MPI_Type_free(&(_dc->comp_type));
    BFT_FREE(_dc->_send_buffer);
    BFT_FREE(_dc->recv_displ);
    BFT_FREE(_dc->send_displ);
    BFT_FREE(_dc->recv_count);
    BFT_FREE(_dc->send_count);
    BFT_FREE(*dc);
  }
}

/*----------------------------------------------------------------------------
 * Exchange metadata for an MPI_Alltoall(v) caller.
 *
 * Order of data from a same source rank is preserved.
 *
 * parameters:
 *   dc        <-> associated MPI_Alltoall(v) caller structure
 *   n_elts    <-- number of elements
 *   dest_rank <-- destination rank for each element
 *---------------------------------------------------------------------------*/

static  void
_alltoall_caller_exchange_meta(_mpi_all_to_all_caller_t  *dc,
                               size_t                     n_elts,
                               const int                  dest_rank[])
{
  /* Count values to send and receive */

  for (int i = 0; i < dc->n_ranks; i++)
    dc->send_count[i] = 0;

  for (size_t j = 0; j < n_elts; j++)
    dc->send_count[dest_rank[j]] += 1;

  dc->send_size = _compute_displ(dc->n_ranks, dc->send_count, dc->send_displ);

  /* Exchange counts */

  cs_timer_t t0 = cs_timer_time();

  MPI_Alltoall(dc->send_count, 1, MPI_INT,
               dc->recv_count, 1, MPI_INT,
               dc->comm);

  cs_timer_t t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_METADATA,
                            &t0, &t1);
  _all_to_all_calls[CS_ALL_TO_ALL_TIME_METADATA] += 1;

  dc->recv_size = _compute_displ(dc->n_ranks, dc->recv_count, dc->recv_displ);
}

/*----------------------------------------------------------------------------
 * Exchange strided data with a MPI_Alltoall(v) caller.
 *
 * parameters:
 *   d         <-> pointer to associated all-to-all distributor
 *   dc        <-> associated MPI_Alltoall(v) caller structure
 *   dest_data <-> destination data buffer, or NULL
 *
 * returns:
 *   pointer to dest_data, or newly allocated buffer
 *---------------------------------------------------------------------------*/

static  void *
_alltoall_caller_exchange_s(cs_all_to_all_t           *d,
                            _mpi_all_to_all_caller_t  *dc,
                            void                      *dest_data)
{
  size_t elt_size = cs_datatype_size[dc->datatype]*dc->stride;
  unsigned char *_dest_data = dest_data, *_recv_data = dest_data;

  /* Final data buffer */

  if (_dest_data == NULL && dc->recv_size*elt_size > 0)
    BFT_MALLOC(_dest_data, dc->recv_size*elt_size, unsigned char);

  /* Data buffer for MPI exchange (may merge data and metadata) */
  if (dc->dest_id_datatype == CS_LNUM_TYPE || d->dest_id != NULL)
    BFT_MALLOC(_recv_data, dc->recv_size*dc->comp_size, unsigned char);
  else
    _recv_data = _dest_data;

  cs_timer_t t0 = cs_timer_time();

  MPI_Alltoallv(dc->send_buffer, dc->send_count, dc->send_displ, dc->comp_type,
                _recv_data, dc->recv_count, dc->recv_displ, dc->comp_type,
                dc->comm);

  cs_timer_t t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_EXCHANGE,
                            &t0, &t1);
  _all_to_all_calls[CS_ALL_TO_ALL_TIME_EXCHANGE] += 1;

  /* dest id datatype only used for first exchange */

  if (dc->dest_id_datatype == CS_LNUM_TYPE) {
    assert(d->recv_id == NULL);
    BFT_MALLOC(d->recv_id, d->dc->recv_size, cs_lnum_t);
    const unsigned char *sp = _recv_data;
    for (size_t i = 0; i < d->dc->recv_size; i++)
      memcpy(d->recv_id + i,
             sp + d->dc->comp_size*i,
             sizeof(cs_lnum_t));
    dc->dest_id_datatype = CS_DATATYPE_NULL;
  }

  /* Now handle main data buffer */

  if (_dest_data != _recv_data) {
    const unsigned char *sp = _recv_data + d->dc->elt_shift;
    if (d->recv_id != NULL) {
      for (size_t i = 0; i < d->dc->recv_size; i++) {
        size_t w_displ = d->recv_id[i]*elt_size;
        size_t r_displ = d->dc->comp_size*i;
        for (size_t j = 0; j < elt_size; j++)
          _dest_data[w_displ + j] = sp[r_displ + j];
      }
    }
    else {
      for (size_t i = 0; i < d->dc->recv_size; i++) {
        size_t w_displ = i*elt_size;
        size_t r_displ = dc->comp_size*i;
        for (size_t j = 0; j < elt_size; j++)
          _dest_data[w_displ + j] = sp[r_displ + j];
      }
    }
    BFT_FREE(_recv_data);
  }

  return _dest_data;
}

#endif /* defined(HAVE_MPI) */

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public function definitions
 *============================================================================*/

#if defined(HAVE_MPI)

/*----------------------------------------------------------------------------*/
/*!
 * \brief Create an all-to-all distributor based on destination rank.
 *
 * This is a collective operation on communicator comm.
 *
 * If the flags bit mask matches \ref CS_ALL_TO_ALL_ORDER_BY_DEST_ID,
 * data exchanged will be ordered by the array passed to the
 * \c dest_id argument. For \c n total values received on a rank
 * (as given by \ref cs_all_to_all_n_elts_dest), those destination ids
 * must be in the range [0, \c n[.
 *
 * If the flags bit mask matches \ref CS_ALL_TO_ALL_ORDER_BY_SRC_RANK,
 * data exchanged will be ordered by source rank (this is incompatible
 * with \ref CS_ALL_TO_ALL_ORDER_BY_DEST_ID.
 *
 * \attention
 * The \c dest_rank and \c dest_id arrays are only referenced by
 * the distributor, not copied, and must remain available throughout
 * the distributor's lifetime. They may be fully transferred to
 * the structure if not needed elsewhere using the
 * \ref cs_all_to_all_transfer_dest_rank and
 * \ref cs_all_to_all_transfer_dest_id functions.
 *
 * \param[in]  n_elts       number of elements
 * \param[in]  flags        sum of ordering and metadata flag constants
 * \param[in]  dest_id      element destination id (required if flags
 *                          contain \ref CS_ALL_TO_ALL_ORDER_BY_DEST_ID),
 *                          or NULL
 * \param[in]  dest_rank    destination rank for each element
 * \param[in]  comm         associated MPI communicator
 *
 * \return  pointer to new all-to-all distributor
 */
/*----------------------------------------------------------------------------*/

cs_all_to_all_t *
cs_all_to_all_create(size_t            n_elts,
                     int               flags,
                     const cs_lnum_t  *dest_id,
                     const int         dest_rank[],
                     MPI_Comm          comm)
{
  cs_timer_t t0, t1;

  t0 = cs_timer_time();

  cs_all_to_all_t *d = _all_to_all_create_base(n_elts,
                                               flags,
                                               comm);

  d->dest_id = dest_id;
  d->dest_rank = dest_rank;

  /* Create substructures based on info available at this stage
     (for Crystal Router, delay creation as data is not passed yet) */

  if (d->type == CS_ALL_TO_ALL_MPI_DEFAULT)
    d->dc = _alltoall_caller_create_meta(flags, comm);

  t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                            &t0, &t1);
  _all_to_all_calls[CS_ALL_TO_ALL_TIME_TOTAL] += 1;

  return d;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Create an all-to-all distributor for elements whose destination
 * rank is determined from global numbers and block distribution information.
 *
 * This is a collective operation on communicator comm.
 *
 * If the flags bit mask matches \ref CS_ALL_TO_ALL_ORDER_BY_DEST_ID,
 * data exchanged will be ordered by global element number.
 *
 * If the flags bit mask matches \ref CS_ALL_TO_ALL_ORDER_BY_SRC_RANK,
 * data exchanged will be ordered by source rank (this is incompatible
 * with \ref CS_ALL_TO_ALL_ORDER_BY_DEST_ID.
 *
 * \param[in]  n_elts       number of elements
 * \param[in]  flags        sum of ordering and metadata flag constants
 * \param[in]  src_gnum     global source element numbers
 * \param[in]  bi           destination block distribution info
 * \param[in]  comm         associated MPI communicator
 *
 * \return  pointer to new all-to-all distributor
 */
/*----------------------------------------------------------------------------*/

cs_all_to_all_t *
cs_all_to_all_create_from_block(size_t                 n_elts,
                                int                    flags,
                                const cs_gnum_t       *src_gnum,
                                cs_block_dist_info_t   bi,
                                MPI_Comm               comm)
{
  cs_timer_t t0, t1;

  t0 = cs_timer_time();

  cs_all_to_all_t *d = _all_to_all_create_base(n_elts,
                                               flags,
                                               comm);

  /* Compute arrays based on block distribution
     (using global ids and block info at lower levels could
     save some memory, but would make things less modular;
     in addition, rank is an int, while global ids are usually
     64-bit uints, so the overhead is only 50%, and access should
     be faster). */

  BFT_MALLOC(d->_dest_rank, n_elts, int);
  d->dest_rank = d->_dest_rank;

  if (flags & CS_ALL_TO_ALL_ORDER_BY_DEST_ID) {
    BFT_MALLOC(d->_dest_id, n_elts, cs_lnum_t);
    d->dest_id = d->_dest_id;
  }

  const int rank_step = bi.rank_step;
  const cs_gnum_t block_size = bi.block_size;

  if (d->_dest_id != NULL) {
    #pragma omp parallel for if (n_elts > CS_THR_MIN)
    for (size_t i = 0; i < n_elts; i++) {
      cs_gnum_t g_elt_id = src_gnum[i] -1;
      cs_gnum_t _dest_rank = g_elt_id / block_size;
      d->_dest_rank[i] = _dest_rank*rank_step;
      d->_dest_id[i]   = g_elt_id % block_size;;
    }
  }
  else {
    #pragma omp parallel for if (n_elts > CS_THR_MIN)
    for (size_t i = 0; i < n_elts; i++) {
      cs_gnum_t g_elt_id = src_gnum[i] -1;
      cs_gnum_t _dest_rank = g_elt_id / block_size;
      d->_dest_rank[i] = _dest_rank*rank_step;
    }
  }

  /* Create substructures based on info available at this stage
     (for Crystal Router, delay creation as data is not passed yet) */

  if (d->type == CS_ALL_TO_ALL_MPI_DEFAULT)
    d->dc = _alltoall_caller_create_meta(flags, comm);

  t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                            &t0, &t1);
  _all_to_all_calls[CS_ALL_TO_ALL_TIME_TOTAL] += 1;

  return d;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Destroy an all-to-all distributor.
 *
 * \param[in, out]  d   pointer to associated all-to-all distributor
 */
/*----------------------------------------------------------------------------*/

void
cs_all_to_all_destroy(cs_all_to_all_t **d)
{
  if (d != NULL) {

    cs_timer_t t0, t1;

    t0 = cs_timer_time();

    cs_all_to_all_t *_d = *d;
    if (_d->cr != NULL)
      cs_crystal_router_destroy(&(_d->cr));
    else if (_d->dc != NULL) {
      _alltoall_caller_destroy(&(_d->dc));
    }

    BFT_FREE(_d->src_rank);
    BFT_FREE(_d->src_id);

    BFT_FREE(_d->_dest_id);
    BFT_FREE(_d->_dest_rank);
    BFT_FREE(_d);

    t1 = cs_timer_time();
    cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                              &t0, &t1);

    /* no increment to _all_to_all_calls[0] as create/destroy are grouped */
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Transfer ownership of destination rank to an all-to-all distributor.
 *
 * The dest_rank array should be the same as the one used for the creation of
 * the distributor.
 *
 * \param[in, out]  d          pointer to associated all-to-all distributor
 * \param[in, out]  dest_rank  pointer to element destination rank
 */
/*----------------------------------------------------------------------------*/

void
cs_all_to_all_transfer_dest_rank(cs_all_to_all_t   *d,
                                 cs_lnum_t        **dest_rank)
{
  cs_assert(d != NULL);

  if (d->dest_rank == *dest_rank) {
    d->_dest_rank = *dest_rank;
    *dest_rank  = NULL;
  }
  else {
    bft_error(__FILE__, __LINE__, 0,
              "%s: array transferred (%p)does not match the one used\n"
              "for all-to-all distributor creation (%p).",
              __func__, (void *)*dest_rank, (const void *)d->dest_rank);
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Transfer ownership of destination ids to an
 *        all-to-all distributor.
 *
 * The dest_id array should be the same as the one used for the creation of
 * the distributor.
 *
 * \param[in, out]  d        pointer to associated all-to-all distributor
 * \param[in, out]  dest_id  pointer to element destination id
 */
/*----------------------------------------------------------------------------*/

void
cs_all_to_all_transfer_dest_id(cs_all_to_all_t   *d,
                               cs_lnum_t        **dest_id)
{
  cs_assert(d != NULL);

  if (d->dest_id == *dest_id) {
    d->_dest_id = *dest_id;
    *dest_id  = NULL;
  }
  else {
    bft_error(__FILE__, __LINE__, 0,
              "%s: array transferred (%p)does not match the one used\n"
              "for all-to-all distributor creation (%p).",
              __func__, (void *)*dest_id, (const void *)d->dest_id);
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Get number of elements associated with all-to-all distributor.
 *
 * The number of elements is the number of elements received after exchange.
 *
 * If no exchange has been done yet (depending on the communication protocol),
 * metadata will be exchanged by this call, so it is a collective operation.
 *
 * \param[in]  d   pointer to associated all-to-all distributor
 *
 * \return  number of elements associated with distributor.
 */
/*----------------------------------------------------------------------------*/

cs_lnum_t
cs_all_to_all_n_elts_dest(cs_all_to_all_t  *d)
{
  cs_assert(d != NULL);

  /* Obtain count if not available yet */

  if (d->n_elts_dest < 0) {

    cs_timer_t t0, t1;

    t0 = cs_timer_time();

    switch(d->type) {
    case CS_ALL_TO_ALL_MPI_DEFAULT:
      {
        _alltoall_caller_exchange_meta(d->dc,
                                       d->n_elts_src,
                                       d->dest_rank);
        d->n_elts_dest = d->dc->recv_size;
      }
      break;

    case CS_ALL_TO_ALL_CRYSTAL_ROUTER:
      {
        cs_crystal_router_t *cr
          = cs_crystal_router_create_s(d->n_elts_src,
                                       0,
                                       CS_DATATYPE_NULL,
                                       0,
                                       NULL,
                                       NULL,
                                       d->dest_rank,
                                       d->comm);

        cs_timer_t tcr0 = cs_timer_time();

        cs_crystal_router_exchange(cr);

        cs_timer_t tcr1 = cs_timer_time();
        cs_timer_counter_add_diff
          (_all_to_all_timers + CS_ALL_TO_ALL_TIME_METADATA, &tcr0, &tcr1);
        _all_to_all_calls[CS_ALL_TO_ALL_TIME_METADATA] += 1;

        d->n_elts_dest = cs_crystal_router_n_elts(cr);
        cs_crystal_router_destroy(&cr);
      }
      break;

    }

    t1 = cs_timer_time();
    cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                              &t0, &t1);
    _all_to_all_calls[CS_ALL_TO_ALL_TIME_TOTAL] += 1;

  }

  return d->n_elts_dest;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Communicate array data using all-to-all distributor.
 *
 * If a destination buffer is provided, it should be of sufficient size for
 * the number of elements returned by \ref cs_all_to_all_n_elts_dest
 * (multiplied by stride and datatype size).
 *
 * If no buffer is provided, one is allocated automatically, and transferred
 * to the caller (who is responsible for freeing it when no longer needed).
 *
 * If used in reverse mode, data is still communicated from src_data
 * to dest_buffer or an internal buffer, but communication direction
 * (i.e. source and destination ranks) are reversed.
 *
 * This is obviously a collective operation, and all ranks must provide
 * the same datatype, stride, and reverse values.
 *
 * \param[in, out]  d          pointer to associated all-to-all distributor
 * \param[in]       datatype   type of data considered
 * \param[in]       stride     number of values per entity (interlaced),
 * \param[in]       reverse    if true, communicate in reverse direction
 * \param[in]       src_data   source data
 * \param[out]      dest_data  pointer to destination data, or NULL
 *
 * \return pointer to destination data (dest_buffer if non-NULL)
 */
/*----------------------------------------------------------------------------*/

void *
cs_all_to_all_copy_array(cs_all_to_all_t   *d,
                         cs_datatype_t      datatype,
                         int                stride,
                         bool               reverse,
                         const void        *src_data,
                         void              *dest_data)
{
  cs_assert(d != NULL);

  void  *_dest_data = NULL;

  cs_timer_t t0, t1;

  t0 = cs_timer_time();

  /* Reverse can only be called after direct exchange in most cases
     (this case should be rare, and requires additional echanges,
     but let's play it safe and make sure it works if needed) */

  if (d->n_elts_dest < 0 && reverse)
    cs_all_to_all_copy_array(d,
                             CS_DATATYPE_NULL,
                             0,
                             false,
                             NULL,
                             NULL);

  /* Now do regular exchange */

  switch(d->type) {

  case CS_ALL_TO_ALL_MPI_DEFAULT:
    {
      if (d->n_elts_dest < 0) { /* Exchange metadata if not done yet */
        _alltoall_caller_exchange_meta(d->dc,
                                       d->n_elts_src,
                                       d->dest_rank);
        d->n_elts_dest = d->dc->recv_size;
      }
      size_t n_elts = (reverse) ? d->n_elts_dest : d->n_elts_src;
      if (reverse)
        _alltoall_caller_swap_src_dest(d->dc);
      _alltoall_caller_update_meta_s(d->dc,
                                     d->dc->datatype,
                                     stride);
      _alltoall_caller_prepare_s(d->dc,
                                 n_elts,
                                 stride,
                                 datatype,
                                 reverse,
                                 src_data,
                                 d->dest_id,
                                 d->recv_id,
                                 d->dest_rank);
      _dest_data = _alltoall_caller_exchange_s(d, d->dc, dest_data);
      if (reverse) {
        _alltoall_caller_swap_src_dest(d->dc);
        if (d->dc->send_buffer == src_data)
          d->dc->send_buffer = NULL;
      }
    }
    break;

  case CS_ALL_TO_ALL_CRYSTAL_ROUTER:
    {
      int cr_flags = 0;
      _dest_data = dest_data;
      cs_timer_t tcr0, tcr1;
      cs_crystal_router_t *cr;
      if (!reverse) {
        /* Additional flags for metadata (some on first exchange only) */
        if (d->n_elts_dest < 0) {
          if (d->flags & CS_ALL_TO_ALL_ORDER_BY_DEST_ID)
            cr_flags = cr_flags | CS_CRYSTAL_ROUTER_USE_DEST_ID;
          if (! (d->flags & CS_ALL_TO_ALL_NO_REVERSE)) {
            cr_flags = cr_flags | CS_CRYSTAL_ROUTER_ADD_SRC_ID;
            cr_flags = cr_flags | CS_CRYSTAL_ROUTER_ADD_SRC_RANK;
          }
          if (d->flags & CS_ALL_TO_ALL_USE_SRC_RANK)
            cr_flags = cr_flags | CS_CRYSTAL_ROUTER_ADD_SRC_RANK;
        }
        if (d->flags & CS_ALL_TO_ALL_ORDER_BY_SRC_RANK)
          cr_flags = cr_flags | CS_CRYSTAL_ROUTER_ADD_SRC_RANK;
        cr = cs_crystal_router_create_s(d->n_elts_src,
                                        stride,
                                        datatype,
                                        cr_flags,
                                        src_data,
                                        d->dest_id,
                                        d->dest_rank,
                                        d->comm);
        tcr0 = cs_timer_time();
        cs_crystal_router_exchange(cr);
        tcr1 = cs_timer_time();
        if (d->n_elts_dest < 0)
          d->n_elts_dest = cs_crystal_router_n_elts(cr);
        int **p_src_rank = (d->src_rank == NULL) ? &(d->src_rank) : NULL;
        cs_crystal_router_get_data(cr,
                                   p_src_rank,
                                   &(d->recv_id),
                                   &(d->src_id),
                                   NULL, /* dest_index */
                                   &_dest_data);
      }
      else {
        cr_flags = cr_flags | CS_CRYSTAL_ROUTER_USE_DEST_ID;
        cr = cs_crystal_router_create_s(d->n_elts_dest,
                                        stride,
                                        datatype,
                                        cr_flags,
                                        src_data,
                                        d->src_id,
                                        d->src_rank,
                                        d->comm);
        tcr0 = cs_timer_time();
        cs_crystal_router_exchange(cr);
        tcr1 = cs_timer_time();
        cs_crystal_router_get_data(cr,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL, /* dest_index */
                                   &_dest_data);
      }
      cs_crystal_router_destroy(&cr);
      cs_timer_counter_add_diff
        (_all_to_all_timers + CS_ALL_TO_ALL_TIME_EXCHANGE, &tcr0, &tcr1);
      _all_to_all_calls[CS_ALL_TO_ALL_TIME_EXCHANGE] += 1;
    }
    break;

  }

  t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                            &t0, &t1);
  _all_to_all_calls[CS_ALL_TO_ALL_TIME_TOTAL] += 1;

  return _dest_data;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Communicate local index using all-to-all distributor.
 *
 * If a destination buffer is provided, it should be of sufficient size for
 * the number of elements returned by \ref cs_all_to_all_n_elts_dest.
 *
 * If no buffer is provided, one is allocated automatically, and transferred
 * to the caller (who is responsible for freeing it when no longer needed).
 *
 * If used in reverse mode, data is still communicated from src_index
 * to dest_index or an internal buffer, but communication direction
 * (i.e. source and destination ranks) are reversed.
 *
 * This is obviously a collective operation, and all ranks must provide
 * the same value for the reverse parameter.
 *
 * \param[in, out]  d           pointer to associated all-to-all distributor
 * \param[in]       reverse     if true, communicate in reverse direction
 * \param[in]       src_index   source index
 * \param[out]      dest_index  pointer to destination index, or NULL
 *
 * \return pointer to destination data (dest_buffer if non-NULL)
 */
/*----------------------------------------------------------------------------*/

cs_lnum_t *
cs_all_to_all_copy_index(cs_all_to_all_t  *d,
                         bool              reverse,
                         cs_lnum_t        *src_index,
                         cs_lnum_t        *dest_index)
{
  cs_timer_t t0, t1;

  cs_assert(d != NULL);

  cs_lnum_t *src_count = NULL;
  cs_lnum_t *_dest_index = dest_index;

  cs_lnum_t n_dest = cs_all_to_all_n_elts_dest(d);

  t0 = cs_timer_time();

  if (dest_index == NULL)
    BFT_MALLOC(_dest_index, n_dest + 1, cs_lnum_t);

  /* Convert send index to count, then exchange */

  BFT_MALLOC(src_count, d->n_elts_src, cs_lnum_t);

  for (cs_lnum_t i = 0; i < d->n_elts_src; i++)
    src_count[i] = src_index[i+1] - src_index[i];

  t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                            &t0, &t1);

  cs_all_to_all_copy_array(d,
                           CS_LNUM_TYPE,
                           1,
                           reverse,
                           src_count,
                           _dest_index + 1);

  t0 = cs_timer_time();

  BFT_FREE(src_count);

  _dest_index[0] = 0;

  for (cs_lnum_t i = 0; i < n_dest; i++)
    _dest_index[i+1] += _dest_index[i];

  t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                            &t0, &t1);

  return _dest_index;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Communicate local index using all-to-all distributor.
 *
 * If a destination buffer is provided, it should be of sufficient size for
 * the number of elements indicated by
 * dest_index[\ref cs_all_to_all_n_elts_dest "cs_all_to_all_n_elts_dest(d)"];
 *
 * If no buffer is provided, one is allocated automatically, and transferred
 * to the caller (who is responsible for freeing it when no longer needed).
 *
 * If used in reverse mode, data is still communicated from src_index
 * to dest_index or an internal buffer, but communication direction
 * (i.e. source and destination ranks) are reversed.
 *
 * This is obviously a collective operation, and all ranks must provide
 * the same value for the reverse parameter.
 *
 * \param[in, out]  d           pointer to associated all-to-all distributor
 * \param[in]       datatype    type of data considered
 * \param[in]       reverse     if true, communicate in reverse direction
 * \param[in]       src_index   source index
 * \param[in]       src_data    source data
 * \param[in]       dest_index  destination index
 * \param[out]      dest_data   pointer to destination data, or NULL
 *
 * \return pointer to destination data (dest_buffer if non-NULL)
 */
/*----------------------------------------------------------------------------*/

void *
cs_all_to_all_copy_indexed(cs_all_to_all_t  *d,
                           cs_datatype_t     datatype,
                           bool              reverse,
                           const cs_lnum_t  *src_index,
                           const void       *src_data,
                           const cs_lnum_t  *dest_index,
                           void             *dest_data)
{
  cs_assert(d != NULL);

  cs_assert(false);

  return NULL;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Get array of source element ranks associated with an
 *        all-to-all distributor.
 *
 * This function should be called only after \ref cs_all_to_all_copy_array,
 * and allocates and returns an array of source element ranks matching the
 * exchanged data elements.
 *
 * It should also be called only if the distributor creation flags match
 * CS_ALL_TO_ALL_USE_SRC_RANK or CS_ALL_TO_ALL_ORDER_BY_SRC_RANK.
 *
 * The returned data is owned by the caller, who is responsible for freeing
 * it when no longer needed.
 *
 * If source ranks are not available (depending on the distributor
 * creation function and options), the matching pointer will
 * be set to NULL.
 *
 * \param[in]  d   pointer to associated all-to-all distributor
 *
 * \return  array of source ranks (or NULL)
 */
/*----------------------------------------------------------------------------*/

int *
cs_all_to_all_get_src_rank(cs_all_to_all_t  *d)
{
  cs_timer_t t0 = cs_timer_time();

  int *src_rank = NULL;

  cs_assert(d != NULL);

  if (! (   d->flags & CS_ALL_TO_ALL_USE_SRC_RANK
         || d->flags & CS_ALL_TO_ALL_ORDER_BY_SRC_RANK))
    bft_error(__FILE__, __LINE__, 0,
              "%s: is called for a distributor with flags %d, which does not\n"
              "match masks CS_ALL_TO_ALL_USE_SRC_RANK (%d) or "
              "CS_ALL_TO_ALL_ORDER_BY_SRC_RANK (%d).",
              __func__, d->flags,
              CS_ALL_TO_ALL_USE_SRC_RANK,
              CS_ALL_TO_ALL_ORDER_BY_SRC_RANK);

  BFT_MALLOC(src_rank, d->n_elts_dest, int);

  switch(d->type) {

  case CS_ALL_TO_ALL_MPI_DEFAULT:
    {
      int i;
      cs_lnum_t j;
      _mpi_all_to_all_caller_t *dc = d->dc;

      for (i = 0; i < dc->n_ranks; i++) {
        for (j = dc->recv_displ[i]; j < dc->recv_displ[i+1]; j++)
          src_rank[j] = i;
      }
    }
    break;

  case CS_ALL_TO_ALL_CRYSTAL_ROUTER:
    {
      if (d->src_rank != NULL)
        memcpy(src_rank, d->src_rank, d->n_elts_dest*sizeof(int));
    }

  }

  cs_timer_t t1 = cs_timer_time();
  cs_timer_counter_add_diff(_all_to_all_timers + CS_ALL_TO_ALL_TIME_TOTAL,
                            &t0, &t1);

  return src_rank;
}

#endif /* defined(HAVE_MPI) */

/*----------------------------------------------------------------------------*/
/*!
 * \brief Get current type of all-to-all distributor algorithm choice.
 *
 * \return  current type of all-to-all distributor algorithm choice
 */
/*----------------------------------------------------------------------------*/

cs_all_to_all_type_t
cs_all_to_all_get_type(void)
{
  return _all_to_all_type;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Set current type of all-to-all distributor algorithm choice.
 *
 * \param  t  type of all-to-all distributor algorithm choice to select
 */
/*----------------------------------------------------------------------------*/

void
cs_all_to_all_set_type(cs_all_to_all_type_t  t)
{
  _all_to_all_type = t;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Log performance information relative to instrumented all-to-all
 * distribution.
 */
/*----------------------------------------------------------------------------*/

void
cs_all_to_all_log_finalize(void)
{
  cs_crystal_router_log_finalize();

#if defined(HAVE_MPI)

  int i;
  size_t name_width = 0;

  const char *method_name[] = {N_("MPI_Alltoall and MPI_Alltoallv"),
                               N_("Crystal Router algorithm")};
  const char *timer_name[] = {N_("Total:"),
                              N_("Metadata exchange:"),
                              N_("Data exchange:")};

  if (_all_to_all_calls[0] <= 0)
    return;

  cs_log_printf(CS_LOG_PERFORMANCE,
                _("\nInstrumented all-to-all operations (using %s):\n\n"),
                _(method_name[_all_to_all_type]));

  /* Determine width */

  for (i = 0; i < 3; i++) {
    if (_all_to_all_calls[i] > 0) {
      size_t l = cs_log_strlen(_(timer_name[i]));
      name_width = CS_MAX(name_width, l);
    }
  }
  name_width = CS_MIN(name_width, 63);

  /* Print times */

  for (i = 0; i < 3; i++) {

    if (_all_to_all_calls[i] > 0) {

      char tmp_s[64];
      double wtimes = (_all_to_all_timers[i]).wall_nsec*1e-9;

      cs_log_strpad(tmp_s, _(timer_name[i]), name_width, 64);

      cs_log_printf(CS_LOG_PERFORMANCE,
                    _("  %s %12.5f s, %lu calls\n"),
                    tmp_s, wtimes, (unsigned long)(_all_to_all_calls[i]));

    }

  }

  cs_log_printf(CS_LOG_PERFORMANCE, "\n");
  cs_log_separator(CS_LOG_PERFORMANCE);

#endif /* defined(HAVE_MPI) */
}


/*----------------------------------------------------------------------------*/

END_C_DECLS
