/*----------------------------------------------------------------------------------
 *  Solve acoustic forward problem with FDFD for different stages and frequencies
 *
 *  D. Koehn
 *  Kiel, 22.06.2016
 *  --------------------------------------------------------------------------------*/
#include "fd.h"

void forward_AC(char *fileinp1){

	/* declaration of global variables */
      	extern int MYID, NF, MYID, LOG, NXG, NYG, NX, NY, NXNY, INFO, INVMAT, N_STREAMER;
	extern int READMOD, NX0, NY0, NPML, READ_REC, FSSHIFT, NFREQ1, NFREQ2, COLOR;
	extern int NPROCFREQ, NPROCSHOT, NP, MYID_SHOT, NSHOT1, NSHOT2;
	extern float FC_low, FC_high, A0_PML;
	extern char LOG_FILE[STRING_SIZE];
	extern FILE *FP;
    
        /* declaration of local variables */
	int i, j, nstage, stagemax, nfreq;
	int ntr, nshots;
        char ext[10];
	float *stage_freq;

	FILE *FP_stage;

	/* open log-file (each PE is using different file) */
	/*	fp=stdout; */
	sprintf(ext,".%i",MYID);  
	strcat(LOG_FILE,ext);

	if ((MYID==0) && (LOG==1)) FP=stdout;
	else FP=fopen(LOG_FILE,"w");
	fprintf(FP," This is the log-file generated by PE %d \n\n",MYID);

	/* output of parameters to log-file or stdout */
	if (MYID==0) write_par(FP);

	/* store old NX and NY values */
	NX0 = NX;
        NY0 = NY;

	/* add external PML layers */
	NX += 2 * NPML;
	NY += NPML + FSSHIFT;

	NXG = NX;
	NYG = NY;

	/* size of impedance matrix */
	NXNY = NX * NY;

	/* Calculate number of non-zero elements in impedance matrix */
	calc_nonzero();

	/* define data structures for acoustic problem */
	struct waveAC;
	struct matAC;
	struct PML_AC;
	struct acq;

	/* allocate memory for acoustic forward problem */
	alloc_waveAC(&waveAC,&PML_AC);
	alloc_matAC(&matAC);

	/* If INVMAT!=0 deactivate unnecessary output */
	INFO=0;

	/* If INVMAT==0 allow unnecessary output */
	if(INVMAT==0){
	   INFO=1;
	}

	/*if (MYID == 0) info_mem(stdout,NLBFGS_vec,ntr);*/

	/* Reading source positions from SOURCE_FILE */ 	
	acq.srcpos=sources(&nshots);

	/* read receiver positions from receiver files for each shot */
	if(READ_REC==0){

	    acq.recpos=receiver(FP, &ntr, 1);

	    /* Allocate memory for FD seismograms */
	    alloc_seis_AC(&waveAC,ntr);

	    if(N_STREAMER>0){
	      free_imatrix(acq.recpos,1,3,1,ntr);
	    }			                         

	}

	/* read/create P-wave velocity */
	if (READMOD){
	    readmod(&matAC); 
	}else{
	    model(matAC.vp);
	}

	/* read parameters from workflow-file (stdin) */
	FP=fopen(fileinp1,"r");
	if(FP==NULL) {
		if (MYID == 0){
			printf("\n==================================================================\n");
			printf(" Cannot open GERMAINE workflow input file %s \n",fileinp1);
			printf("\n==================================================================\n\n");
			err(" --- ");
		}
	}

	/* estimate number of lines in FWI-workflow */
	i=0;
	stagemax=0;
	while ((i=fgetc(FP)) != EOF)
	if (i=='\n') ++stagemax;
	rewind(FP);
	stagemax--;
	fclose(FP);
     
	/* initiate vp, ivp2, k2 */
	init_mat_AC(&waveAC,&matAC);

	/* loop over GERMAINE workflow stages */
	for(nstage=1;nstage<=stagemax;nstage++){

		/* read workflow input file *.inp */
		FP_stage=fopen(fileinp1,"r");
		read_par_inv(FP_stage,nstage,stagemax);

		/* estimate frequency sample interval */
		waveAC.dfreq = (FC_high-FC_low) / NF;

		/* estimate frequencies for current FWI stage */
		waveAC.stage_freq = vector(1,NF);
		waveAC.stage_freq[1] = FC_low;

		for(i=2;i<=NF;i++){
		    waveAC.stage_freq[i] = waveAC.stage_freq[i-1] + waveAC.dfreq; 
		} 		

		/* split MPI communicator for shot parallelization */
		COLOR = MYID / NPROCFREQ;

		MPI_Comm shot_comm;
		MPI_Comm_split(MPI_COMM_WORLD, COLOR, MYID, &shot_comm);		

		/* esimtate communicator size for shot_comm and number of colors (NPROCSHOT) */
		MPI_Comm_rank(shot_comm, &MYID_SHOT);
		NPROCSHOT = NP / NPROCFREQ;

		/* Initiate MPI shot parallelization */
		init_MPIshot(nshots);

		/* Initiate MPI frequency parallelization */		
		init_MPIfreq();

		/* loop over frequencies at each stage */
		for(nfreq=NFREQ1;nfreq<NFREQ2;nfreq++){			

			/* set frequency on local MPI process */
			waveAC.freq = waveAC.stage_freq[nfreq];			

			/* define PML damping profiles */
			pml_pro(&PML_AC,&waveAC);

			/* set squared angular frequency */
			waveAC.omega2 = pow(2.0*M_PI*waveAC.freq,2.0);

			/* update material parameters */
			init_mat_AC(&waveAC,&matAC);
	
			/* solve forward problem for all shots*/
			forward_shot_AC(&waveAC,&PML_AC,&matAC,acq.srcpos,nshots,acq.recpos,ntr,nstage,nfreq);

		} /* end of loop over frequencies */

		/* free shot_comm */
		MPI_Comm_free(&shot_comm);

  	} /* end of loop over workflow stages*/

	if(READ_REC==0){free_imatrix(acq.recpos,1,3,1,ntr);}
	
}
