/****************************************************************************

  (c) SYSTEC electronic GmbH, D-07973 Greiz, August-Bebel-Str. 29
      www.systec-electronic.com

  Project:      openPOWERLINK

  Description:  include file for EPL Kernel-Timermodule

  License:

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

    3. Neither the name of SYSTEC electronic GmbH nor the names of its
       contributors may be used to endorse or promote products derived
       from this software without prior written permission. For written
       permission, please contact info@systec-electronic.com.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.

    Severability Clause:

        If a provision of this License is or becomes illegal, invalid or
        unenforceable in any jurisdiction, that shall not affect:
        1. the validity or enforceability in that jurisdiction of any other
           provision of this License; or
        2. the validity or enforceability in other jurisdictions of that or
           any other provision of this License.

  -------------------------------------------------------------------------

                $RCSfile: EplTimerk.h,v $

                $Author: wtchen $

                $Revision: 1.2 $  $Date: 2014/11/19 09:13:32 $

                $State: Exp $

                Build Environment:
                    GCC V3.4

  -------------------------------------------------------------------------

  Revision History:

  2006/07/06 k.t.:   start of the implementation

****************************************************************************/

#ifndef _EPLTIMERK_H_
#define _EPLTIMERK_H_

#include "../EplTimer.h"
#include "../user/EplEventu.h"

#if EPL_TIMER_USE_USER != FALSE
#include "../user/EplTimeru.h"
#endif


#if EPL_TIMER_USE_USER != FALSE
#define EplTimerkInit           EplTimeruInit
#define EplTimerkAddInstance    EplTimeruAddInstance
#define EplTimerkDelInstance    EplTimeruDelInstance
#define EplTimerkSetTimerMs     EplTimeruSetTimerMs
#define EplTimerkModifyTimerMs  EplTimeruModifyTimerMs
#define EplTimerkDeleteTimer    EplTimeruDeleteTimer
#endif

#if EPL_TIMER_USE_USER == FALSE
tEplKernel EplTimerkInit(void);

tEplKernel EplTimerkAddInstance(void);

tEplKernel EplTimerkDelInstance(void);

tEplKernel EplTimerkSetTimerMs(tEplTimerHdl *pTimerHdl_p,
			       unsigned long ulTime_p,
			       tEplTimerArg Argument_p);

tEplKernel EplTimerkModifyTimerMs(tEplTimerHdl *pTimerHdl_p,
				  unsigned long ulTime_p,
				  tEplTimerArg Argument_p);

tEplKernel EplTimerkDeleteTimer(tEplTimerHdl *pTimerHdl_p);
#endif
#endif // #ifndef _EPLTIMERK_H_
