/*------------------------------------------------------------------------
 *  Acoustic FDFD Full Waveform Inversion
 *
 *  D. Koehn
 *  Kiel, 17.05.2017
 *  ----------------------------------------------------------------------*/

#include "fd.h"

void fwi_FD_AC(char *fileinp1){

	/* declaration of global variables */
        extern int NX, NY, NSHOT1, NSHOT2, GRAD_METHOD, NLBFGS, MYID, ITERMAX, LINESEARCH;
	extern int NXG, NYG, NXNY, LOG, N_STREAMER, INFO, INVMAT, READMOD;
	extern int NX0, NY0, NPML, READ_REC, HESSIAN, FSSHIFT;
        extern int NPROCFREQ, NPROCSHOT, NP, MYID_SHOT, NSHOT1, NSHOT2, COLOR, NF;
        extern char MISFIT_LOG_FILE[STRING_SIZE], LOG_FILE[STRING_SIZE];
        extern float PRO, A0_PML, FC_high, FC_low;

	extern FILE *FP;
    
        /* declaration of local variables */
        int i, j;
        char ext[10];
	int ntr, nshots;

        /* variables for the L-BFGS method */
	float * rho_LBFGS, * alpha_LBFGS, * beta_LBFGS; 
	float * y_LBFGS, * s_LBFGS, * q_LBFGS, * r_LBFGS;
	int NLBFGS_class, LBFGS_pointer, NLBFGS_vec;

	/* Variables for PCG */
	float * PCG_old, * PCG_new, * PCG_dir;
	int PCG_class, PCG_vec;

	/*vector for abort criterion*/
	float * L2_hist=NULL;

	/* variables for workflow */
	int nstage, stagemax, iter, iter_true;
        
        /* variables for step-length estimation */
        float eps_scale, L2, *L2t, diff;

        FILE *FP_stage, *FPL2;

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

	    fwiAC.presr = vector(1,ntr);
 	    fwiAC.presi = vector(1,ntr);

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


	/* memory allocation for abort criterion*/
	L2_hist = vector(1,ITERMAX*stagemax);

	/* Variables for PCG method */
	if(GRAD_METHOD==1){

	  PCG_class = 1;                 /* number of parameter classes */ 
	  PCG_vec = PCG_class*NX*NY;  	 /* length of one PCG-parameter class */
	  
	  PCG_old  =  vector(1,PCG_vec);
	  PCG_new  =  vector(1,PCG_vec);
	  PCG_dir  =  vector(1,PCG_vec);
	 
	}

	/* Variables for the l-BFGS method */
	if(GRAD_METHOD==2){

	  NLBFGS_class = 1;                 /* number of parameter classes */ 
	  NLBFGS_vec = NLBFGS_class*NX*NY;  /* length of one LBFGS-parameter class */
	  LBFGS_pointer = 1;                /* initiate pointer in the cyclic LBFGS-vectors */
	  
	  y_LBFGS  =  vector(1,NLBFGS_vec*NLBFGS);
	  s_LBFGS  =  vector(1,NLBFGS_vec*NLBFGS);

	  q_LBFGS  =  vector(1,NLBFGS_vec);
	  r_LBFGS  =  vector(1,NLBFGS_vec);

	  rho_LBFGS = vector(1,NLBFGS);
	  alpha_LBFGS = vector(1,NLBFGS);
	  beta_LBFGS = vector(1,NLBFGS);
	  
	}

	/* define data structures and allocate memory for acoustic FWI problem */
	struct fwiAC;
	alloc_fwiAC(&fwiAC,ntr);

	/* memory of L2 norm */
	L2t = vector(1,4);

	/* initiate vp, ivp2, k2 */
	init_mat_AC(&waveAC,&matAC);

	iter_true=1;
	/* Begin of FWI inversion workflow */
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

		iter=1;
		/* --------------------------------------
		 * Begin of FWI iteration loop
		 * -------------------------------------- */
		while(iter<=ITERMAX){

			if(GRAD_METHOD==2){
			  			  
			    /* if LBFGS-pointer > NLBFGS -> rotate l-BFGS vector */ 
			    if(LBFGS_pointer>NLBFGS){			    
			        rot_LBFGS_vec(y_LBFGS, s_LBFGS, NLBFGS, NLBFGS_vec);
			        LBFGS_pointer = NLBFGS;
			    }

			}

			if (MYID==0){
			   printf("\n\n\n ------------------------------------------------------------------\n");
			   printf("\n\n\n                   FWI ITERATION %d \t of %d \n",iter,ITERMAX);
			   printf("\n\n\n ------------------------------------------------------------------\n");
			}

			/* Open Log File for L2 norm */
			if(MYID==0){
			    if(iter_true==1){
			      FPL2=fopen(MISFIT_LOG_FILE,"w");
			    }

			    if(iter_true>1){
			      FPL2=fopen(MISFIT_LOG_FILE,"a");
			    }
			}

			   /* FD Full Waveform Inversion (FWI) */
			   /* -------------------------------- */

			   /* calculate Vp gradient and objective function */
 			   L2 = grad_obj_AC(&fwiAC,&waveAC,&PML_AC,&matAC,acq.srcpos,nshots,acq.recpos,ntr,iter,nstage);
			   L2t[1] = L2;

			   /* calculate and apply approximate Hessian at first iteration of each frequency group */
			   if(HESSIAN){

			       /* calculate approximate Hessian */
			       if((HESSIAN==1)&&(iter==1)){
			           hessian_AC(&fwiAC,&waveAC,&PML_AC,&matAC,acq.srcpos,nshots,acq.recpos,ntr,iter,nstage);
			       }
			       
			       /* calculate Pseudo-Hessian */
			       if((HESSIAN==2)&&(iter==1)){
			           hessian_shin_AC(&fwiAC,&waveAC,&PML_AC,&matAC,acq.srcpos,nshots,acq.recpos,ntr,iter,nstage);
			       }

   			       /* apply approximate Hessian to gradient */			    
			       apply_hess_AC(fwiAC.grad,fwiAC.hess);

			   }

			   /* apply smoothing and taper functions to gradient */
			   precond(fwiAC.grad);

			   /* calculate descent directon gradm from gradient grad */
			   cp_grad_frame(fwiAC.grad);
			   descent(fwiAC.grad,fwiAC.gradm);

			   /* estimate search direction waveconv with ... */

			   /* ... non-linear preconditioned conjugate gradient method */
			   if(GRAD_METHOD==1){

			       MPI_Barrier(MPI_COMM_WORLD);	
	    
			       /* store current steepest decent direction in PCG_new vector */
			       store_PCG_AC(PCG_new,fwiAC.gradm);

			       /* apply PCG method */
			       PCG(PCG_new,PCG_old,PCG_dir,PCG_class);

			       /* extract CG-search directions */
			       extract_PCG_AC(PCG_dir,fwiAC.Hgrad);

			       /* store old steepest descent direction in PCG_old vector */
			       store_PCG_AC(PCG_old,fwiAC.gradm);

			   }

			   /* ... quasi-Newton l-BFGS method */
			   if(GRAD_METHOD==2){                              
  			      MPI_Barrier(MPI_COMM_WORLD);
			      LBFGS(fwiAC.Hgrad,fwiAC.grad,fwiAC.gradm,iter,y_LBFGS,s_LBFGS,rho_LBFGS,alpha_LBFGS,matAC.vp,q_LBFGS,r_LBFGS,beta_LBFGS,LBFGS_pointer,NLBFGS,NLBFGS_vec);
			   }

                           /* ... Descent method */
                           if(GRAD_METHOD==3){
                              MPI_Barrier(MPI_COMM_WORLD);
                              descent(fwiAC.grad,fwiAC.Hgrad);
                           }

			   /* check if search direction is a descent direction, otherwise reset l-BFGS history */
			   check_descent(fwiAC.Hgrad,fwiAC.grad,NLBFGS_vec,y_LBFGS,s_LBFGS,iter);

			   /* Estimate optimum step length ... */
			   MPI_Barrier(MPI_COMM_WORLD);

			   /* ... by line search which satisfies the Wolfe conditions */
                           /*if(LINESEARCH==1){
			   eps_scale=wolfels(Hgrad,grad,Vp,S,TT,lam,Tmod,Tobs,Tres,srcpos,nshots,recpos,ntr,iter,eps_scale,L2);}*/
			   
			   /* ... by inexact parabolic line search */
                           if(LINESEARCH==2){
			      eps_scale = parabolicls_AC(&fwiAC,&waveAC,&PML_AC,&matAC,acq.srcpos,nshots,acq.recpos,ntr,iter,nstage,eps_scale,L2);
			   }
			   

			   if(MYID==0){
			      fprintf(FPL2,"%e \t %e \t %d \n",eps_scale,L2t[1],nstage);
			   }

			   /* saving history of final L2*/
			   L2_hist[iter]=L2t[1];

			   /* calculate optimal change of the material parameters */
                           MPI_Barrier(MPI_COMM_WORLD);
          
			   /* copy vp -> vp_old */
	  		   store_mat(matAC.vp,fwiAC.vp_old,NX,NY);
			   calc_mat_change_wolfe(fwiAC.Hgrad,matAC.vp,fwiAC.vp_old,eps_scale,0);

			    if(MYID==0){
			       fclose(FPL2);
			    }

			    /* calculating difference of the actual L2 and before two iterations, dividing with L2_hist[iter-2] provide changing in percent*/
			    diff=1e20;
			    if(iter > 2){
			       diff=fabs((L2_hist[iter-2]-L2_hist[iter])/L2_hist[iter-2]);
			    }

			    if(GRAD_METHOD==2){
			  
			       /* increase pointer to LBFGS-vector*/
			       if(iter>1){
			           LBFGS_pointer++;
			       }			  			 

			    }

			    /* are convergence criteria satisfied? */	
			    if((diff<=PRO)||(eps_scale<1e-10)||(iter==ITERMAX)){
	
			       /* model output at the end of given workflow stage */
			       model_out(matAC.vp,nstage);
			       iter=0;

			       if(GRAD_METHOD==2){

				  zero_LBFGS(NLBFGS, NLBFGS_vec, y_LBFGS, s_LBFGS, q_LBFGS, r_LBFGS, alpha_LBFGS, beta_LBFGS, rho_LBFGS);
				  LBFGS_pointer = 1;  

			       }

			       if(MYID==0){

				  if(eps_scale<1e-10){

				     printf("\n Steplength estimation failed. Changing to next FWI stage \n");

				  }else{

				    printf("\n Reached the abort criterion of pro=%e and diff=%e \n Changing to next FWI stage \n",PRO,diff);
			
				  }

			       }
		
			       break;
			    }

		iter++;
		iter_true++;

		/* ====================================== */
		} /* end of FWI iteration loop*/
		/* ====================================== */

		/* free shot_comm and stage_freq */
		MPI_Comm_free(&shot_comm);
		free_vector(waveAC.stage_freq,1,NF);

	} /* End of FWI-workflow loop */
        
        /* memory deallocation */

        /* free memory for abort criterion */
        free_vector(L2_hist,1,ITERMAX*stagemax);
        free_vector(L2t,1,4);

        /* free memory for gradient */
        free_matrix(fwiAC.lam,1,NY,1,NX);
        free_matrix(fwiAC.grad,1,NY,1,NX);
        free_matrix(fwiAC.gradm,1,NY,1,NX);
   	free_matrix(fwiAC.Hgrad,1,NY,1,NX);
	free_matrix(fwiAC.vp_old,1,NY,1,NX);
	free_matrix(fwiAC.forwardr,1,NY,1,NX);
	free_matrix(fwiAC.forwardi,1,NY,1,NX);

	/* free memory for l-BFGS */
	if(GRAD_METHOD==2){
	  
	  free_vector(y_LBFGS,1,NLBFGS_vec*NLBFGS);
	  free_vector(s_LBFGS,1,NLBFGS_vec*NLBFGS);

	  free_vector(q_LBFGS,1,NLBFGS_vec);
	  free_vector(r_LBFGS,1,NLBFGS_vec);

	  free_vector(rho_LBFGS,1,NLBFGS);
	  free_vector(alpha_LBFGS,1,NLBFGS);
	  free_vector(beta_LBFGS,1,NLBFGS);
	  
	}

	/* deallocation of memory */
	free_matrix(matAC.vp,1,NY,1,NX);
	free_matrix(matAC.ivp2,1,NY,1,NX);
	free_matrix(matAC.k2,1,NY,1,NX);

	/* free memory for source positions */
	free_matrix(acq.srcpos,1,8,1,nshots);

	/* free memory for receiver positions */
	free_seis_AC(&waveAC,ntr); 

	if(READ_REC==0){
	    free_imatrix(acq.recpos,1,3,1,ntr);
	    free_vector(fwiAC.presr,1,ntr);
	    free_vector(fwiAC.presi,1,ntr);
	}
        	    
}
