/* devWfJoergerVtr1.c */
/* share/src/dev $Id$ */

/* devWfJoergerVtr1.c - Device Support Routines */
/*
 *      Original Author: Bob Dalesio
 *      Current Author:  Marty Kraimer
 *      Date:            6-1-90
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *              The Controls and Automation Group (AT-8)
 *              Ground Test Accelerator
 *              Accelerator Technology Division
 *              Los Alamos National Laboratory
 *
 *      Co-developed with
 *              The Controls and Computing Group
 *              Accelerator Systems Division
 *              Advanced Photon Source
 *              Argonne National Laboratory
 *
 * Modification Log:
 * -----------------
 * .01  mm-dd-yy        iii     Comment
 * .02  mm-dd-yy        iii     Comment
 *      ...
 */


#include	<vxWorks.h>
#include	<types.h>
#include	<stdioLib.h>

#include	<alarm.h>
#include	<dbDefs.h>
#include	<dbAccess.h>
#include	<devSup.h>
#include	<link.h>
#include	<module_types.h>
#include	<waveformRecord.h>

long init_record();
long get_ioint_info();
long read_wf();
long arm_wf();


struct {
	long		number;
	DEVSUPFUN	report;
	DEVSUPFUN	init;
	DEVSUPFUN	init_record;
        DEVSUPFUN       get_ioint_info;
	DEVSUPFUN	read_wf;
} devWfJoergerVtr1={
	5,
	NULL,
	NULL,
	init_record,
	get_ioint_info,
	read_wf};
/* control block for callback */
struct callback{
	struct dbAddr dbAddr;
        void (*process)();
};


static void myCallback(pcallback,no_read,pdata)
    struct callback *pcallback;
    int		    no_read;
    unsigned short   *pdata;
{
        struct waveformRecord   *pwf=
		(struct waveformRecord *)(pcallback->dbAddr.precord);
	short ftvl = pwf->ftvl;
	long   i;

	if(!pwf->busy) return;
        dbScanLock(pwf);
	pwf->busy = FALSE;
	if(no_read>pwf->nelm)no_read = pwf->nelm;
	if(ftvl==DBF_CHAR || ftvl==DBF_UCHAR) {
		unsigned char *pdest=(unsigned char *)pwf->bptr;

		for(i=0; i<no_read; i++) {
			*pdest++ = *pdata++;
		}
       		pwf->nord = no_read;            /* number of values read */
	} else if(ftvl==DBF_SHORT || ftvl==DBF_USHORT) {
		unsigned short *pdest=(unsigned short *)pwf->bptr;

		for(i=0; i<no_read; i++) {
			*pdest++ = *pdata++;
		}
       		pwf->nord = no_read;            /* number of values read */
	} else {
		recGblRecSupError(S_db_badField,&pcallback->dbAddr,
			"read_wf - illegal ftvl");
		if(pwf->nsev<VALID_ALARM ) {
                        pwf->nsta = READ_ALARM;
                        pwf->nsev = VALID_ALARM;

                }
	}
	(pcallback->process)(&pcallback->dbAddr);
        dbScanUnlock(pwf);
}

static long get_ioint_info(pwf,io_type,card_type,card_number)
    struct waveformRecord     *pwf;
    short               *io_type;
    short               *card_type;
    short               *card_number;
{
    if(pwf->inp.type != VME_IO) return(S_dev_badInpType);
    *io_type = IO_WF;
    *card_type = JGVTR1;
    *card_number = pwf->inp.value.vmeio.card;
    return(0);
}


static long init_record(pwf,process)
    struct waveformRecord	*pwf;
    void (*process)();
{
    char message[100];
    struct callback *pcallback;

    /* wf.inp must be an VME_IO */
    switch (pwf->inp.type) {
    case (VME_IO) :
        pcallback = (struct callback *)(calloc(1,sizeof(struct callback)));
        pwf->dpvt = (caddr_t)pcallback;
        if(dbNameToAddr(pwf->name,&(pcallback->dbAddr))) {
                logMsg("dbNameToAddr failed in init_record for devWfJoergerVtr1\n");
                exit(1);
        }
        pcallback->process = process;
	break;
    default :
	strcpy(message,pwf->name);
	strcat(message,": devWfJoergerVtr1 (init_record) Illegal INP field");
	errMessage(S_db_badField,message);
	return(S_db_badField);
    }
    return(0);
}

static long read_wf(pwf)
    struct waveformRecord	*pwf;
{
	char message[100];
	struct callback *pcallback=(struct callback *)(pwf->dpvt);
	unsigned short value;
	struct vmeio *pvmeio;
	long status;

	
	if(pwf->busy) return(1);
	status = 0;
	/* determine if wave form is to be rearmed*/
	/* If not active then request rearm */
	if(!pwf->pact) status = arm_wf(pwf);
	/* if already active then call is from myCallback. check rarm*/
	else if(pwf->rarm) {
		(void)arm_wf(pwf);
	}
	return(status);
}

static long arm_wf(pwf)
struct waveformRecord   *pwf;
{
	struct vmeio *pvmeio = (struct vmeio *)&(pwf->inp.value);

	pwf->busy = TRUE;
	if(wf_driver(JGVTR1,pvmeio->card,myCallback,pwf->dpvt)<0){
		if(pwf->nsev<VALID_ALARM ) {
                	pwf->nsta = READ_ALARM;
                	pwf->nsev = VALID_ALARM;

		}
		pwf->busy = FALSE;
		return(0);
	}
	return(1);
}
