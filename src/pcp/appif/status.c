/**
********************************************************************************
\file   status.c

\brief  Status module for synchronization information forwarding

This module configures the synchronous interrupt and forwards the time information
to the status triple buffer. It also updates the asynchronous channel status
information.

\ingroup module_status
*******************************************************************************/

/*------------------------------------------------------------------------------
* License Agreement
*
* Copyright 2013 BERNECKER + RAINER, AUSTRIA, 5142 EGGELSBERG, B&R STRASSE 1
* All rights reserved.
*
* Redistribution and use in source and binary forms,
* with or without modification,
* are permitted provided that the following conditions are met:
*
*   * Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer
*     in the documentation and/or other materials provided with the
*     distribution.
*   * Neither the name of the B&R nor the names of its contributors
*     may be used to endorse or promote products derived from this software
*     without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
* THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
* A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
* ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
* THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
------------------------------------------------------------------------------*/

//------------------------------------------------------------------------------
// includes
//------------------------------------------------------------------------------

#include <appif/status.h>

#include <appif/tbuf.h>

#include <kernel/EplTimerSynck.h>

//============================================================================//
//            G L O B A L   D E F I N I T I O N S                             //
//============================================================================//

//------------------------------------------------------------------------------
// const defines
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// module global vars
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
// global function prototypes
//------------------------------------------------------------------------------


//============================================================================//
//            P R I V A T E   D E F I N I T I O N S                           //
//============================================================================//

//------------------------------------------------------------------------------
// const defines
//------------------------------------------------------------------------------

#define SYNC_INT_CYCLE_NUM          1      ///< execute the sync interrupt in every cycle
#define SYNC_INT_PULSE_WIDTH_NS     2000   ///< Width of the synchronous interrupt pulse [ns]

//------------------------------------------------------------------------------
// local types
//------------------------------------------------------------------------------

typedef enum
{
    kStatusRelTimeStateInvalid = 0x00,               ///< invalid state
    kStatusRelTimeStateWaitFirstValidTime = 0x01,  ///< no valid RelativeTime received yet
    kStatusRelTimeStateActiv = 0x02,                 ///< RelativeTime is running
} tRelativeTimeState;

/**
\brief status user instance type

The status instance holds the status information of this module
*/
typedef struct
{
    tTbufInstance   pTbufOutInstance_m;    ///< Instance pointer to the outgoing triple buffer
    tTbufInstance   pTbufInInstance_m;     ///< Instance pointer to the incoming triple buffer

    tRelativeTimeState relTimeState_m;     ///< state variable of the RelativeTime state machine
    UINT32 relTimeLow_m;                   ///< local relative status counter low uint32
    UINT32 relTimeHigh_m;                  ///< local relative status counter high uint32
    UINT32 cycleTime_m;                    ///< local copy of the cycle status

    UINT8  iccStatus_m;                    ///< Icc status register
    UINT16 asyncConsStatus_m;              ///< Async consumer buffer status register

    UINT16 asyncProdStatus_m;              ///< Async producer buffer status register
} tStatusInstance;

//------------------------------------------------------------------------------
// local vars
//------------------------------------------------------------------------------

static tStatusInstance          statusInstance_l;

//------------------------------------------------------------------------------
// local function prototypes
//------------------------------------------------------------------------------
static tAppIfStatus status_processOut(tTimeInfo* pTime_p);
static tAppIfStatus status_processIn(void);
static void status_incRelTime(UINT32* prelTimeLow_p, UINT32* prelTimeHigh_p,
        UINT32* pCycleTime_p);
static tAppIfStatus status_setRelTime(UINT32 relTimeLow_p, UINT32 relTimeHigh_p);
static tAppIfStatus status_calcRelTime(tTimeInfo* pTime_p);
//============================================================================//
//            P U B L I C   F U N C T I O N S                                 //
//============================================================================//

//------------------------------------------------------------------------------
/**
\brief    Initialize the status module

\param[in] pInitParam_p     Initialization structure of the status module

\return  tAppIfStatus
\retval  kAppIfSuccessful            On success
\retval  kAppIfstatusInitError       Unable to init status module

\ingroup module_status
*/
//------------------------------------------------------------------------------
tAppIfStatus status_init(tStatusInitStruct* pInitParam_p)
{
    tAppIfStatus ret = kAppIfSuccessful;
    tTbufInitStruct  tbufInitParam;

    APPIF_MEMSET(&statusInstance_l, 0 , sizeof(tStatusInstance));

#if _DEBUG
    if(pInitParam_p->outTbufSize_m != sizeof(tTbufStatusOutStructure))
    {
        ret = kAppIfStatusBufferSizeMismatch;
        goto Exit;
    }

    if(pInitParam_p->inTbufSize_m != sizeof(tTbufStatusInStructure))
    {
        ret = kAppIfStatusBufferSizeMismatch;
        goto Exit;
    }
#endif

    // set initial relative status state
    statusInstance_l.relTimeState_m = kStatusRelTimeStateWaitFirstValidTime;

    // init the outgoing triple buffer module
    tbufInitParam.id_m = pInitParam_p->outId_m;
    tbufInitParam.pAckBase_m = pInitParam_p->pProdAckBase_m;
    tbufInitParam.pBase_m = (UINT8 *)pInitParam_p->pOutTbufBase_m;
    tbufInitParam.size_m = pInitParam_p->outTbufSize_m;

    statusInstance_l.pTbufOutInstance_m = tbuf_create(&tbufInitParam);
    if(statusInstance_l.pTbufOutInstance_m == NULL)
    {
        ret = kAppIfStatusInitError;
        goto Exit;
    }

    // init the incoming triple buffer module
    tbufInitParam.id_m = pInitParam_p->inId_m;
    tbufInitParam.pAckBase_m = pInitParam_p->pConsAckBase_m;
    tbufInitParam.pBase_m = (UINT8 *)pInitParam_p->pInTbufBase_m;
    tbufInitParam.size_m = pInitParam_p->inTbufSize_m;

    statusInstance_l.pTbufInInstance_m = tbuf_create(&tbufInitParam);
    if(statusInstance_l.pTbufInInstance_m == NULL)
    {
        ret = kAppIfStatusInitError;
        goto Exit;
    }

Exit:
    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Cleanup status module

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_exit(void)
{
    // Disable synchronous interrupt
    status_disableSyncInt();

    // Destroy triple buffer
    tbuf_destroy(statusInstance_l.pTbufOutInstance_m);
}

//------------------------------------------------------------------------------
/**
\brief    Set current cycle time

This function provides the current cycle time to the status module

\param[in] cycleTime_p      The cycle time to set

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_setCycleTime(UINT32 cycleTime_p)
{
    statusInstance_l.cycleTime_m = cycleTime_p;
}

//------------------------------------------------------------------------------
/**
\brief    Reset the relative time internals

Reset the relative time state machine and the relative time values.

\return tAppIfStatus
\retval kAppIfSuccessful       On success

\ingroup module_status
*/
//------------------------------------------------------------------------------
tAppIfStatus status_resetRelTime(void)
{
    tAppIfStatus ret = kAppIfSuccessful;

    statusInstance_l.relTimeState_m = kStatusRelTimeStateWaitFirstValidTime;
    statusInstance_l.relTimeLow_m = 0;
    statusInstance_l.relTimeHigh_m = 0;

    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Get the low value of the relative time

\param[out] pRelTimeLow_p        The current relative time

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_getRelativeTimeLow(UINT32* pRelTimeLow_p)
{
    *pRelTimeLow_p = statusInstance_l.relTimeLow_m;
}

//------------------------------------------------------------------------------
/**
\brief    Process the status module

Process all synchronous actions of the status module. This function needs to
be called in every POWERLINK cycle.

\return tAppIfStatus
\retval kAppIfSuccessful       On success

\ingroup module_status
*/
//------------------------------------------------------------------------------
tAppIfStatus status_process(tTimeInfo* pTime_p)
{
    tAppIfStatus ret = kAppIfSuccessful;

    // Process outgoing status registers
    ret = status_processOut(pTime_p);
    if(ret != kAppIfSuccessful)
    {
        goto Exit;
    }

    // Process incoming status registers
    ret = status_processIn();
    if(ret != kAppIfSuccessful)
    {
        goto Exit;
    }

Exit:
    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    enable the synchronous interrupt

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_enableSyncInt(void)
{
    /* Enable the interrupt in the EPL status sync module*/
    EplTimerSynckExtSyncIrqEnable(SYNC_INT_CYCLE_NUM, SYNC_INT_PULSE_WIDTH_NS);
}

//------------------------------------------------------------------------------
/**
\brief    disable the synchronous interrupt

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_disableSyncInt(void)
{
    /* Disable the interrupt in the EPL status sync module*/
    EplTimerSynckExtSyncIrqDisable();
}

//------------------------------------------------------------------------------
/**
\brief    Set the ICC busy flag

\param[in] seqNr_p        The sequence number of the icc

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_setIccStatus(tSeqNrValue seqNr_p)
{
    if(seqNr_p == kSeqNrValueFirst)
    {
        statusInstance_l.iccStatus_m &= ~(1<<STATUS_ICC_BUSY_FLAG_POS);
    }
    else
    {
        statusInstance_l.iccStatus_m |= (1<<STATUS_ICC_BUSY_FLAG_POS);
    }

}

//------------------------------------------------------------------------------
/**
\brief    Set the asynchronous transmit consuming channel to next element

\param[in] chanNum_p     Id of the channel to acknowledge
\param[in] seqNr_p       The value of the sequence number

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_setAsyncConsChanFlag(UINT8 chanNum_p, tSeqNrValue seqNr_p)
{
    if(seqNr_p == kSeqNrValueFirst)
    {
        statusInstance_l.asyncConsStatus_m &= ~(1<<chanNum_p);
    }
    else
    {
        statusInstance_l.asyncConsStatus_m |= (1<<chanNum_p);
    }

}


//------------------------------------------------------------------------------
/**
\brief    Get the asynchronous receive producing channel status

\param[in]  chanNum_p     Id of the channel to mark as busy
\param[out] seqNr_p       The value of the sequence number

\return tAppIfStatus
\retval kAppIfSuccessful      On success
\retval kAppIfTbuffReadError  Unable to read sequence number from buffer

\ingroup module_status
*/
//------------------------------------------------------------------------------
void status_getAsyncProdChanFlag(UINT8 chanNum_p, tSeqNrValue* pSeqNr_p)
{
    // Reformat to sequence number type
    if(CHECK_BIT(statusInstance_l.asyncProdStatus_m ,chanNum_p))
    {
        *pSeqNr_p = kSeqNrValueSecond;
    }
    else
    {
        *pSeqNr_p = kSeqNrValueFirst;
    }
}

//============================================================================//
//            P R I V A T E   F U N C T I O N S                               //
//============================================================================//
/// \name Private Functions
/// \{

//------------------------------------------------------------------------------
/**
\brief    Process outgoing status registers

\param[in] pTime_p             Time information field

\return tAppIfStatus
\retval kAppIfSuccessful         On success
\retval kAppIfTbuffWriteError    Error on writing

\ingroup module_status
*/
//------------------------------------------------------------------------------
static tAppIfStatus status_processOut(tTimeInfo* pTime_p)
{
    tAppIfStatus ret = kAppIfSuccessful;

    ret = status_calcRelTime(pTime_p);
    if(ret != kAppIfSuccessful)
    {
        goto Exit;
    }

    // Write icc status field to buffer
    ret = tbuf_writeByte(statusInstance_l.pTbufOutInstance_m, TBUF_ICC_STATUS_OFF,
            statusInstance_l.iccStatus_m);
    if(ret != kAppIfSuccessful)
    {
        goto Exit;
    }

    // Write aynchronous channels status field to buffer
    ret = tbuf_writeWord(statusInstance_l.pTbufOutInstance_m, TBUF_ASYNC_CONS_STATUS_OFF,
            statusInstance_l.asyncConsStatus_m);
    if(ret != kAppIfSuccessful)
    {
        goto Exit;
    }

    // Set acknowledge byte
    tbuf_setAck(statusInstance_l.pTbufOutInstance_m);

Exit:
    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Process incoming status registers

\return tAppIfStatus
\retval kAppIfSuccessful        On success
\retval kAppIfTbuffReadError    Error on reading

\ingroup module_status
*/
//------------------------------------------------------------------------------
static tAppIfStatus status_processIn(void)
{
    tAppIfStatus ret = kAppIfSuccessful;

    // Set acknowledge byte
    tbuf_setAck(statusInstance_l.pTbufInInstance_m);

    // Read asynchronous channels status field from buffer
    ret = tbuf_readWord(statusInstance_l.pTbufInInstance_m, TBUF_ASYNC_PROD_STATUS_OFF,
            &statusInstance_l.asyncProdStatus_m);
    if(ret != kAppIfSuccessful)
    {
        goto Exit;
    }

Exit:
    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Increment the relative time and check for overflow

\param[out] pRelTimeLow_p         Relative time low word
\param[out] pRelTimeHigh_p        Relative time high word
\param[in]  pCycleTime_p          Cycle time of the CN

\ingroup module_status
*/
//------------------------------------------------------------------------------
static void status_incRelTime(UINT32* pRelTimeLow_p, UINT32* pRelTimeHigh_p,
        UINT32* pCycleTime_p)
{
    // Increment it once to be up to date
    *pRelTimeLow_p += *pCycleTime_p;

    // Check for an overflow
    if(*pRelTimeLow_p < *pCycleTime_p)
    {
        (*pRelTimeHigh_p)++;
    }
}

//------------------------------------------------------------------------------
/**
\brief    Write the relative time to the buffer

\param relTimeLow_p         Relative time low word
\param relTimeHigh_p        Relative time high word

\ingroup module_status
*/
//------------------------------------------------------------------------------
static tAppIfStatus status_setRelTime(UINT32 relTimeLow_p, UINT32 relTimeHigh_p)
{
    tAppIfStatus ret = kAppIfSuccessful;

    ret = tbuf_writeDword(statusInstance_l.pTbufOutInstance_m, TBUF_RELTIME_LOW_OFF,
            statusInstance_l.relTimeLow_m);
    if(ret != kAppIfSuccessful)
    {
        goto Exit;
    }

    ret = tbuf_writeDword(statusInstance_l.pTbufOutInstance_m, TBUF_RELTIME_HIGH_OFF,
            statusInstance_l.relTimeHigh_m);

Exit:
    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Handle the current relative time value

Calculate the current relative time and forward it to the application interface.

\param  pTime_p             Time information for calculating the relative time

\return tAppIfStatus
\retval kAppIfSuccessful                 On success
\retval kAppIfStatusRelTimeStateError    Invalid relative time state

\ingroup module_status
*/
//------------------------------------------------------------------------------
static tAppIfStatus status_calcRelTime(tTimeInfo* pTime_p)
{
    tAppIfStatus  ret = kAppIfSuccessful;

    switch(statusInstance_l.relTimeState_m)
    {
        case kStatusRelTimeStateWaitFirstValidTime:
        {
            if(pTime_p->fCnIsOperational_m != FALSE)
            {
                if(pTime_p->fTimeValid_m != FALSE)
                {
                    // read the value once and activate relative status
                    statusInstance_l.relTimeLow_m = pTime_p->relativeTimeLow_m;
                    statusInstance_l.relTimeHigh_m = pTime_p->relativeTimeHigh_m;

                    status_incRelTime(&statusInstance_l.relTimeLow_m, &statusInstance_l.relTimeHigh_m,
                            &statusInstance_l.cycleTime_m);

                    ret = status_setRelTime(statusInstance_l.relTimeLow_m, statusInstance_l.relTimeHigh_m);
                    if(ret != kAppIfSuccessful)
                    {
                        goto Exit;
                    }

                    statusInstance_l.relTimeState_m = kStatusRelTimeStateActiv;
                } else {
                    /* CN is operational but RelativeTime is still not Valid!
                     * (We now start counting without an offset) */
                    status_incRelTime(&statusInstance_l.relTimeLow_m, &statusInstance_l.relTimeHigh_m,
                            &statusInstance_l.cycleTime_m);

                    status_setRelTime(statusInstance_l.relTimeLow_m, statusInstance_l.relTimeHigh_m);
                    if(ret != kAppIfSuccessful)
                    {
                        goto Exit;
                    }

                    statusInstance_l.relTimeState_m = kStatusRelTimeStateActiv;
                }
            } else {
                /* increment local RelativeTime and wait for an arriving status
                 * value from the SoC */
                status_incRelTime(&statusInstance_l.relTimeLow_m, &statusInstance_l.relTimeHigh_m,
                        &statusInstance_l.cycleTime_m);
            }
           break;
        }
        case kStatusRelTimeStateActiv:
        {
            status_incRelTime(&statusInstance_l.relTimeLow_m, &statusInstance_l.relTimeHigh_m,
                    &statusInstance_l.cycleTime_m);

            status_setRelTime(statusInstance_l.relTimeLow_m, statusInstance_l.relTimeHigh_m);
            if(ret != kAppIfSuccessful)
            {
                goto Exit;
            }

           break;
        }
        case kStatusRelTimeStateInvalid:
        {
            ret = kAppIfStatusRelTimeStateError;
            break;
        }
        default:
        {
            // do nothing here!
            break;
        }

    }

Exit:
    return ret;
}

/// \}

