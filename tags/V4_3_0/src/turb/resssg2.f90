!-------------------------------------------------------------------------------

! This file is part of Code_Saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2016 EDF S.A.
!
! This program is free software; you can redistribute it and/or modify it under
! the terms of the GNU General Public License as published by the Free Software
! Foundation; either version 2 of the License, or (at your option) any later
! version.
!
! This program is distributed in the hope that it will be useful, but WITHOUT
! ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
! FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
! details.
!
! You should have received a copy of the GNU General Public License along with
! this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
! Street, Fifth Floor, Boston, MA 02110-1301, USA.

!-------------------------------------------------------------------------------

!===============================================================================
! Function:
! ---------

!> \file resssg2.f90
!>
!> \brief This subroutine performs the solving of the Reynolds stress components
!> in \f$ R_{ij} - \varepsilon \f$ RANS (SSG) turbulence model.
!>
!> Remark:
!> - isou=1 for \f$ R_{11} \f$
!> - isou=2 for \f$ R_{22} \f$
!> - isou=3 for \f$ R_{33} \f$
!> - isou=4 for \f$ R_{12} \f$
!> - isou=5 for \f$ R_{23} \f$
!> - isou=6 for \f$ R_{13} \f$
!-------------------------------------------------------------------------------

!-------------------------------------------------------------------------------
! Arguments
!______________________________________________________________________________.
!  mode           name          role
!______________________________________________________________________________!
!> \param[in]     nvar          total number of variables
!> \param[in]     nscal         total number of scalars
!> \param[in]     ncepdp        number of cells with head loss
!> \param[in]     ncesmp        number of cells with mass source term
!> \param[in]     ivar          variable number
!> \param[in]     icepdc        index of cells with head loss
!> \param[in]     icetsm        index of cells with mass source term
!> \param[in]     itypsm        type of mass source term for each variable
!>                               (see \ref cs_user_mass_source_terms)
!> \param[in]     dt            time step (per cell)
!> \param[in]     gradv         work array for the velocity grad term
!>                                 only for iturb=31
!> \param[in]     gradro        work array for grad rom
!>                              (without rho volume) only for iturb=30
!> \param[in]     ckupdc        work array for the head loss
!> \param[in]     smacel        value associated to each variable in the mass
!>                               source terms or mass rate
!>                               (see \ref cs_user_mass_source_terms)
!> \param[in]     viscf         visc*surface/dist at internal faces
!> \param[in]     viscb         visc*surface/dist at edge faces
!> \param[in]     tslage        explicit source terms for the Lagrangian module
!> \param[in]     tslagi        implicit source terms for the Lagrangian module
!> \param[in]     smbr          working array
!> \param[in]     rovsdt        working array
!_______________________________________________________________________________

subroutine resssg2 &
 ( nvar   , nscal  , ncepdp , ncesmp ,                            &
   ivar   ,                                               &
   icepdc , icetsm , itypsm ,                                     &
   dt     ,                                                       &
   gradv  , gradro ,                                              &
   ckupdc , smacel ,                                              &
   viscf  , viscb  ,                                              &
   tslage , tslagi ,                                              &
   smbr   , rovsdt )

!===============================================================================

!===============================================================================
! Module files
!===============================================================================

use paramx
use numvar
use entsor
use optcal
use cstphy
use cstnum
use parall
use period
use lagran
use mesh
use field
use field_operator
use cs_f_interfaces
use rotation
use turbomachinery
use cs_c_bindings

!===============================================================================

implicit none

! Arguments

integer          nvar   , nscal
integer          ncepdp , ncesmp
integer          ivar

integer          icepdc(ncepdp)
integer          icetsm(ncesmp), itypsm(ncesmp,nvar)

double precision dt(ncelet)
double precision gradv(3, 3, ncelet)
double precision gradro(3,ncelet)
double precision ckupdc(ncepdp,6), smacel(ncesmp,nvar)
double precision viscf(nfac), viscb(nfabor)
double precision tslage(6,ncelet),tslagi(6,ncelet)
double precision smbr(6,ncelet)
double precision rovsdt(6,6,ncelet)

! Local variables

integer          iel, isou, jsou
integer          ii    , jj    , kk    , iiun  , iii   , jjj
integer          iflmas, iflmab
integer          nswrgp, imligp, iwarnp
integer          iconvp, idiffp, ndircp
integer          nswrsp, ircflp, ischcp, isstpp
integer          st_prv_id
integer          iprev , inc, iccocg, ll
integer          idftnp, iswdyp
integer          icvflb
integer          ivoid(1)
integer          dimrij

double precision blencp, epsilp, epsrgp, climgp, extrap, relaxp
double precision epsrsp
double precision trprod, trrij
double precision deltij(6)
double precision tuexpr, thets , thetv , thetp1
double precision aiksjk, aikrjk, aii ,aklskl, aikakj
double precision xaniso(3,3), xstrai(3,3), xrotac(3,3), xprod(3,3), matrot(3,3)
double precision xrij(3,3), xnal(3), xnoral, xnnd
double precision d1s2, d1s3, d2s3
double precision alpha3
double precision pij, phiij1, phiij2, epsij
double precision phiijw, epsijw
double precision ccorio
double precision rctse

character(len=80) :: label
double precision, allocatable, dimension(:,:) :: grad
double precision, allocatable, dimension(:) :: w1, w2
double precision, allocatable, dimension(:,:) :: w7
double precision, allocatable, dimension(:) :: dpvar
double precision, allocatable, dimension(:,:) :: viscce
double precision, allocatable, dimension(:,:) :: weighf
double precision, allocatable, dimension(:) :: weighb
double precision, dimension(:), pointer :: imasfl, bmasfl
double precision, dimension(:), pointer :: crom, cromo
double precision, dimension(:,:), pointer :: coefap, cofafp
double precision, dimension(:,:,:), pointer :: coefbp, cofbfp
double precision, dimension(:,:), pointer :: visten
double precision, dimension(:), pointer :: cvara_ep, cvar_al
double precision, dimension(:,:), pointer :: cvar_var, cvara_var
double precision, dimension(:), pointer :: viscl
double precision, dimension(:,:), pointer:: c_st_prv

double precision, allocatable, dimension(:,:) :: cvara_r

!===============================================================================

!===============================================================================
! 1. Initialization
!===============================================================================

! Allocate work arrays
allocate(w1(ncelet), w2(ncelet))
allocate(dpvar(ncelet))
allocate(viscce(6,ncelet))
allocate(weighf(2,nfac))
allocate(weighb(nfabor))

! Initialize variables to avoid compiler warnings
iii = 0
jjj = 0

if (iwarni(ivar).ge.1) then
  call field_get_label(ivarfl(ivar), label)
  write(nfecra,1000) label
endif

call field_get_val_s(icrom, crom)
call field_get_val_s(iprpfl(iviscl), viscl)

call field_get_val_prev_s(ivarfl(iep), cvara_ep)
if (iturb.ne.31) call field_get_val_s(ivarfl(ial), cvar_al)

call field_get_val_v(ivarfl(ivar), cvar_var)
call field_get_val_prev_v(ivarfl(ivar), cvara_var)
call field_get_dim(ivarfl(ivar),dimrij)! dimension of Rij
!dimrij = 6 if irijco = 1

call field_get_key_int(ivarfl(iu), kimasf, iflmas)
call field_get_key_int(ivarfl(iu), kbmasf, iflmab)
call field_get_val_s(iflmas, imasfl)
call field_get_val_s(iflmab, bmasfl)

d1s2   = 1.d0/2.d0
d1s3   = 1.d0/3.d0
d2s3   = 2.d0/3.d0

do isou = 1, 6
  deltij(isou) = 1.0d0
  if (isou.gt.3) then
    deltij(isou) = 0.0d0
  endif
enddo
!     S as Source, V as Variable
thets  = thetst
thetv  = thetav(ivar)

call field_get_key_int(ivarfl(ivar), kstprv, st_prv_id)
if (st_prv_id .ge. 0) then
  call field_get_val_v(st_prv_id, c_st_prv)
else
  c_st_prv=> null()
endif

if (st_prv_id.ge.0.and.iroext.gt.0) then
  call field_get_val_prev_s(icrom, cromo)
else
  call field_get_val_s(icrom, cromo)
endif

do isou = 1, 6
  do iel = 1, ncel
    smbr(isou,iel) = 0.d0
  enddo
enddo
do iel = 1, ncel
  do isou = 1, 6
    do jsou = 1, 6
      rovsdt(isou,jsou,iel) = 0.d0
    enddo
  enddo
enddo

if (icorio.eq.1 .or. iturbo.eq.1) then
allocate(cvara_r(3,3))

  ! Coefficient of the "Coriolis-type" term
  if (icorio.eq.1) then
    ! Relative velocity formulation
    ccorio = 2.d0
  elseif (iturbo.eq.1) then
    ! Mixed relative/absolute velocity formulation
    ccorio = 1.d0
  endif

endif

!===============================================================================
! 2. User source terms
!===============================================================================
call cs_user_turbulence_source_terms2 &
!===================================
 ( nvar   , nscal  , ncepdp , ncesmp ,                            &
   ivarfl(ivar)    ,                                              &
   icepdc , icetsm , itypsm ,                                     &
   ckupdc , smacel ,                                              &
   smbr   , rovsdt )

  !     If we extrapolate the source terms
do isou = 1, dimrij
  if (st_prv_id.ge.0) then
    do iel = 1, ncel
      !       Save for exchange
      tuexpr = c_st_prv(isou,iel)
      !       For continuation and the next time step
      c_st_prv(isou,iel) = smbr(isou,iel)
      !       Second member of the previous time step
      !       We suppose -rovsdt > 0: we implicite
      !          the user source term (the rest)
      smbr(isou,iel) = rovsdt(isou,isou,iel)*cvara_var(isou,iel)  - thets*tuexpr
      !       Diagonal
      rovsdt(isou,isou,iel) = - thetv*rovsdt(isou,isou,iel)
    enddo
  else
    do iel = 1, ncel
      smbr(isou,iel)   = rovsdt(isou,isou,iel)*cvara_var(isou,iel) + smbr(isou,iel)
      rovsdt(isou,isou,iel) = max(-rovsdt(isou,isou,iel),zero)
    enddo
  endif
enddo

!===============================================================================
! 3. Lagrangian source terms
!===============================================================================

!     2nd order is not taken into account
do isou = 1, dimrij
        if (iilagr.eq.2 .and. ltsdyn.eq.1) then
    do iel = 1,ncel
      smbr(isou, iel)   = smbr(isou, iel)   + tslage(isou,iel)
      rovsdt(isou,isou,iel) = rovsdt(isou,isou, iel) + max(-tslagi(isou, iel),zero)
    enddo
  endif
enddo

!===============================================================================
! 4. Mass source term
!===============================================================================

do isou = 1, dimrij
  if (ncesmp.gt.0) then

    !       Integer equal to 1 (for navsto: nb of sur-iter)
    iiun = 1

    ! We increment smbr with -Gamma.var_prev and rovsdr with Gamma
    call catsmt &
    !==========
   ( ncelet , ncel   , ncesmp , iiun     , isto2t ,                   &
     icetsm , itypsm(:,ivar + isou - 1)  ,                            &
     cell_f_vol , cvara_var  , smacel(:,ivar+isou-1)  , smacel(:,ipr) ,   &
     smbr   ,  rovsdt    , w1 )

    ! If we extrapolate the source terms we put Gamma Pinj in the previous st
    if (st_prv_id.ge.0) then
      do iel = 1, ncel
        c_st_prv(isou,iel) = c_st_prv(isou,iel) + w1(iel)
      enddo
    ! Otherwise we put it directly in the RHS
    else
      do iel = 1, ncel
        smbr(isou, iel) = smbr(isou, iel) + w1(iel)
      enddo
    endif

  endif
enddo

!===============================================================================
! 5. Unsteady term
!===============================================================================

! ---> Added in the matrix diagonal

do isou = 1, dimrij
  do iel=1,ncel
    rovsdt(isou,isou,iel) = rovsdt(isou, isou,iel)                                       &
              + istat(ivar+isou-1)*(crom(iel)/dt(iel))*cell_f_vol(iel)
  enddo
enddo

!===============================================================================
! 6. Production, Pressure-Strain correlation, dissipation, Coriolis
!===============================================================================

! ---> Source term
!     -rho*epsilon*( Cs1*aij + Cs2*(aikajk -1/3*aijaij*deltaij))
!     -Cr1*P*aij + Cr2*rho*k*sij - Cr3*rho*k*sij*sqrt(aijaij)
!     +Cr4*rho*k(aik*sjk+ajk*sik-2/3*akl*skl*deltaij)
!     +Cr5*rho*k*(aik*rjk + ajk*rik)
!     -2/3*epsilon*deltaij


! EBRSM
if (iturb.eq.32) then
  allocate(grad(3,ncelet))

  ! Compute the gradient of Alpha
  iprev  = 1
  inc    = 1
  iccocg = 1

  call field_gradient_scalar(ivarfl(ial), iprev, imrgra, inc,     &
                             iccocg,                              &
                             grad)

endif

do iel=1,ncel

  ! EBRSM
  if (iturb.eq.32) then
    ! Compute the magnitude of the Alpha gradient
    xnoral = ( grad(1,iel)*grad(1,iel)          &
           +   grad(2,iel)*grad(2,iel)          &
           +   grad(3,iel)*grad(3,iel) )
    xnoral = sqrt(xnoral)
   ! Compute the unitary vector of Alpha
    if (xnoral.le.epzero) then
      xnal(1) = 0.d0
      xnal(2) = 0.d0
      xnal(3) = 0.d0
    else
      xnal(1) = grad(1,iel)/xnoral
      xnal(2) = grad(2,iel)/xnoral
      xnal(3) = grad(3,iel)/xnoral
    endif
  endif

  ! Pij
  xprod(1,1) = -2.0d0*(cvara_var(1 ,iel)*gradv(1, 1, iel) +         &
                       cvara_var(4 ,iel)*gradv(2, 1, iel) +         &
                       cvara_var(6 ,iel)*gradv(3, 1, iel) )
  xprod(1,2) = -(      cvara_var(1 ,iel)*gradv(1, 2, iel) +         &
                       cvara_var(4 ,iel)*gradv(2, 2, iel) +         &
                       cvara_var(6 ,iel)*gradv(3, 2, iel) )         &
               -(      cvara_var(4 ,iel)*gradv(1, 1, iel) +         &
                       cvara_var(2 ,iel)*gradv(2, 1, iel) +         &
                       cvara_var(5 ,iel)*gradv(3, 1, iel) )
  xprod(1,3) = -(      cvara_var(1 ,iel)*gradv(1, 3, iel) +         &
                       cvara_var(4 ,iel)*gradv(2, 3, iel) +         &
                       cvara_var(6 ,iel)*gradv(3, 3, iel) )         &
               -(      cvara_var(6 ,iel)*gradv(1, 1, iel) +         &
                       cvara_var(5 ,iel)*gradv(2, 1, iel) +         &
                       cvara_var(3 ,iel)*gradv(3, 1, iel) )
  xprod(2,2) = -2.0d0*(cvara_var(4 ,iel)*gradv(1, 2, iel) +         &
                       cvara_var(2 ,iel)*gradv(2, 2, iel) +         &
                       cvara_var(5 ,iel)*gradv(3, 2, iel) )
  xprod(2,3) = -(      cvara_var(4 ,iel)*gradv(1, 3, iel) +         &
                       cvara_var(2 ,iel)*gradv(2, 3, iel) +         &
                       cvara_var(5 ,iel)*gradv(3, 3, iel) )         &
               -(      cvara_var(6 ,iel)*gradv(1, 2, iel) +         &
                       cvara_var(5 ,iel)*gradv(2, 2, iel) +         &
                       cvara_var(3 ,iel)*gradv(3, 2, iel) )
  xprod(3,3) = -2.0d0*(cvara_var(6 ,iel)*gradv(1, 3, iel) +         &
                       cvara_var(5 ,iel)*gradv(2, 3, iel) +         &
                       cvara_var(3 ,iel)*gradv(3, 3, iel) )

  ! Rotating frame of reference => "Coriolis production" term

  if (icorio.eq.1 .or. iturbo.eq.1) then

    call coriolis_t(irotce(iel), 1.d0, matrot)
    cvara_r(1,1) = cvara_var(1,iel)
    cvara_r(2,2) = cvara_var(2,iel)
    cvara_r(3,3) = cvara_var(3,iel)
    cvara_r(1,2) = cvara_var(4,iel)
    cvara_r(2,3) = cvara_var(5,iel)
    cvara_r(1,3) = cvara_var(6,iel)
    cvara_r(2,1) = cvara_var(4,iel)
    cvara_r(3,2) = cvara_var(5,iel)
    cvara_r(3,1) = cvara_var(6,iel)
    if (irotce(iel).gt.0) then
      do ii = 1, 3
        do jj = ii, 3
          do kk = 1, 3
            xprod(ii,jj) = xprod(ii,jj)                             &
                     - ccorio*( matrot(ii,kk)*cvara_r(jj,kk) &
                     + matrot(jj,kk)*cvara_r(ii,kk) )
          enddo
        enddo
      enddo
    endif
  endif

  xprod(2,1) = xprod(1,2)
  xprod(3,1) = xprod(1,3)
  xprod(3,2) = xprod(2,3)

  trprod = d1s2 * (xprod(1,1) + xprod(2,2) + xprod(3,3) )
  trrij  = d1s2 * (cvara_var(1 ,iel) + cvara_var(2 ,iel) + cvara_var(3 ,iel))
  !-----> aII = aijaij
  aii    = 0.d0
  aklskl = 0.d0
  aiksjk = 0.d0
  aikrjk = 0.d0
  aikakj = 0.d0
  ! aij
  xaniso(1,1) = cvara_var(1 ,iel)/trrij - d2s3
  xaniso(2,2) = cvara_var(2 ,iel)/trrij - d2s3
  xaniso(3,3) = cvara_var(3 ,iel)/trrij - d2s3
  xaniso(1,2) = cvara_var(4 ,iel)/trrij
  xaniso(1,3) = cvara_var(6 ,iel)/trrij
  xaniso(2,3) = cvara_var(5 ,iel)/trrij
  xaniso(2,1) = xaniso(1,2)
  xaniso(3,1) = xaniso(1,3)
  xaniso(3,2) = xaniso(2,3)
  ! Sij
  xstrai(1,1) = gradv(1, 1, iel)
  xstrai(1,2) = d1s2*(gradv(2, 1, iel)+gradv(1, 2, iel))
  xstrai(1,3) = d1s2*(gradv(3, 1, iel)+gradv(1, 3, iel))
  xstrai(2,1) = xstrai(1,2)
  xstrai(2,2) = gradv(2, 2, iel)
  xstrai(2,3) = d1s2*(gradv(3, 2, iel)+gradv(2, 3, iel))
  xstrai(3,1) = xstrai(1,3)
  xstrai(3,2) = xstrai(2,3)
  xstrai(3,3) = gradv(3, 3, iel)
  ! omegaij
  xrotac(1,1) = 0.d0
  xrotac(1,2) = d1s2*(gradv(2, 1, iel)-gradv(1, 2, iel))
  xrotac(1,3) = d1s2*(gradv(3, 1, iel)-gradv(1, 3, iel))
  xrotac(2,1) = -xrotac(1,2)
  xrotac(2,2) = 0.d0
  xrotac(2,3) = d1s2*(gradv(3, 2, iel)-gradv(2, 3, iel))
  xrotac(3,1) = -xrotac(1,3)
  xrotac(3,2) = -xrotac(2,3)
  xrotac(3,3) = 0.d0

  ! Rotating frame of reference => "absolute" vorticity
  if (icorio.eq.1) then
    do ii = 1, 3
      do jj = 1, 3
        xrotac(ii,jj) = xrotac(ii,jj) + matrot(ii,jj)
      enddo
    enddo
  endif

  do ii=1,3
    do jj = 1,3
      ! aii = aij.aij
      aii    = aii+xaniso(ii,jj)*xaniso(ii,jj)
      ! aklskl = aij.Sij
      aklskl = aklskl + xaniso(ii,jj)*xstrai(ii,jj)
    enddo
  enddo

  do isou = 1, dimrij
    if (isou.eq.1)then
      iii = 1
      jjj = 1
    elseif (isou.eq.2)then
      iii = 2
      jjj = 2
    elseif (isou.eq.3)then
      iii = 3
      jjj = 3
    elseif (isou.eq.4)then
      iii = 1
      jjj = 2
    elseif (isou.eq.5)then
      iii = 2
      jjj = 3
    elseif (isou.eq.6)then
      iii = 1
      jjj = 3
    endif
    aiksjk = 0
    aikrjk = 0
    aikakj = 0
    do kk = 1,3
      ! aiksjk = aik.Sjk+ajk.Sik
      aiksjk = aiksjk + xaniso(iii,kk)*xstrai(jjj,kk)              &
                +xaniso(jjj,kk)*xstrai(iii,kk)
      ! aikrjk = aik.Omega_jk + ajk.omega_ik
      aikrjk = aikrjk + xaniso(iii,kk)*xrotac(jjj,kk)              &
                +xaniso(jjj,kk)*xrotac(iii,kk)
      ! aikakj = aik*akj
      aikakj = aikakj + xaniso(iii,kk)*xaniso(kk,jjj)
    enddo

    !     If we extrapolate the source terms (rarely), we put all in the previous ST..
    !     We do not implicit the term with Cs1*aij neither the term with Cr1*P*aij.
    !     Otherwise, we put all in smbr and we can implicit Cs1*aij
    !     and Cr1*P*aij. Here we store the second member and the implicit term
    !     in W1 and W2, to avoid the test(ST_PRV_ID.GE.0)
    !     in the ncel loop
    !     In the term with W1, which is dedicated to be extrapolated, we use
    !     cromo.
    !     The implicitation of the two terms can also be done in the case of
    !     extrapolation, by isolating those two terms and by putting it in
    !     the RHS but not in the prev. ST and by using ipcrom .... to be modified if needed

    if (iturb.eq.31) then

      pij = xprod(iii,jjj)
      phiij1 = -cvara_ep(iel)* &
         (cssgs1*xaniso(iii,jjj)+cssgs2*(aikakj-d1s3*deltij(isou)*aii))
      phiij2 = - cssgr1*trprod*xaniso(iii,jjj)                             &
             +   trrij*xstrai(iii,jjj)*(cssgr2-cssgr3*sqrt(aii))           &
             +   cssgr4*trrij*(aiksjk-d2s3*deltij(isou)*aklskl)                  &
             +   cssgr5*trrij* aikrjk
      epsij = -d2s3*cvara_ep(iel)*deltij(isou)

      w1(iel) = cromo(iel)*cell_f_vol(iel)*(pij+phiij1+phiij2+epsij)

      w2(iel) = cell_f_vol(iel)/trrij*crom(iel)*(                              &
             cssgs1*cvara_ep(iel) + cssgr1*max(trprod,0.d0) )

    ! EBRSM
    else

      xrij(1,1) = cvara_var(1,iel)
      xrij(2,2) = cvara_var(2,iel)
      xrij(3,3) = cvara_var(3,iel)
      xrij(1,2) = cvara_var(4,iel)
      xrij(2,3) = cvara_var(5,iel)
      xrij(1,3) = cvara_var(6,iel)
      xrij(2,1) = xrij(1,2)
      xrij(3,1) = xrij(1,3)
      xrij(3,2) = xrij(2,3)

      ! Compute the explicit term

      ! Calculation of the terms near the walls and et almost homogeneous
      ! of phi and epsilon

      ! Calculation of the term near the wall \f$ \Phi_{ij}^w \f$ --> W3
      phiijw = 0.d0
      xnnd = d1s2*( xnal(iii)*xnal(jjj) + deltij(isou) )
      do kk = 1, 3
        phiijw = phiijw + xrij(iii,kk)*xnal(jjj)*xnal(kk)
        phiijw = phiijw + xrij(jjj,kk)*xnal(iii)*xnal(kk)
        do ll = 1, 3
          phiijw = phiijw - xrij(kk,ll)*xnal(kk)*xnal(ll)*xnnd
        enddo
      enddo
      phiijw = -5.d0*cvara_ep(iel)/trrij * phiijw

      ! Calculation of the almost homogeneous term \f$ \phi_{ij}^h \f$ --> W4
      phiij1 = -cvara_ep(iel)*cebms1*xaniso(iii,jjj)
      phiij2 = -cebmr1*trprod*xaniso(iii,jjj)                       &
                 +trrij*xstrai(iii,jjj)*(cebmr2-cebmr3*sqrt(aii))   &
                 +cebmr4*trrij   *(aiksjk-d2s3*deltij(isou)*aklskl)       &
                 +cebmr5*trrij   * aikrjk

      ! Calculation of \f $\e_{ij}^w \f$ --> W5 (Rotta model)
      ! Rij/k*epsilon
      epsijw =  xrij(iii,jjj)/trrij   *cvara_ep(iel)

      ! Calcul de \e_{ij}^h --> W6
      epsij =  d2s3*cvara_ep(iel)*deltij(isou)

      ! Calcul du terme source explicite de l'equation des Rij
      !  \f[ P_{ij} + (1-\alpha^3)\Phi_{ij}^w + \alpha^3\Phi_{ij}^h
      !            - (1-\alpha^3)\e_{ij}^w   - \alpha^3\e_{ij}^h  ]\f$ --> W1
      alpha3 = cvar_al(iel)**3

      w1(iel) = cell_f_vol(iel)*crom(iel)*(                             &
                 xprod(iii,jjj)                                     &
              + (1.d0-alpha3)*phiijw + alpha3*(phiij1+phiij2)       &
              - (1.d0-alpha3)*epsijw - alpha3*epsij)

      !  Implicite term

      ! The term below corresponds to the implicit part of SSG
      ! in the context of elliptical weighting, it is multiplied by
      ! \f$ \alpha^3 \f$
      w2(iel) = cell_f_vol(iel)*crom(iel)*(                             &
                cebms1*cvara_ep(iel)/trrij*alpha3                       &
               +cebmr1*max(trprod/trrij,0.d0)*alpha3                &
      ! Implicitation of epsijw
      ! (the factor 5 appears when we calculate \f$ Phi_{ij}^w - epsijw\f$)
              + 5.d0 * (1.d0-alpha3)*cvara_ep(iel)/trrij                &
              +        (1.d0-alpha3)*cvara_ep(iel)/trrij)
    endif
    if (st_prv_id.ge.0) then
      c_st_prv(isou,iel) = c_st_prv(isou,iel) + w1(iel)
    else
      smbr(isou,iel) = smbr(isou,iel) + w1(iel)
      rovsdt(isou,isou,iel) = rovsdt(isou,isou,iel) + w2(iel)
    endif
  enddo
enddo

if (icorio.eq.1 .or. iturbo.eq.1) then
  deallocate(cvara_r)
endif

if (iturb.eq.32) then
  deallocate(grad)
endif


!===============================================================================
! 7. Buoyancy source term
!===============================================================================

if (igrari.eq.1) then

  ! Allocate a work array
  allocate(w7(dimrij,ncelet))

  do iel = 1, ncel
    do isou = 1, dimrij
      w7(isou,iel) = 0.d0
    enddo
  enddo
  call rijthe2(nscal, gradro, w7)

  do isou = 1, dimrij
    ! If we extrapolate the source terms: previous ST
    if (st_prv_id.ge.0) then
      do iel = 1, ncel
        c_st_prv(isou,iel) = c_st_prv(isou,iel) + w7(isou,iel)
      enddo
    ! Otherwise smbr
    else
      do iel = 1, ncel
        smbr(isou,iel) = smbr(isou,iel) + w7(isou,iel)
      enddo
    endif
  enddo

  ! Free memory
  deallocate(w7)

endif

!===============================================================================
! 8. Diffusion term (Daly Harlow: generalized gradient hypothesis method)
!===============================================================================

! Symmetric tensor diffusivity (GGDH)
if (idften(ivar).eq.6) then

  call field_get_val_v(ivsten, visten)

  do iel = 1, ncel
    viscce(1,iel) = visten(1,iel) + viscl(iel)
    viscce(2,iel) = visten(2,iel) + viscl(iel)
    viscce(3,iel) = visten(3,iel) + viscl(iel)
    viscce(4,iel) = visten(4,iel)
    viscce(5,iel) = visten(5,iel)
    viscce(6,iel) = visten(6,iel)
  enddo

  iwarnp = iwarni(ivar)

  call vitens &
  !==========
 ( viscce , iwarnp ,             &
   weighf , weighb ,             &
   viscf  , viscb  )

! Scalar diffusivity
else

  do iel = 1, ncel
    trrij = 0.5d0 * (cvara_var(1,iel) + cvara_var(2,iel) + cvara_var(3,iel))
    rctse = crom(iel) * csrij * trrij**2 / cvara_ep(iel)
    w1(iel) = viscl(iel) + idifft(ivar)*rctse
  enddo

  call viscfa                    &
  !==========
  ( imvisf ,                      &
   w1     ,                      &
   viscf  , viscb  )

endif

!===============================================================================
! 9. Solving
!===============================================================================

if (st_prv_id.ge.0) then
  thetp1 = 1.d0 + thets
  do iel = 1, ncel
    do isou = 1, dimrij
      smbr(isou,iel) = smbr(isou,iel) + thetp1*c_st_prv(isou,iel)
    enddo
  enddo
endif

iconvp = iconv (ivar)
idiffp = idiff (ivar)
ndircp = ndircl(ivar)
nswrsp = nswrsm(ivar)
nswrgp = nswrgr(ivar)
imligp = imligr(ivar)
ircflp = ircflu(ivar)
ischcp = ischcv(ivar)
isstpp = isstpc(ivar)
idftnp = idften(ivar)
iswdyp = iswdyn(ivar)
iwarnp = iwarni(ivar)
blencp = blencv(ivar)
epsilp = epsilo(ivar)
epsrsp = epsrsm(ivar)
epsrgp = epsrgr(ivar)
climgp = climgr(ivar)
extrap = extrag(ivar)
relaxp = relaxv(ivar)
! all boundary convective flux with upwind
icvflb = 0

call field_get_coefa_v(ivarfl(ivar), coefap)
call field_get_coefb_v(ivarfl(ivar), coefbp)
call field_get_coefaf_v(ivarfl(ivar), cofafp)
call field_get_coefbf_v(ivarfl(ivar), cofbfp)

call coditts &
!==========
 ( idtvar , ivar   , iconvp , idiffp , ndircp ,                   &
   imrgra , nswrsp , nswrgp , imligp , ircflp ,                   &
   ischcp , isstpp , idftnp , iswdyp ,                            &
   iwarnp ,                                                       &
   blencp , epsilp , epsrsp , epsrgp , climgp ,                   &
   relaxp , thetv  ,                                              &
   cvara_var       , cvara_var       ,                            &
   coefap , coefbp , cofafp , cofbfp ,                            &
   imasfl , bmasfl ,                                              &
   viscf  , viscb  , viscf  , viscb  ,  viscce ,                  &
   weighf , weighb ,                                              &
   icvflb , ivoid  ,                                              &
   rovsdt , smbr   , cvar_var        )

! Free memory
deallocate(w1, w2)
deallocate(dpvar)
deallocate(viscce)
deallocate(weighf, weighb)

!--------
! Formats
!--------

#if defined(_CS_LANG_FR)

 1000 format(/,'           Resolution pour la variable ',A8,/)

#else

 1000 format(/,'           Solving variable ',A8           ,/)

#endif

!----
! End
!----

return

end subroutine
