
//
// $Id$
//
// Author: Jeff Hill
//
//

//
// ANSI C
//
#include <math.h>
#include <time.h>
#include <limits.h>

//
// WIN32
//
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN 
#include <winsock2.h>
#include <process.h>

/*
 * for _ftime()
 */
#include <sys/types.h>
#include <sys/timeb.h>

//
// EPICS
//
#define epicsExportSharedSymbols
#include "epicsTime.h"
#include "errlog.h"
#include "epicsAssert.h"

//
// performance counter last value, ticks per sec,
// and its offset from the EPICS epoch.
// 
static LONGLONG perf_last, perf_freq;

//
// divide the offset into seconds and
// fractions of a second so that overflow is less
// likely (we dont know the magnitude of perf_freq
// until run time)
//
static LONGLONG perf_sec_offset=-1, perf_frac_offset;

static const SYSTEMTIME epicsEpochST = {
	1990, // year
	1, // month
	1, // day of the week (Monday)
	1, // day of the month
	0, // hour
	0, // min
	0, // sec
	0 // milli sec
};

static const LONGLONG FILE_TIME_TICKS_PER_SEC = 10000000L;
static HANDLE osdTimeMutex = NULL;
static bool osdTimeSyncThreadExit = false;
static LONGLONG epicsEpoch;

static int osdTimeSych ();

/*
 * osdTimeExit ()
 */
static void osdTimeExit ()
{
    if ( osdTimeMutex ) {
        CloseHandle (osdTimeMutex);
    }
    osdTimeSyncThreadExit = true;
}

/*
 * epicsWin32ThreadEntry()
 */
static unsigned __stdcall osdTimeSynchThreadEntry (LPVOID)
{
    static const DWORD tmoTenSec = 10 * epicsTime::mSecPerSec;
    int status;

    while ( ! osdTimeSyncThreadExit ) {
	    Sleep (tmoTenSec); 
        status = osdTimeSych ();
        if (status!=epicsTimeOK) {
            errlogPrintf ("osdTimeSych (): failed?\n");
        }
    }
    return 0u;
}

//
// osdTimeInit ()
//
static void osdTimeInit ()
{
	LARGE_INTEGER parm;
	BOOL success;
    int unixStyleStatus;
    HANDLE osdTimeThread;
    unsigned threadAddr;
	FILETIME epicsEpochFT;
    DWORD status;

    if ( osdTimeMutex ) {
        /* wait for init to complete */
        status = WaitForSingleObject ( osdTimeMutex, INFINITE );
        assert ( status == WAIT_OBJECT_0 );
        success = ReleaseMutex ( osdTimeMutex );
        assert (success);
        return;
    }
    else {
        HANDLE osdTimeMutexTmp;
        osdTimeMutexTmp = CreateMutex (NULL, TRUE, NULL);
        if ( osdTimeMutexTmp == 0 ) {
            return;
        }

#if 1
        /* not arch neutral, but at least supported by w95 and borland */
        if ( InterlockedExchange ( (LPLONG) &osdTimeMutex, (LONG) osdTimeMutexTmp ) ) {
#else

        /* not supported on W95, but the alternative requires assuming that pointer and integer are the same */
        if (InterlockedCompareExchange ( (PVOID *) &osdTimeMutex, (PVOID) osdTimeMutexTmp, (PVOID)0 ) != 0) {
#endif
            CloseHandle (osdTimeMutexTmp);
            /* wait for init to complete */
            status = WaitForSingleObject (osdTimeMutex, INFINITE);
            assert ( status == WAIT_OBJECT_0 );
            success = ReleaseMutex (osdTimeMutex);
            assert (success);
            return;
        }
    }

	//
	// initialize elapsed time counters
	//
	// All CPUs running win32 currently have HR
	// counters (Intel and Mips processors do)
	//
	if ( QueryPerformanceFrequency (&parm) == 0 ) {
        CloseHandle (osdTimeMutex);
        osdTimeMutex = NULL;
        return;
	}
	perf_freq = parm.QuadPart;

	//
	// convert the EPICS epoch to file time
	//
	success = SystemTimeToFileTime (&epicsEpochST, &epicsEpochFT);
    if ( ! success ) {
        CloseHandle ( osdTimeMutex );
        osdTimeMutex = NULL;
        return;
    }
	parm.LowPart = epicsEpochFT.dwLowDateTime;
	parm.HighPart = epicsEpochFT.dwHighDateTime;
	epicsEpoch = parm.QuadPart;

    ReleaseMutex ( osdTimeMutex );

    unixStyleStatus = osdTimeSych ();
    if ( unixStyleStatus != epicsTimeOK ) {
        CloseHandle ( osdTimeMutex );
        osdTimeMutex = NULL;
        return;
    }

    //
    // spawn off a thread which periodically resynchronizes the offset
    //
    osdTimeThread = (HANDLE) _beginthreadex ( NULL, 4096, osdTimeSynchThreadEntry, 
                0, 0, &threadAddr );
    if ( osdTimeThread == NULL ) {
        errlogPrintf ( "osdTimeInit(): unable to start time synch thread\n" );
    }
    else {
        assert ( CloseHandle ( osdTimeThread ) );
    }

    atexit ( osdTimeExit );
}

//
// osdTimeSych ()
//
static int osdTimeSych ()
{
    static const DWORD tmoTwentySec = 20 * epicsTime::mSecPerSec;
	LONGLONG new_sec_offset, new_frac_offset;
	LARGE_INTEGER parm;
	LONGLONG secondsSinceBoot;
	FILETIME currentTimeFT;
	LONGLONG currentTime;
	BOOL win32Stat;
    DWORD win32SemStat;

	if (!osdTimeMutex) {
        osdTimeInit ();
        if (!osdTimeMutex) {
            return epicsTimeERROR;
        }
    }

    win32SemStat = WaitForSingleObject (osdTimeMutex, tmoTwentySec);
    if ( win32SemStat != WAIT_OBJECT_0 ) {
        return epicsTimeERROR;
    }

	//
	// its important that the following two time queries
	// are close together (immediately adjacent to each
	// other) in the code
	//
	GetSystemTimeAsFileTime (&currentTimeFT);
	// this one is second because QueryPerformanceFrequency()
	// has forced its code to load
    if (QueryPerformanceCounter (&parm)==0) {
         ReleaseMutex (osdTimeMutex);
         return epicsTimeERROR;
    }
	 
	perf_last = parm.QuadPart;
	parm.LowPart = currentTimeFT.dwLowDateTime;
	parm.HighPart = currentTimeFT.dwHighDateTime;
	currentTime = parm.QuadPart;

	//
	// check for strange date, and clamp to the
	// epics epoch if so
	//
	if (currentTime>epicsEpoch) {
		//
		// compute the offset from the EPICS epoch
		//
		LONGLONG diff = currentTime - epicsEpoch;

		//
		// compute the seconds offset and the 
		// fractional offset part in the FILETIME timebase
		//
		new_sec_offset = diff / FILE_TIME_TICKS_PER_SEC;
		new_frac_offset = diff % FILE_TIME_TICKS_PER_SEC;

		//
		// compute the fractional second offset in the performance 
		// counter time base
		//	
		new_frac_offset = (new_frac_offset*perf_freq) / FILE_TIME_TICKS_PER_SEC;
	}
	else {
		new_sec_offset = 0;
		new_frac_offset = 0;
	}

	//
	// subtract out the perormance counter ticks since the 
	// begining of windows
	//	
	secondsSinceBoot = perf_last / perf_freq;
	if (new_sec_offset>=secondsSinceBoot) {
		new_sec_offset -= secondsSinceBoot;

		LONGLONG fracSinceBoot = perf_last % perf_freq;
		if (new_frac_offset>=fracSinceBoot) {
			new_frac_offset -= fracSinceBoot;
		}
		else if (new_sec_offset>0) {
			//
			// borrow
			//
			new_sec_offset--;
			new_frac_offset += perf_freq - fracSinceBoot;
		}
		else {
			new_frac_offset = 0;
		}
	}
	else {
		new_sec_offset = 0;
		new_frac_offset = 0;
	}

#if 0
	//
	// calculate the change
	//
	{
		double change;
		change = (double) (perf_sec_offset-new_sec_offset)*perf_freq +
			(perf_frac_offset-new_frac_offset);
		change /= perf_freq;
		printf ("The performance counter drifted by %f sec\n", change);
	}
#endif

	//
	// update the current value
	//
	perf_sec_offset = new_sec_offset;
	perf_frac_offset = new_frac_offset;

    win32Stat = ReleaseMutex (osdTimeMutex);
    if (!win32Stat) {
        return epicsTimeERROR;
    }

    return epicsTimeOK;
}

//
// epicsTime::osdGetCurrent ()
//
extern "C" epicsShareFunc int epicsShareAPI epicsTimeGetCurrent (epicsTimeStamp *pDest)
{
    static const DWORD tmoTwentySec = 20 * epicsTime::mSecPerSec;
	LONGLONG time_cur, time_sec, time_remainder;
	LARGE_INTEGER parm;
    BOOL status;

	//
	// lazy init
	//
	if (!osdTimeMutex) {
        osdTimeInit ();
        if (!osdTimeMutex) {
            return epicsTimeERROR;
        }
	}

    status = WaitForSingleObject (osdTimeMutex, tmoTwentySec);
    if ( status != WAIT_OBJECT_0 ) {
        return epicsTimeERROR;
    }

	//
	// dont need to check status since it was checked once
	// during initialization to see if the CPU has HR
	// counters (Intel and Mips processors do)
	//
	status = QueryPerformanceCounter (&parm);
    if (!status) {
        ReleaseMutex (osdTimeMutex);
        return epicsTimeERROR;
    }
	time_cur = parm.QuadPart;
	if (perf_last > time_cur) {	

		//
		// must have been a timer roll-over
		// It takes 9.223372036855e+18/perf_freq sec
		// to roll over this counter (perf_freq is 1193182
		// sec on my system). This is currently about 245118 years.
		//
		// attempt to add number of seconds in a 64 bit integer
		// in case the timer resolution improves
		//
		perf_sec_offset += MAXLONGLONG / perf_freq;
		perf_frac_offset += MAXLONGLONG % perf_freq;
		if (perf_frac_offset>=perf_freq) {
			perf_sec_offset++;
			perf_frac_offset -= perf_freq;
		}
	}
	time_sec = time_cur / perf_freq;
	time_remainder = time_cur % perf_freq;

	//
	// add time (sec) since the EPICS epoch 
	//
	time_sec += perf_sec_offset;
	time_remainder += perf_frac_offset;
	if (time_remainder>=perf_freq) {
		time_sec += 1;
		time_remainder -= perf_freq;
	}

	perf_last = time_cur;

	pDest->secPastEpoch = (unsigned long) (time_sec%ULONG_MAX);
    pDest->nsec = (unsigned long) ((time_remainder*epicsTime::nSecPerSec)/perf_freq);

    status = ReleaseMutex (osdTimeMutex);
    if (!status) {
        return epicsTimeERROR;
    }

	return epicsTimeOK;
}

//
// epicsTimeGetEvent ()
//
extern "C" epicsShareFunc int epicsShareAPI epicsTimeGetEvent (epicsTimeStamp *pDest, unsigned eventNumber)
{
    if (eventNumber==epicsTimeEventCurrentTime) {
        return epicsTimeGetCurrent (pDest);
    }
    else {
        return epicsTimeERROR;
    }
}

//
// gmtime_r ()
//
// from posix real time
//
struct tm *gmtime_r (const time_t *pAnsiTime, struct tm *pTM)
{
	HANDLE thisThread = GetCurrentThread ();
	struct tm *p;
	int oldPriority;
	BOOL win32Success;

	oldPriority = GetThreadPriority (thisThread);
	if (oldPriority==THREAD_PRIORITY_ERROR_RETURN) {
		return NULL;
	}

	win32Success = SetThreadPriority (thisThread, THREAD_PRIORITY_TIME_CRITICAL);
	if (!win32Success) {
		return NULL;
	}

	p = gmtime (pAnsiTime);
	if (p!=NULL) {
		*pTM = *p;
	}

	win32Success = SetThreadPriority (thisThread, oldPriority);
	if (!win32Success) {
        return NULL;
    }

	return p;
}

//
// localtime_r ()
//
// from posix real time
//
struct tm *localtime_r (const time_t *pAnsiTime, struct tm *pTM)
{
	HANDLE thisThread = GetCurrentThread ();
	struct tm *p;
	int oldPriority;
	BOOL win32Success;

	oldPriority = GetThreadPriority (thisThread);
	if (oldPriority==THREAD_PRIORITY_ERROR_RETURN) {
		return NULL;
	}

	win32Success = SetThreadPriority (thisThread, THREAD_PRIORITY_TIME_CRITICAL);
	if (!win32Success) {
		return NULL;
	}

	p = localtime (pAnsiTime);
	if (p!=NULL) {
		*pTM = *p;
	}

	win32Success = SetThreadPriority (thisThread, oldPriority);
	if (!win32Success) {
        return NULL;
    }
	
	return p;
}
