! 
! Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
!
! Licensed under the Apache License, Version 2.0 (the "License");
! you may not use this file except in compliance with the License.
! You may obtain a copy of the License at
!
!     http://www.apache.org/licenses/LICENSE-2.0
!
! Unless required by applicable law or agreed to in writing, software
! distributed under the License is distributed on an "AS IS" BASIS,
! WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
! See the License for the specific language governing permissions and
! limitations under the License.
! 


#include "mmul_dir.h"


subroutine ftn_gather_real8( a, lda, alpha, buffer, bufrows, bufcols )
  implicit none
#ifdef TARGET_X8664
  integer*8 lda
#else
  integer lda
#endif
  real*8 :: a( lda,* ), alpha
  integer :: bufrows, bufcols
  integer i, j, ndx, ndxsave
  real*8 :: buffer(bufrows * bufcols)

  !
  ! This routine gathers the matrix into l1 chunks. The purpose is much as it
  ! is for the transpose case, and works much like transpose_real8()
  !
  !   What do the parameters mean?
  !   buffer: buffer array
  !   a: matrix to be gathered
  !   bufcols: number of rows in matrix a to gather
  !   bufrows: number of cols in matrix a to gather
  !   lda: number of rows in matrix a
  !   Note that we don't care what the dimensions of a are. We assume that the
  !   calling function has done this correctly
  !
  
  ndx = 0
  do j = 1, bufcols
     do i = 1, bufrows
        buffer( ndx + i ) = alpha * a( i, j )
     enddo
     ndx = ndx + bufrows
  enddo
  !      write( *, * ) ( a(1, j ), j = 1, bufcols )
  !      write( *, * )( buffer( i ), i = 1, bufrows * bufcols )
  return
end subroutine ftn_gather_real8
