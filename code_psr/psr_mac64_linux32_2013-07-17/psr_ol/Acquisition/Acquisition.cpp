#include "Includes.h"
Acquisition::Acquisition()
{
	//Empty constructor, do nothing of value
}

Acquisition::Acquisition(Acq_Options _Opt)
{
	int lcv, index, dopp_tick;
	double phase, phase_step, f_doppler;

	/* private control variables */
	/*-----------------------------------------------------------------------------*/
	Opt = _Opt;
	Opt.tticks = Opt.cticks*Opt.iticks;
	/*-----------------------------------------------------------------------------*/

	/* Allocate arrays */
	/*-----------------------------------------------------------------------------*/
	data_buff = new DATA_CPX [Opt.resamples_per_ms*Opt.tticks];

	power = new int[Opt.resamples_per_ms];

	re_cb = new int[Opt.resamples_per_ms];

	im_cb = new int[Opt.resamples_per_ms];

	sines = new DATA_CPX *[Opt.doppler_bins*2+1];
	for(lcv = 0; lcv < Opt.doppler_bins*2+1; lcv++)
		sines[lcv] = new DATA_CPX[Opt.resamples_per_ms];

	FFT_S2 = new DATA_CPX[Opt.resamples_per_ms];

	for(lcv = 0; lcv < NUM_CODES; lcv++)
		FFT_PRN[lcv] = new DATA_CPX[Opt.resamples_per_ms];
	/*-----------------------------------------------------------------------------*/

	/* setup the kiss_fft profiles */
	pFFT = new FFT(Opt.resamples_per_ms);

	/* FFT of PRN Codes, Step 1 of FFT Correlation */
	/*-----------------------------------------------------------------------------*/
	for(lcv = 0; lcv < NUM_CODES; lcv++)
	{
		/* Sample for PRN FFT at correct rate */
		for(int samp_tick = 0; samp_tick < Opt.resamples_per_ms; ++samp_tick)
		{
			index = (unsigned) floor((float)samp_tick*(CODE_RATE/Opt.resample_rate));
			FFT_PRN[lcv][samp_tick].r = 32767*PRN_Codes[lcv][index];
			FFT_PRN[lcv][samp_tick].i = 32767*PRN_Codes[lcv][index];
		}
		/* Now perform the FFT */
		pFFT->doFFTdf((int*)FFT_PRN[lcv], false);
	}
	/*-----------------------------------------------------------------------------*/



	/* Carrier wipeoff for FFT Correlations */
	/*-----------------------------------------------------------------------------*/
	///* Sample sine/cosine at each doppler bin */
	// Use a negative frequency sinusoid to mix the signal to baseband.
	for(dopp_tick = -Opt.doppler_bins; dopp_tick <= Opt.doppler_bins; dopp_tick++)
	{
		f_doppler = (double)ZERO_DOPPLER_RATE + (double)dopp_tick*(double)Opt.doppler_bin_width;	//generate f_carrier
		phase_step = TWO_PI*f_doppler/Opt.resample_rate;
		phase = 0;
		for(lcv = 0; lcv < Opt.resamples_per_ms; lcv++)
		{
			sines[dopp_tick + Opt.doppler_bins][lcv].r = (short)(2048*cos(phase));
			sines[dopp_tick + Opt.doppler_bins][lcv].i = (short)(-2048*sin(phase));
			phase += phase_step;
		}
	}
	/*-----------------------------------------------------------------------------*/




};




Acquisition::~Acquisition()
{

	delete pFFT;

	int lcv;

	for(lcv = 0; lcv < NUM_CODES; ++lcv)
		delete[] FFT_PRN[lcv];

	for(lcv = 0; lcv < Opt.doppler_bins*2+1; lcv++)
		delete[] sines[lcv];

	delete[] sines;
	delete[] FFT_S2;
	delete[] power;
	delete[] re_cb;
	delete[] im_cb;

}



void Acquisition::Cold_Acquire(void *_data, Acq_Result *_results)
{

	int resamples_per_ms, doppler_bins, doppler_bin_width, tticks, iticks, cticks;
	int dopp_tick, sv_tick, c_tick, i_tick, ms_tick; //loops
	int lcv, index;

	DATA_CPX *data;
	DATA_CPX *sine;
	DATA_CPX *re_data;

	FILE *acq;

	int curr_max;
	int dopp_bin;
	float code_phase;
	float psi;	//phase fix	
	DATA_CPX spsi;
	float f_carrier;
	char header[1024];
	int num;


	if(Opt.log)
		acq = fopen(Opt.filename,"wb");

	//Prevent unecesarry pointer dereferencing
	/*-----------------------------------------------------------------------------*/
	doppler_bin_width = Opt.doppler_bin_width;
	resamples_per_ms = Opt.resamples_per_ms;
	doppler_bins = Opt.doppler_bins;
	tticks = Opt.tticks;
	iticks = Opt.iticks;
	cticks = Opt.cticks;
	/*-----------------------------------------------------------------------------*/
	//Insert the header
	/*-----------------------------------------------------------------------------*/
	if(Opt.log)
	{
		memset(header,0x0,sizeof(header));
		num = 0;
		num += sprintf(&header[num],"ACQ_DOPPLER_BINS:%d\n",Opt.doppler_bins);
		num += sprintf(&header[num],"ACQ_DOPPLER_BIN_WIDTH:%d\n",Opt.doppler_bin_width);
		num += sprintf(&header[num],"ACQ_TICKS:%d\n",Opt.tticks);
		num += sprintf(&header[num],"ACQ_SAMPLES_PER_MS:%d\n",Opt.resamples_per_ms);
		fwrite(&header[0],sizeof(char),1024,acq);
	}
	/*-----------------------------------------------------------------------------*/


	//Resample the data
	/*-----------------------------------------------------------------------------*/
	data = (DATA_CPX *)_data;
	for(lcv = 0; lcv < tticks*resamples_per_ms; lcv++)
	{
		index = (int)floor((float)lcv*Opt.sample_rate/Opt.resample_rate);
		data_buff[lcv].r = data[index].r;
		data_buff[lcv].i = data[index].i;
	}
	/*-----------------------------------------------------------------------------*/
	/*-----------------------------------------------------------------------------*/
	float MEAN[doppler_bins*2+1];
	float mean;

	for(sv_tick = 0; sv_tick < NUM_CODES; sv_tick++)
	{

		curr_max = 0;

		for(dopp_tick = 0; dopp_tick < (doppler_bins*2+1); dopp_tick++)
		{

			//Clear integrations
			memset(&power[0],0x0,sizeof(int)*resamples_per_ms);

			//Doppler frequency
			f_carrier = ((dopp_tick - doppler_bins) * doppler_bin_width) + ZERO_DOPPLER_RATE;

			//Carrier wipeoff vectors
			sine = sines[dopp_tick];

			ms_tick = 0;


			for(i_tick = 0; i_tick < iticks; i_tick++) //Incoherent loop
			{

				memset(&re_cb[0],0x0,sizeof(int)*resamples_per_ms);
				memset(&im_cb[0],0x0,sizeof(int)*resamples_per_ms);

				for(c_tick = 0; c_tick < cticks; c_tick++) //Coherent loop
				{

					re_data = &data_buff[ms_tick * resamples_per_ms];

					/* Wipe carrier off of sampled data */
					sse_cmulsc(re_data, sine, FFT_S2, resamples_per_ms,0);

					/* Perform FFT */
					pFFT->doFFTdf((int*)FFT_S2, false);

					/* Multiply conjugate of step2 by fft of prn code */
					sse_conj(FFT_S2, resamples_per_ms);
					sse_cmuls(FFT_S2, FFT_PRN[sv_tick], resamples_per_ms,0);

					/* Get back into time domain */
					pFFT->doiFFT((int*)FFT_S2, false);

					/* Rotate the correlations by this angle */
					if(Opt.cticks > 1)
					{
						psi = PI*resamples_per_ms*f_carrier*c_tick/resamples_per_ms;
						spsi.r = (short)32767*cos(psi);
						spsi.i = (short)32767*sin(psi);

						x86_crot(FFT_S2, &spsi, resamples_per_ms);
					}


					/* Add coherently */
					for(lcv = 0; lcv < resamples_per_ms; lcv++)
					{
						re_cb[lcv] += FFT_S2[lcv].r;
						im_cb[lcv] += FFT_S2[lcv].i;
					}

					ms_tick++;


				} //end c_tick, coherent integration
					
				/* Add incoherently */
				for(lcv = 0; lcv < resamples_per_ms; lcv++)
					power[lcv] += re_cb[lcv]*re_cb[lcv] + im_cb[lcv]*im_cb[lcv];

			}//end i_tick, incoherent integration


			MEAN[dopp_tick]=0;
			for(lcv = 0; lcv < resamples_per_ms; lcv++)
			{
				if(curr_max < power[lcv])
				{
					curr_max = power[lcv];
					dopp_bin = dopp_tick;
					code_phase = static_cast<float>(lcv)*static_cast<float>(CODE_CHIPS)/static_cast<float>(resamples_per_ms);
				}
				MEAN[dopp_tick]+= static_cast<float>(power[lcv])/static_cast<float>(resamples_per_ms);
			}

			/* write it out for now */
			if(Opt.log)
				fwrite(&power[0],sizeof(int),resamples_per_ms,acq);
			
		} // end doppler spread 

		mean=0;

		for(lcv = 0; lcv < doppler_bins*2+1; lcv++)
		{
			mean += MEAN[lcv]/static_cast<float>(doppler_bins*2+1);
		}


		// do the results
		_results[sv_tick].SV = sv_tick;
		_results[sv_tick].dopp_bin = dopp_bin;
		_results[sv_tick].doppler = (dopp_bin-doppler_bins)*doppler_bin_width;
		_results[sv_tick].code_phase = code_phase;
		_results[sv_tick].CorrMax = curr_max;
		_results[sv_tick].Mean = mean;
		_results[sv_tick].SNR = 10*log10((double)(curr_max));

		//std::cout << "SNRs\n";
		//std::cout << "SV " <<_results[sv_tick].SV+1<<", SNR = "<< _results[sv_tick].SNR <<"\n";
//		printf("SV %2d, \t SNR = %f\n", _results[sv_tick].SV+1,	_results[sv_tick].SNR);

	} //end SV_tick


	if(Opt.log && acq != NULL) {
		fclose(acq);
		acq = NULL;
	}

}
