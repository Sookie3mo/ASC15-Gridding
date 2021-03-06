/// @copyright (c) 2007 CSIRO
/// Australia Telescope National Facility (ATNF)
/// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
/// PO Box 76, Epping NSW 1710, Australia
///
/// This file is part of the ASKAP software distribution.
///
/// The ASKAP software distribution is free software: you can redistribute it
/// and/or modify it under the terms of the GNU General Public License as
/// published by the Free Software Foundation; either version 2 of the License,
/// or (at your option) any later version.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU General Public License for more details.
///
/// You should have received a copy of the GNU General Public License
/// along with this program; if not, write to the Free Software
/// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
///
/// This program was modified so as to use it in the contest.
/// The last modification was on January 12, 2015.
///

// Include own header file first
#include "Benchmark.h"

// System includes
#include <iostream>
#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>
#include <omp.h>

Benchmark::Benchmark()
        : next(1)
{
}

// Return a pseudo-random integer in the range 0..2147483647
// Based on an algorithm in Kernighan & Ritchie, "The C Programming Language"
int Benchmark::randomInt()
{
    const unsigned int maxint = std::numeric_limits<int>::max();
    next = next * 1103515245 + 12345;
    return ((unsigned int)(next / 65536) % maxint);
}

void Benchmark::init()
{
    // Initialize the data to be gridded
    u.resize(nSamples);
    v.resize(nSamples);
    w.resize(nSamples);
    samples.resize(nSamples*nChan);
    outdata.resize(nSamples*nChan);


    Coord rd;
    FILE * fp;
    if( (fp=fopen("randnum.dat","rb"))==NULL )
    {
      printf("cannot open file\n");
      return;
    }
    
    for (int i = 0; i < nSamples; i++) {
        if(fread(&rd,sizeof(Coord),1,fp)!=1){printf("Rand number read error!\n");}
        u[i] = baseline * rd - baseline / 2;
        if(fread(&rd,sizeof(Coord),1,fp)!=1){printf("Rand number read error!\n");}
        v[i] = baseline * rd - baseline / 2;
        if(fread(&rd,sizeof(Coord),1,fp)!=1){printf("Rand number read error!\n");}
        w[i] = baseline * rd - baseline / 2;

        for (int chan = 0; chan < nChan; chan++) {
            if(fread(&rd,sizeof(Coord),1,fp)!=1){printf("Rand number read error!\n");}
            samples[i*nChan+chan].data = rd;
            outdata[i*nChan+chan] = 0.0;
        }
    }
    fclose(fp);

    grid.resize(gSize*gSize);
    grid.assign(grid.size(), Value(0.0));

    // Measure frequency in inverse wavelengths
    std::vector<Coord> freq(nChan);

    for (int i = 0; i < nChan; i++) {
        freq[i] = (1.4e9 - 2.0e5 * Coord(i) / Coord(nChan)) / 2.998e8;
    }

    // Initialize convolution function and offsets
    initC(freq, cellSize, wSize, m_support, overSample, wCellSize, C);
    initCOffset(u, v, w, freq, cellSize, wCellSize, wSize, gSize,
                m_support, overSample);
}

void Benchmark::runGrid()
{
    gridKernel(m_support, C, grid, gSize);
}

/////////////////////////////////////////////////////////////////////////////////
// The next function is the kernel of the gridding.
// The data are presented as a vector. Offsets for the convolution function
// and for the grid location are precalculated so that the kernel does
// not need to know anything about world coordinates or the shape of
// the convolution function. The ordering of cOffset and iu, iv is
// random.
//
// Perform gridding
//
// data - values to be gridded in a 1D vector
// support - Total width of convolution function=2*support+1
// C - convolution function shape: (2*support+1, 2*support+1, *)
// cOffset - offset into convolution function per data point
// iu, iv - integer locations of grid points
// grid - Output grid: shape (gSize, *)
// gSize - size of one axis of grid
//
typedef unsigned long ul;

//const int MAX_MIC = 1;
//const double DIVISION[NUM_OF_CARDS+1] = {0.00,0.50,1.00};

void Benchmark::gridKernel(const int support,
                           const std::vector<Value>& C,
                           std::vector<Value>& grid, const int gSize)
{
	int samplessize = (int)samples.size();

	int MAX_MIC = _Offload_number_of_devices();
	int NUM_OF_CARDS = MAX_MIC + 1;
	double PICESE = 1.0f/NUM_OF_CARDS;
	double DIVISION[NUM_OF_CARDS+1];
	if (NUM_OF_CARDS == 2){
		DIVISION[0] = 0.0;
		DIVISION[1] = 0.5;
		DIVISION[2] = 1.0;
	}
	else if (NUM_OF_CARDS == 3){
		DIVISION[0] = 0.00;
		DIVISION[1] = 0.31;
		DIVISION[2] = 0.60;
		DIVISION[3] = 1.00;
	}
	else{
		for (int i=0;i<NUM_OF_CARDS;i++)
			DIVISION[i] = i*PICESE;
		DIVISION[NUM_OF_CARDS] = 1;
	}


    const int sSize = 2 * support + 1;


	Coord* gptr = &grid[0].real();
	Coord* cptr = const_cast<Coord*>(&C[0].real());
	char* sptr = (char*)&samples[0].data.real();
	int gridsize = grid.size()*2;
	int Csize = C.size()*2;
	int counter = MAX_MIC;

	Coord *tmp = (Coord*)_mm_malloc(sizeof(Coord)*gridsize*MAX_MIC,64);
	/*
	double pieces = (double)samplessize / NUM_OF_CARDS;
	
	for (int imic = 0;imic < NUM_OF_CARDS ; ++imic){
		int mindind = int(imic * pieces);
		int maxdind;
		if (imic == NUM_OF_CARDS)
			maxdind = samplessize;
		else
			maxdind = int((imic+1) * pieces);
			*/
	

	for (int imic = 0;imic < NUM_OF_CARDS ; ++imic){
		int mindind = int(DIVISION[imic] * samplessize);
		int maxdind = int(DIVISION[imic+1] * samplessize);

		Coord* itmp = tmp + gridsize*imic;

		#pragma offload target(mic:imic) if(imic<MAX_MIC)\
			in(gridsize,Csize,samplessize,sSize,support,gSize,mindind,maxdind)\
  	 		out(gptr:length(gridsize) into(itmp))\
	   		in(cptr:length(Csize) align(8)) in(sptr[mindind*sizeof(Sample):(maxdind-mindind)*sizeof(Sample)]:align(8)) signal(gptr)
		{
		for (int dind = mindind; dind < maxdind; ++dind) {
			ul offset = (ul)sptr + sizeof(Sample)*(ul)(dind);
			Coord dreal = *(Coord*)offset;
			Coord dimag = *(Coord*)(offset+8); 
			int iu = *(int*)(offset+16);
			int iv = *(int*)(offset+20);
			int cOffset = *(int*)(offset+24);
			
			int _gind = iu + gSize * iv - support;
			int _cind = cOffset;
			
			#pragma omp parallel for
			//for (int suppvsuppu = 0; suppvsuppu < sSize*sSize; suppvsuppu++) {
			for (int suppv = 0; suppv < sSize; suppv++) {
				/*
				int suppv = suppvsuppu/(sSize);
				int suppu = suppvsuppu%(sSize);
				*/
				int gind = ((_gind + gSize * suppv)*2);
				int cind = ((_cind + sSize * suppv)*2);

				
				#pragma vector aligned
				#pragma ivdep
				for (int suppu=0; suppu < sSize; suppu++){
					int ig = gind + (suppu<<1);
					int ic = cind + (suppu<<1);
					gptr[ig] += dreal*cptr[ic]-dimag*cptr[ic+1];
					gptr[ig+1] += dreal*cptr[ic+1]+dimag*cptr[ic];
				}
			}
		}
		}
	}

	for (int imic=0;imic<MAX_MIC;++imic){
		#pragma offload_wait target(mic:imic) wait(gptr)

		#pragma omp parallel for
		#pragma ivdep
		for (int i=0;i<gridsize;i++){
			gptr[i] += tmp[i+imic*gridsize];
		}
	}

	//#pragma offload target(mic:0) inout(gptr:length(gridsize)) in(tmp:length(gridsize*MAX_MIC))
	_mm_free(tmp);
}

/////////////////////////////////////////////////////////////////////////////////
// Initialize W project convolution function
// - This is application specific and should not need any changes.
//
// freq - temporal frequency (inverse wavelengths)
// cellSize - size of one grid cell in wavelengths
// wSize - Size of lookup table in w
// support - Total width of convolution function=2*support+1
// wCellSize - size of one w grid cell in wavelengths
void Benchmark::initC(const std::vector<Coord>& freq,
                      const Coord cellSize, const int wSize,
                      int& support, int& overSample,
                      Coord& wCellSize, std::vector<Value>& C)
{
    std::cout << "Initializing W projection convolution function" << std::endl;
    support = static_cast<int>(1.5 * sqrt(std::abs(baseline) * static_cast<Coord>(cellSize)
                                          * freq[0]) / cellSize);

    overSample = 8;
    std::cout << "Support = " << support << " pixels" << std::endl;
    wCellSize = 2 * baseline * freq[0] / wSize;
    std::cout << "W cellsize = " << wCellSize << " wavelengths" << std::endl;

    // Convolution function. This should be the convolution of the
    // w projection kernel (the Fresnel term) with the convolution
    // function used in the standard case. The latter is needed to
    // suppress aliasing. In practice, we calculate entire function
    // by Fourier transformation. Here we take an approximation that
    // is good enough.
    const int sSize = 2 * support + 1;

    const int cCenter = (sSize - 1) / 2;

    C.resize(sSize*sSize*overSample*overSample*wSize);
    std::cout << "Size of convolution function = " << sSize*sSize*overSample
              *overSample*wSize*sizeof(Value) / (1024*1024) << " MB" << std::endl;
    std::cout << "Shape of convolution function = [" << sSize << ", " << sSize << ", "
                  << overSample << ", " << overSample << ", " << wSize << "]" << std::endl;

    for (int k = 0; k < wSize; k++) {
        double w = double(k - wSize / 2);
        double fScale = sqrt(std::abs(w) * wCellSize * freq[0]) / cellSize;

        for (int osj = 0; osj < overSample; osj++) {
            for (int osi = 0; osi < overSample; osi++) {
                for (int j = 0; j < sSize; j++) {
                    double j2 = std::pow((double(j - cCenter) + double(osj) / double(overSample)), 2);

                    for (int i = 0; i < sSize; i++) {
                        double r2 = j2 + std::pow((double(i - cCenter) + double(osi) / double(overSample)), 2);
                        long int cind = i + sSize * (j + sSize * (osi + overSample * (osj + overSample * k)));

                        if (w != 0.0) {
                            C[cind] = static_cast<Value>(std::cos(r2 / (w * fScale)));
                        } else {
                            C[cind] = static_cast<Value>(std::exp(-r2));
                        }
                    }
                }
            }
        }
    }

    // Now normalise the convolution function
    Coord sumC = 0.0;

    for (int i = 0; i < sSize*sSize*overSample*overSample*wSize; i++) {
        sumC += std::abs(C[i]);
    }

    for (int i = 0; i < sSize*sSize*overSample*overSample*wSize; i++) {
        C[i] *= Value(wSize * overSample * overSample / sumC);
    }
}

// Initialize Lookup function
// - This is application specific and should not need any changes.
//
// freq - temporal frequency (inverse wavelengths)
// cellSize - size of one grid cell in wavelengths
// gSize - size of grid in pixels (per axis)
// support - Total width of convolution function=2*support+1
// wCellSize - size of one w grid cell in wavelengths
// wSize - Size of lookup table in w
void Benchmark::initCOffset(const std::vector<Coord>& u, const std::vector<Coord>& v,
                            const std::vector<Coord>& w, const std::vector<Coord>& freq,
                            const Coord cellSize, const Coord wCellSize,
                            const int wSize, const int gSize, const int support,
                            const int overSample)
{
    const int nSamples = u.size();
    const int nChan = freq.size();

    const int sSize = 2 * support + 1;

    // Now calculate the offset for each visibility point
    for (int i = 0; i < nSamples; i++) {
        for (int chan = 0; chan < nChan; chan++) {

            int dind = i * nChan + chan;

            Coord uScaled = freq[chan] * u[i] / cellSize;
            samples[dind].iu = int(uScaled);

            if (uScaled < Coord(samples[dind].iu)) {
                samples[dind].iu -= 1;
            }

            int fracu = int(overSample * (uScaled - Coord(samples[dind].iu)));
            samples[dind].iu += gSize / 2;

            Coord vScaled = freq[chan] * v[i] / cellSize;
            samples[dind].iv = int(vScaled);

            if (vScaled < Coord(samples[dind].iv)) {
                samples[dind].iv -= 1;
            }

            int fracv = int(overSample * (vScaled - Coord(samples[dind].iv)));
            samples[dind].iv += gSize / 2;

            // The beginning of the convolution function for this point
            Coord wScaled = freq[chan] * w[i] / wCellSize;
            int woff = wSize / 2 + int(wScaled);
            samples[dind].cOffset = sSize * sSize * (fracu + overSample * (fracv + overSample * woff));
        }
    }
}

void Benchmark::printGrid()
{
  FILE * fp;
  if( (fp=fopen("grid.dat","wb"))==NULL )
  {
    printf("cannot open file\n");
    return;
  }  

  unsigned ij;
  for (int i = 0; i < gSize; i++)
  {
    for (int j = 0; j < gSize; j++)
    {
      ij=j+i*gSize;
      if(fwrite(&grid[ij],sizeof(Value),1,fp)!=1)
        printf("File write error!\n"); 

    }
  }
  
  fclose(fp);
}

int Benchmark::getSupport()
{
    return m_support;
};
