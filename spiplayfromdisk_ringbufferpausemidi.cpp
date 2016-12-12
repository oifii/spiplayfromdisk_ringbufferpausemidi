////////////////////////////////////////////////////////////////
//nakedsoftware.org, spi@oifii.org or stephane.poirier@oifii.org
//
//
//2013april03, creation for playing a stereo wav file using portaudio
//             directly reading wav file from disk without loading it
//			   completely into memory at start.
//			   especially interesting when attempting to play very large
//			   wav file that do not fit as a whole into the host memory.
//			   initially derived from paex_record_file and modified so
//			   the wav file is playing reading a wav file format instead of
//			   the raw format that was used in paex_record_file.c
//			   also, support for asio and device selection from arguments.
//
//nakedsoftware.org, spi@oifii.org or stephane.poirier@oifii.org
////////////////////////////////////////////////////////////////
 
#include <stdio.h>
#include <stdlib.h>
#include "portaudio.h"
#include "pa_asio.h"
#include "pa_ringbuffer.h"
#include "pa_util.h"

#include <sndfile.h>
#include <sndfile.hh>
#include <assert.h>
#include <map>
#include <string>
using namespace std;

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#endif


 
/* #define SAMPLE_RATE  (17932) // Test failure to open with this value. */
#define FILE_NAME       "audio_data.raw"
#define SAMPLE_RATE  (44100)
#define FRAMES_PER_BUFFER (512)
#define NUM_SECONDS     (10)
#define NUM_CHANNELS    (2)
#define NUM_WRITES_PER_BUFFER   (4)
/* #define DITHER_FLAG     (paDitherOff) */
#define DITHER_FLAG     (0) /**/
 


/* Select sample format. */
#if 1
#define PA_SAMPLE_TYPE  paFloat32
typedef float SAMPLE;
#define SAMPLE_SILENCE  (0.0f)
#define PRINTF_S_FORMAT "%.8f"
#elif 1
#define PA_SAMPLE_TYPE  paInt16
typedef short SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#elif 0
#define PA_SAMPLE_TYPE  paInt8
typedef char SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#else
#define PA_SAMPLE_TYPE  paUInt8
typedef unsigned char SAMPLE;
#define SAMPLE_SILENCE  (128)
#define PRINTF_S_FORMAT "%d"
#endif
 


int Terminate();
string global_filename;
map<string,int> global_devicemap;
PaStreamParameters global_outputParameters;
PaError global_err;
string global_audiodevicename;
int global_outputAudioChannelSelectors[2];
PaAsioStreamInfo global_asioOutputInfo;

bool SelectAudioDevice()
{
	const PaDeviceInfo* deviceInfo;
    int numDevices = Pa_GetDeviceCount();
    for( int i=0; i<numDevices; i++ )
    {
        deviceInfo = Pa_GetDeviceInfo( i );
		string devicenamestring = deviceInfo->name;
		global_devicemap.insert(pair<string,int>(devicenamestring,i));
	}

	int deviceid = Pa_GetDefaultOutputDevice(); // default output device 
	map<string,int>::iterator it;
	it = global_devicemap.find(global_audiodevicename);
	if(it!=global_devicemap.end())
	{
		deviceid = (*it).second;
		printf("%s maps to %d\n", global_audiodevicename.c_str(), deviceid);
		deviceInfo = Pa_GetDeviceInfo(deviceid);
		//assert(inputAudioChannelSelectors[0]<deviceInfo->maxInputChannels);
		//assert(inputAudioChannelSelectors[1]<deviceInfo->maxInputChannels);
	}
	else
	{
		for(it=global_devicemap.begin(); it!=global_devicemap.end(); it++)
		{
			printf("%s maps to %d\n", (*it).first.c_str(), (*it).second);
		}
		//Pa_Terminate();
		//return -1;
		printf("error, audio device not found, will use default\n");
		deviceid = Pa_GetDefaultOutputDevice();
	}


	global_outputParameters.device = deviceid; 
	if (global_outputParameters.device == paNoDevice) 
	{
		fprintf(stderr,"Error: No default input device.\n");
		//goto error;
		Pa_Terminate();
		fprintf( stderr, "An error occured while using the portaudio stream\n" );
		fprintf( stderr, "Error number: %d\n", global_err );
		fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( global_err ) );
		return Terminate();
	}
	global_outputParameters.channelCount = 2;
	global_outputParameters.sampleFormat =  PA_SAMPLE_TYPE;
	global_outputParameters.suggestedLatency = Pa_GetDeviceInfo( global_outputParameters.device )->defaultLowOutputLatency;
	//inputParameters.hostApiSpecificStreamInfo = NULL;

	//Use an ASIO specific structure. WARNING - this is not portable. 
	//PaAsioStreamInfo asioInputInfo;
	global_asioOutputInfo.size = sizeof(PaAsioStreamInfo);
	global_asioOutputInfo.hostApiType = paASIO;
	global_asioOutputInfo.version = 1;
	global_asioOutputInfo.flags = paAsioUseChannelSelectors;
	global_asioOutputInfo.channelSelectors = global_outputAudioChannelSelectors;
	if(deviceid==Pa_GetDefaultOutputDevice())
	{
		global_outputParameters.hostApiSpecificStreamInfo = NULL;
	}
	else if(Pa_GetHostApiInfo(Pa_GetDeviceInfo(deviceid)->hostApi)->type == paASIO) 
	{
		global_outputParameters.hostApiSpecificStreamInfo = &global_asioOutputInfo;
	}
	else if(Pa_GetHostApiInfo(Pa_GetDeviceInfo(deviceid)->hostApi)->type == paWDMKS) 
	{
		global_outputParameters.hostApiSpecificStreamInfo = NULL;
	}
	else
	{
		//assert(false);
		global_outputParameters.hostApiSpecificStreamInfo = NULL;
	}
	return true;
}


 
typedef struct
{
    unsigned            frameIndex;
    int                 threadSyncFlag;
    SAMPLE             *ringBufferData;
    PaUtilRingBuffer    ringBuffer;
    FILE               *file;
    void               *threadHandle;
} paTestData;
 
bool AppendWavFile(const char* filename, const void* pVoid, long sizeelementinbytes, long count)
{
	assert(filename);
	assert(sizeelementinbytes==4);
    const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_16;  
	//const int format=SF_FORMAT_WAV | SF_FORMAT_FLOAT;  
    //const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_24;  
    //const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_32;  
	//SndfileHandle outfile(filename, SFM_WRITE, format, numChannels, SampleRate); 

	SndfileHandle outfile(filename, SFM_RDWR, format, NUM_CHANNELS, SAMPLE_RATE); 
	//SndfileHandle outfile(filename, SFM_RDWR, format, 1, SAMPLE_RATE); 
	outfile.seek(outfile.frames(), SEEK_SET);
	/*
	if(frameIndex==0)
	{
		outfile.write(pSamples, numSamples);  
	}
	else
	{
		assert(frameIndex<=totalFrames);
		outfile.write(pSamples, frameIndex*NUM_CHANNELS); 
	}
	*/
	outfile.write((const float*)pVoid, count); 
	return true;
}

// This routine is run in a separate thread to write data from the ring buffer into a wav file (during Recording)
static int threadFunctionWriteToWavFile(void* ptr)
{
    paTestData* pData = (paTestData*)ptr;
 
    // Mark thread started  
    pData->threadSyncFlag = 0;
 
    while (1)
    {
        ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferReadAvailable(&pData->ringBuffer);
        if ( (elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER) ||
             pData->threadSyncFlag )
        {
            void* ptr[2] = {0};
            ring_buffer_size_t sizes[2] = {0};
 
            /* By using PaUtil_GetRingBufferReadRegions, we can read directly from the ring buffer */
            ring_buffer_size_t elementsRead = PaUtil_GetRingBufferReadRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);
            if (elementsRead > 0)
            {
                int i;
                for (i = 0; i < 2 && ptr[i] != NULL; ++i)
                {
                    //fwrite(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
					AppendWavFile(global_filename.c_str(), ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i]);
                }
                PaUtil_AdvanceRingBufferReadIndex(&pData->ringBuffer, elementsRead);
            }
 
            if (pData->threadSyncFlag)
            {
                break;
            }
        }
 
        /* Sleep a little while... */
        Pa_Sleep(20);
 
    }
 
    pData->threadSyncFlag = 0;
    return 0;
}

/* This routine is run in a separate thread to write data from the ring buffer into a file (during Recording) */ 
static int threadFunctionWriteToRawFile(void* ptr)
{
    paTestData* pData = (paTestData*)ptr;
 
    /* Mark thread started */ 
    pData->threadSyncFlag = 0;
 
    while (1)
    {
        ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferReadAvailable(&pData->ringBuffer);
        if ( (elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER) ||
             pData->threadSyncFlag )
        {
            void* ptr[2] = {0};
            ring_buffer_size_t sizes[2] = {0};
 
            /* By using PaUtil_GetRingBufferReadRegions, we can read directly from the ring buffer */
            ring_buffer_size_t elementsRead = PaUtil_GetRingBufferReadRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);
            if (elementsRead > 0)
            {
                int i;
                for (i = 0; i < 2 && ptr[i] != NULL; ++i)
                {
                    fwrite(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
                }
                PaUtil_AdvanceRingBufferReadIndex(&pData->ringBuffer, elementsRead);
            }
 
            if (pData->threadSyncFlag)
            {
                break;
            }
        }
 
        /* Sleep a little while... */
        Pa_Sleep(20);
 
    }
 
    pData->threadSyncFlag = 0;
    return 0;
}
 

/* This routine is run in a separate thread to read data from file into the ring buffer (during Playback). When the file
   has reached EOF, a flag is set so that the play PA callback can return paComplete */ 
static int threadFunctionReadFromWavFile(void* ptr)
{
	//FILE* pDebugTxtFile = fopen("debug.txt","w");

    paTestData* pData = (paTestData*)ptr;

	assert(global_filename.c_str());
	assert(pData->ringBuffer.elementSizeBytes==4);
	
	
	const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_16;  
	//const int format=SF_FORMAT_WAV | SF_FORMAT_FLOAT;  
    //const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_24;  
    //const int format=SF_FORMAT_WAV | SF_FORMAT_PCM_32;  
	//SndfileHandle infile(global_filename.c_str(), SFM_READ, format, NUM_CHANNELS, SAMPLE_RATE); 
	SndfileHandle infile(global_filename.c_str(), SFM_READ); 
	/*
	fprintf(pDebugTxtFile, "Opened file %s\n",global_filename.c_str());
	fprintf(pDebugTxtFile, "  Sample rate is %d\n",infile.samplerate());
	fprintf(pDebugTxtFile, "  Channels is %d\n",infile.channels());
	fprintf(pDebugTxtFile, "  Error is %s\n",infile.strError());
	fprintf(pDebugTxtFile, "  frames is %d\n",infile.frames());
	fprintf(pDebugTxtFile, "\n");
	*/

	/*
	SF_INFO mySF_INFO;
	SNDFILE* pSNDFILE = sf_open(global_filename.c_str(), SFM_READ, &mySF_INFO);
	fprintf(pDebugTxtFile, "Opened file %s\n",global_filename.c_str());
	fprintf(pDebugTxtFile, "  Sample rate is %d\n", mySF_INFO.samplerate);
	fprintf(pDebugTxtFile, "  Channels is %d\n", mySF_INFO.channels);
	//fprintf(pDebugTxtFile, "  Error is %s\n", mySF_INFO.);
	fprintf(pDebugTxtFile, "  frames is %d\n", mySF_INFO.frames);
	fprintf(pDebugTxtFile, "\n");
	*/

    while (1)
    {
        ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferWriteAvailable(&pData->ringBuffer);
 
        if (elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER)
        {
            void* ptr[2] = {0};
            ring_buffer_size_t sizes[2] = {0};
 
            // By using PaUtil_GetRingBufferWriteRegions, we can write directly into the ring buffer 
            PaUtil_GetRingBufferWriteRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);
 
            //if (!feof(pData->file))
            if (1)
            {
                ring_buffer_size_t itemsReadFromFile = 0;
                int i;
                for (i = 0; i < 2 && ptr[i] != NULL; ++i)
                {
                    //itemsReadFromFile += (ring_buffer_size_t)fread(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
					long lcount = infile.read((float*)ptr[i], sizes[i]); 
					if(lcount==0)
					{
						//No more data to read
						pData->threadSyncFlag = 1;
						//break;
						goto out;
					}
					itemsReadFromFile += lcount;
					//fprintf(pDebugTxtFile, "itemsReadFromFile=%d\n",itemsReadFromFile);
                }
                PaUtil_AdvanceRingBufferWriteIndex(&pData->ringBuffer, itemsReadFromFile);
 
                //Mark thread started here, that way we "prime" the ring buffer before playback 
                pData->threadSyncFlag = 0;
            }
            /*
			else
            {
                //No more data to read
                pData->threadSyncFlag = 1;
                break;
            }
			*/
        }
 
        /* Sleep a little while... */
        Pa_Sleep(20);
    }
	out:
	//if(pDebugTxtFile) fclose(pDebugTxtFile);
    return 0;
}


/* This routine is run in a separate thread to read data from file into the ring buffer (during Playback). When the file
   has reached EOF, a flag is set so that the play PA callback can return paComplete */ 
static int threadFunctionReadFromRawFile(void* ptr)
{
    paTestData* pData = (paTestData*)ptr;

    while (1)
    {
        ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferWriteAvailable(&pData->ringBuffer);
 
        if (elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER)
        {
            void* ptr[2] = {0};
            ring_buffer_size_t sizes[2] = {0};
 
            /* By using PaUtil_GetRingBufferWriteRegions, we can write directly into the ring buffer */
            PaUtil_GetRingBufferWriteRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);
 
            if (!feof(pData->file))
            {
                ring_buffer_size_t itemsReadFromFile = 0;
                int i;
                for (i = 0; i < 2 && ptr[i] != NULL; ++i)
                {
                    itemsReadFromFile += (ring_buffer_size_t)fread(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
                }
                PaUtil_AdvanceRingBufferWriteIndex(&pData->ringBuffer, itemsReadFromFile);
 
                /* Mark thread started here, that way we "prime" the ring buffer before playback */
                pData->threadSyncFlag = 0;
            }
            else
            {
                /* No more data to read */
                pData->threadSyncFlag = 1;
                break;
            }
        }
 
        /* Sleep a little while... */
        Pa_Sleep(20);
    }
    return 0;
}
 

 
typedef int (*ThreadFunctionType)(void*);

/* Start up a new thread in the given function, at the moment only Windows, but should be very easy to extend
 
   to posix type OSs (Linux/Mac) */
 
static PaError startThread( paTestData* pData, ThreadFunctionType fn ) 
{
#ifdef _WIN32
    typedef unsigned (__stdcall* WinThreadFunctionType)(void*);
    pData->threadHandle = (void*)_beginthreadex(NULL, 0, (WinThreadFunctionType)fn, pData, CREATE_SUSPENDED, NULL);
    if (pData->threadHandle == NULL) return paUnanticipatedHostError;
 
    /* Set file thread to a little higher prio than normal */
    SetThreadPriority(pData->threadHandle, THREAD_PRIORITY_ABOVE_NORMAL);
 
    /* Start it up */
    pData->threadSyncFlag = 1;
    ResumeThread(pData->threadHandle);
#endif
 
    /* Wait for thread to startup */
    while (pData->threadSyncFlag) {
        Pa_Sleep(10);
    }
 
    return paNoError;
}
 

 
static int stopThread( paTestData* pData )
{
    pData->threadSyncFlag = 1;
    /* Wait for thread to stop */
    while (pData->threadSyncFlag) {
        Pa_Sleep(10);
    }
 
#ifdef _WIN32
    CloseHandle(pData->threadHandle);
    pData->threadHandle = 0;
#endif

    return paNoError;
}
 

 

 
/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int recordCallback( const void *inputBuffer, void *outputBuffer,
                           unsigned long framesPerBuffer,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void *userData )
{
    paTestData *data = (paTestData*)userData;
    ring_buffer_size_t elementsWriteable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
    ring_buffer_size_t elementsToWrite = min(elementsWriteable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
    const SAMPLE *rptr = (const SAMPLE*)inputBuffer;
 
    (void) outputBuffer; /* Prevent unused variable warnings. */
    (void) timeInfo;
    (void) statusFlags;
    (void) userData;
 
    data->frameIndex += PaUtil_WriteRingBuffer(&data->ringBuffer, rptr, elementsToWrite);
 
    return paContinue;
}
 

 
/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int playCallback( const void *inputBuffer, void *outputBuffer, 
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void *userData )
{
    paTestData *data = (paTestData*)userData;
    ring_buffer_size_t elementsToPlay = PaUtil_GetRingBufferReadAvailable(&data->ringBuffer);
    ring_buffer_size_t elementsToRead = min(elementsToPlay, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
    SAMPLE* wptr = (SAMPLE*)outputBuffer;
 
    (void) inputBuffer; /* Prevent unused variable warnings. */
    (void) timeInfo;
    (void) statusFlags;
    (void) userData;
 
    data->frameIndex += PaUtil_ReadRingBuffer(&data->ringBuffer, wptr, elementsToRead);
	/*
	FILE* pDebugTxtFile = fopen("debug_playcallback.txt","a+");
	if(pDebugTxtFile) 
	{
		fprintf(pDebugTxtFile, "%d\n", data->frameIndex);
		fclose(pDebugTxtFile);
	}
	*/
    return data->threadSyncFlag ? paComplete : paContinue;
}
 

 
static unsigned NextPowerOf2(unsigned val)
{
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}
 


 
/*******************************************************************/
int main(int argc, char *argv[]);
int main(int argc, char *argv[])
{
	///////////////////
	//read in arguments
	///////////////////
	global_filename = "testrecording.wav"; //usage: spiplayfromdisk testrecording.wav -1 "E-MU ASIO" 0 1
	float fSecondsPlay = NUM_SECONDS; 
	if(argc>1)
	{
		//first argument is the filename
		global_filename = argv[1];
	}
	if(argc>2)
	{
		//second argument is the time it will play
		fSecondsPlay = atof(argv[2]); //if number of seconds is negative, will play the whole file until endoffile is reached, otherwize will truncate playback
	}
	//use audio_spi\spidevicesselect.exe to find the name of your devices, only exact name will be matched (name as detected by spidevicesselect.exe)  
	global_audiodevicename="E-MU ASIO"; //"Wave (2- E-MU E-DSP Audio Proce"
	//global_audiodevicename="Wave (2- E-MU E-DSP Audio Proce"; //"E-MU ASIO"
	if(argc>3)
	{
		global_audiodevicename = argv[3]; //for spi, device name could be "E-MU ASIO", "Speakers (2- E-MU E-DSP Audio Processor (WDM))", etc.
	}
	//global_outputAudioChannelSelectors[0] = 0; // on emu patchmix ASIO device channel 1 (left)
	//global_outputAudioChannelSelectors[1] = 1; // on emu patchmix ASIO device channel 2 (right)
	global_outputAudioChannelSelectors[0] = 2; // on emu patchmix ASIO device channel 3 (left)
	global_outputAudioChannelSelectors[1] = 3; // on emu patchmix ASIO device channel 4 (right)
	//global_outputAudioChannelSelectors[0] = 10; // on emu patchmix ASIO device channel 11 (left)
	//global_outputAudioChannelSelectors[1] = 11; // on emu patchmix ASIO device channel 12 (right)
	if(argc>4)
	{
		global_outputAudioChannelSelectors[0]=atoi(argv[4]); //0 for first asio channel (left) or 2, 4, 6, etc.
	}
	if(argc>5)
	{
		global_outputAudioChannelSelectors[1]=atoi(argv[5]); //1 for second asio channel (right) or 3, 5, 7, etc.
	}

    //PaStreamParameters  inputParameters;
    //PaStreamParameters  outputParameters;
    PaStream*           stream;
    PaError             err = paNoError;
    paTestData          data = {0};
    unsigned            delayCntr;
    unsigned            numSamples;
    unsigned            numBytes;
 
    printf("patest_record.c\n"); fflush(stdout);
 
    // We set the ring buffer size to about 500 ms
    numSamples = NextPowerOf2((unsigned)(SAMPLE_RATE * 0.5 * NUM_CHANNELS));
    numBytes = numSamples * sizeof(SAMPLE);
    data.ringBufferData = (SAMPLE *) PaUtil_AllocateMemory( numBytes );
    if( data.ringBufferData == NULL )
    {
        printf("Could not allocate ring buffer data.\n");
        goto done;
    }
 
    if (PaUtil_InitializeRingBuffer(&data.ringBuffer, sizeof(SAMPLE), numSamples, data.ringBufferData) < 0)
    {
        printf("Failed to initialize ring buffer. Size is not power of 2 ??\n");
        goto done;
    }
 
    err = Pa_Initialize();
    if( err != paNoError ) goto done;
 
	/*
	if(0)
	{
		global_inputParameters.device = Pa_GetDefaultInputDevice(); //default input device 
		if (global_inputParameters.device == paNoDevice) 
		{
			fprintf(stderr,"Error: No default input device.\n");
			goto done;
		}
		global_inputParameters.channelCount = 2;                    // stereo input
		global_inputParameters.sampleFormat = PA_SAMPLE_TYPE;
		global_inputParameters.suggestedLatency = Pa_GetDeviceInfo( global_inputParameters.device )->defaultLowInputLatency;
		global_inputParameters.hostApiSpecificStreamInfo = NULL;
	}
	else
	{
		////////////////////////
		//audio device selection
		////////////////////////
		SelectAudioDevice();
	}
	*/

	/*
    // Record some audio. --------------------------------------------
    err = Pa_OpenStream(
              &stream,
              &global_inputParameters,
              NULL,                  // &outputParameters, 
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff,      // we won't output out of range samples so don't bother clipping them 
              recordCallback,
              &data );
 
    if( err != paNoError ) goto done;
 
	if(0)
	{
		// Open the raw audio 'cache' file...
		data.file = fopen(FILE_NAME, "wb");
		if (data.file == 0) goto done;
	}
	else
	{
		// Open the wav audio 'cache' file...
		data.file = fopen(global_filename.c_str(), "wb");
		if (data.file == 0) goto done;
	}

    // Start the file writing thread 
    if(0)
	{
		err = startThread(&data, threadFunctionWriteToRawFile);
	}
	else
	{
		err = startThread(&data, threadFunctionWriteToWavFile);
	}
	if( err != paNoError ) goto done;
 
    err = Pa_StartStream( stream );
    if( err != paNoError ) goto done;
    printf("\n=== Now recording to '" FILE_NAME "' for %d seconds!! Please speak into the microphone. ===\n", NUM_SECONDS); fflush(stdout);
 
    // Note that the RECORDING part is limited with TIME, not size of the file and/or buffer, so you can
    // increase NUM_SECONDS until you run out of disk 
    delayCntr = 0;
    while( delayCntr++ < NUM_SECONDS )
    {
        printf("index = %d\n", data.frameIndex ); fflush(stdout);
        Pa_Sleep(1000);
    }
    if( err < 0 ) goto done;
 
    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto done;
 
    // Stop the thread 
    err = stopThread(&data);
    if( err != paNoError ) goto done;
 
    // Close file 
    fclose(data.file);
    data.file = 0;
	*/


	
    // Playback recorded data.  -------------------------------------------- 
    data.frameIndex = 0;
 
	if(0)
	{
		global_outputParameters.device = Pa_GetDefaultOutputDevice(); // default output device 
		if (global_outputParameters.device == paNoDevice) {
			fprintf(stderr,"Error: No default output device.\n");
			goto done;
		}
		global_outputParameters.channelCount = 2;                     // stereo output 
		global_outputParameters.sampleFormat =  PA_SAMPLE_TYPE;
		global_outputParameters.suggestedLatency = Pa_GetDeviceInfo( global_outputParameters.device )->defaultLowOutputLatency;
		global_outputParameters.hostApiSpecificStreamInfo = NULL;
	}
	else
	{
		////////////////////////
		//audio device selection
		////////////////////////
		SelectAudioDevice();
	}

    //printf("\n=== Now playing back from file '" FILE_NAME "' until end-of-file is reached ===\n"); fflush(stdout);
    printf("\n=== Now playing back from file  %s until end-of-file is reached ===\n", global_filename.c_str()); fflush(stdout);
 
    err = Pa_OpenStream(
              &stream,
              NULL, // no input 
              &global_outputParameters,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff,      // we won't output out of range samples so don't bother clipping them 
              playCallback,
              &data );
 
    if( err != paNoError ) goto done;
 
    if( stream )
    {
		/*
        // Open file again for reading 
        data.file = fopen(FILE_NAME, "rb");
        if (data.file != 0)
		*/
        // Open file again for reading 
		data.file = fopen(global_filename.c_str(), "r");
		if (data.file == 0) 
		{
			printf("Error, can't open file %s.\n", global_filename.c_str()); fflush(stdout);
			goto done;
		}
		fclose(data.file);
		if(1)
        {
            // Start the file reading thread 
			if(0)
			{
				err = startThread(&data, threadFunctionReadFromRawFile);
			}
			else
			{
				err = startThread(&data, threadFunctionReadFromWavFile);
			}
			if( err != paNoError ) goto done;
            err = Pa_StartStream( stream );
            if( err != paNoError ) goto done;
            printf("Waiting for playback to finish.\n"); fflush(stdout);
 
            // The playback will end when EOF is reached 
            while( ( err = Pa_IsStreamActive( stream ) ) == 1 ) 
			{
                printf("index = %d\n", data.frameIndex ); fflush(stdout);
                Pa_Sleep(1000);
            }
            if( err < 0 ) goto done;
        }
 
        err = Pa_CloseStream( stream );
        if( err != paNoError ) goto done;
        /*
		fclose(data.file);
		*/

        printf("Done.\n"); fflush(stdout);
    }
	
    printf("Done.\n"); fflush(stdout);

 
done:
 
    Pa_Terminate();
    if( data.ringBufferData )       /* Sure it is NULL or valid. */
        PaUtil_FreeMemory( data.ringBufferData );
    if( err != paNoError )
    {
        fprintf( stderr, "An error occured while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
        err = 1;          /* Always return 0 or 1, but no other return codes. */
    }
 
    return err;
}
 
int Terminate()
{

	printf("Exiting!\n"); fflush(stdout);
	return 0;
}
 
