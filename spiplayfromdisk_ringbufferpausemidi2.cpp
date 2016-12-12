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
//2014may10, added a pause/unpause playback feature attached to key 'P'
//
//2014may10, added a midi controller value detection, values 0-63 play
//           values 64-127 pause playback
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

#include <conio.h> //for _kbhit()

#include "porttime.h"
#include "portmidi.h"

 
/* #define SAMPLE_RATE  (17932) // Test failure to open with this value. */
#define FILE_NAME       "audio_data.raw"
#define SAMPLE_RATE  (44100)
#define FRAMES_PER_BUFFER (512)
#define NUM_SECONDS     (-1)
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
 
#define STRING_MAX 80

#define MIDI_CODE_MASK  0xf0
#define MIDI_CHN_MASK   0x0f
//#define MIDI_REALTIME   0xf8
//  #define MIDI_CHAN_MODE  0xfa 
#define MIDI_OFF_NOTE   0x80
#define MIDI_ON_NOTE    0x90
#define MIDI_POLY_TOUCH 0xa0
#define MIDI_CTRL       0xb0
#define MIDI_CH_PROGRAM 0xc0
#define MIDI_TOUCH      0xd0
#define MIDI_BEND       0xe0

#define MIDI_SYSEX      0xf0
#define MIDI_Q_FRAME	0xf1
#define MIDI_SONG_POINTER 0xf2
#define MIDI_SONG_SELECT 0xf3
#define MIDI_TUNE_REQ	0xf6
#define MIDI_EOX        0xf7
#define MIDI_TIME_CLOCK 0xf8
#define MIDI_START      0xfa
#define MIDI_CONTINUE	0xfb
#define MIDI_STOP       0xfc
#define MIDI_ACTIVE_SENSING 0xfe
#define MIDI_SYS_RESET  0xff

#define MIDI_ALL_SOUND_OFF 0x78
#define MIDI_RESET_CONTROLLERS 0x79
#define MIDI_LOCAL	0x7a
#define MIDI_ALL_OFF	0x7b
#define MIDI_OMNI_OFF	0x7c
#define MIDI_OMNI_ON	0x7d
#define MIDI_MONO_ON	0x7e
#define MIDI_POLY_ON	0x7f

#define private static

//The event signaled when the app should be terminated.
HANDLE g_hTerminateEvent = NULL;
//Handles events that would normally terminate a console application. 
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType);

int Terminate();
string global_filename;
map<string,int> global_devicemap;
PaStreamParameters global_outputParameters;
PaError global_err;
string global_audiodevicename;
int global_outputAudioChannelSelectors[2];
PaAsioStreamInfo global_asioOutputInfo;

bool global_pauseplayback=false;


PmStream* global_pPmStreamMIDIIN;      // midi input 
boolean global_active = false;     // set when global_pPmStreamMIDIIN is ready for reading 
int global_inputmidideviceid = -1; //alesis q49 midi port id (when midi yoke installed)
map<string,int> global_inputmididevicemap;
string global_inputmididevicename = ""; //"In From MIDI Yoke:  1", "In From MIDI Yoke:  2", ... , "In From MIDI Yoke:  8"
int global_midichannelid=0; //0 for midi channel 1, etc.
int global_midictrlnumber=64; //midi control number between 0 and 127
bool global_receivemidi=false;

int debug = false;	// never set, but referenced by userio.c 
boolean in_sysex = false;   // we are reading a sysex message 
boolean inited = false;     // suppress printing during command line parsing 
boolean done = false;       // when true, exit 
boolean notes = true;       // show notes? 
boolean controls = true;    // show continuous controllers 
boolean bender = true;      // record pitch bend etc.? 
boolean excldata = true;    // record system exclusive data? 
boolean verbose = true;     // show text representation? 
boolean realdata = true;    // record real time messages? 
boolean clksencnt = true;   // clock and active sense count on 
boolean chmode = true;      // show channel mode messages 
boolean pgchanges = true;   // show program changes 
boolean flush = false;	    // flush all pending MIDI data 

uint32_t filter = 0;            // remember state of midi filter 

uint32_t clockcount = 0;        // count of clocks 
uint32_t actsensecount = 0;     // cout of active sensing bytes 
uint32_t notescount = 0;        // #notes since last request 
uint32_t notestotal = 0;        // total #notes 

char val_format[] = "    Val %d\n";


///////////////////////////////////////////////////////////////////////////////
//    Routines local to this module
///////////////////////////////////////////////////////////////////////////////

//private    void    mmexit(int code);
private void output(PmMessage data);
private int  put_pitch(int p);
private void showhelp();
private void showbytes(PmMessage data, int len, boolean newline);
private void showstatus(boolean flag);
private void doascii(char c);
private int  get_number(char *prompt);


// read a number from console
//
int get_number(char *prompt)
{
    char line[STRING_MAX];
    int n = 0, i;
    printf(prompt);
    while (n != 1) {
        n = scanf("%d", &i);
        fgets(line, STRING_MAX, stdin);

    }
    return i;
}


void receive_poll(PtTimestamp timestamp, void *userData)
{
    PmEvent event;
    int count; 
    if (!global_active) return;
    while ((count = Pm_Read(global_pPmStreamMIDIIN, &event, 1))) 
	{
        if (count == 1) 
		{
			//1) output message
			//output(event.message);

			//2) if cc value 0-63, keep recording
			//   else if cc value 64-127 pause recording
			int msgstatus = Pm_MessageStatus(event.message);
			//if( msgstatus>=MIDI_CTRL && msgstatus<(MIDI_CTRL+16) )
			if( (msgstatus-MIDI_CTRL)==global_midichannelid )
			{
				int ctrlnumber = Pm_MessageData1(event.message);
				if(ctrlnumber==global_midictrlnumber)
				{
					int ctrlvalue = Pm_MessageData2(event.message);
					if(ctrlvalue>=0 && ctrlvalue<64)
					{
						global_pauseplayback=false; //keep recording
						printf("unpause via midi\n"); fflush(stdout);
					}
					else
					{
						global_pauseplayback=true; //pause recording
						printf("pause via midi\n"); fflush(stdout);
					}
				}
			}
		}
        else            
		{
			printf(Pm_GetErrorText((PmError)count)); //spi a cast as (PmError)
		}
    }
}


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
static int playCallback( const void *inputBuffer, void *outputBuffer, 
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void *userData )
{
    float *out = (float*)outputBuffer;
	if(global_pauseplayback) 
	{
		//Reset output data first 
		memset(out, 0, framesPerBuffer * 2 * sizeof(float));
		return paContinue;
	}

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
 

//migrated out of the main so Terminate() can see it
paTestData          data = {0};
PaStream*           stream;
PaError             err = paNoError;

 
/*******************************************************************/
int main(int argc, char *argv[]);
int main(int argc, char *argv[])
{
	int nShowCmd = false;
	ShellExecuteA(NULL, "open", "begin.bat", "", NULL, nShowCmd);

	///////////////////
	//read in arguments
	///////////////////
	global_filename = "testrecording.wav"; //usage: spiplayfromdisk testrecording.wav -1 "E-MU ASIO" 0 1
	float fSecondsPlay = NUM_SECONDS; //-1 for playing once
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
	//global_audiodevicename="E-MU ASIO"; //"Wave (2- E-MU E-DSP Audio Proce"
	global_audiodevicename="Speakers (2- E-MU E-DSP Audio P"; //"E-MU ASIO"
	if(argc>3)
	{
		global_audiodevicename = argv[3]; //for spi, device name could be "E-MU ASIO", "Speakers (2- E-MU E-DSP Audio Processor (WDM))", etc.
	}
	//global_outputAudioChannelSelectors[0] = 0; // on emu patchmix ASIO device channel 1 (left)
	//global_outputAudioChannelSelectors[1] = 1; // on emu patchmix ASIO device channel 2 (right)
	//global_outputAudioChannelSelectors[0] = 2; // on emu patchmix ASIO device channel 3 (left)
	//global_outputAudioChannelSelectors[1] = 3; // on emu patchmix ASIO device channel 4 (right)
	global_outputAudioChannelSelectors[0] = 6; // on emu patchmix ASIO device channel 7 (left)
	global_outputAudioChannelSelectors[1] = 7; // on emu patchmix ASIO device channel 8 (right)
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
	if(argc>6)
	{
		global_receivemidi=true;
		global_inputmididevicename = argv[6]; //"In From MIDI Yoke:  1", "In From MIDI Yoke:  2", ... , "In From MIDI Yoke:  8"
	}
	if(argc>7)
	{
		global_midichannelid = atoi(argv[7]); //0 for midi channel 1, ..., up to 15 for midi channel 16
	}
	if(argc>8)
	{
		global_midictrlnumber = atoi(argv[8]); //midi control number between 0 and 127
	}

    //Auto-reset, initially non-signaled event 
    g_hTerminateEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    //Add the break handler
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

	/////////////////////
	//initialize portmidi
	/////////////////////
	if(global_receivemidi)
	{
		PmError err;
		Pm_Initialize(); //2013dec09, added by spi, was not there

		/////////////////////////////
		//input midi device selection
		/////////////////////////////
		const PmDeviceInfo* deviceInfo;
		int numDevices = Pm_CountDevices();
		for( int i=0; i<numDevices; i++ )
		{
			deviceInfo = Pm_GetDeviceInfo( i );
			if (deviceInfo->input)
			{
				string devicenamestring = deviceInfo->name;
				global_inputmididevicemap.insert(pair<string,int>(devicenamestring,i));
			}
		}
		map<string,int>::iterator it;
		it = global_inputmididevicemap.find(global_inputmididevicename);
		if(it!=global_inputmididevicemap.end())
		{
			global_inputmidideviceid = (*it).second;
			printf("%s maps to %d\n", global_inputmididevicename.c_str(), global_inputmidideviceid);
			deviceInfo = Pm_GetDeviceInfo(global_inputmidideviceid);
		}
		else
		{
			assert(false);
			for(it=global_inputmididevicemap.begin(); it!=global_inputmididevicemap.end(); it++)
			{
				printf("%s maps to %d\n", (*it).first.c_str(), (*it).second);
			}
			printf("input midi device not found\n");
		}

		// use porttime callback to empty midi queue and print 
		//Pt_Start(1, receive_poll, global_pInstrument); 
		Pt_Start(1, receive_poll, 0);
		// list device information 
		printf("MIDI input devices:\n");
		for (int i = 0; i < Pm_CountDevices(); i++) 
		{
			const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
			if (info->input) printf("%d: %s, %s\n", i, info->interf, info->name);
		}
		//inputmididevice = get_number("Type input device number: ");
		printf("device %d selected\n", global_inputmidideviceid);
		showhelp();

		err = Pm_OpenInput(&global_pPmStreamMIDIIN, global_inputmidideviceid, NULL, 512, NULL, NULL);
		if (err) 
		{
			printf(Pm_GetErrorText(err));
			Pt_Stop();
			Terminate();
			//mmexit(1);
		}
		/*
		//disable
		controls = false;
		filter ^= PM_FILT_CONTROL;
		//re-enable
		controls = true;
		filter ^= PM_FILT_CONTROL;
		*/
        bender = false;
        filter ^= PM_FILT_PITCHBEND;
        pgchanges = false;
        filter ^= PM_FILT_PROGRAM;
        notes = false;
        filter ^= PM_FILT_NOTE;
        excldata = false;
        filter ^= PM_FILT_SYSEX;
        realdata = false;
        filter ^= (PM_FILT_PLAY | PM_FILT_RESET | PM_FILT_TICK | PM_FILT_UNDEFINED);
        clksencnt = false;
        filter ^= PM_FILT_CLOCK;

		Pm_SetFilter(global_pPmStreamMIDIIN, filter);
		inited = true; // now can document changes, set filter 
		printf("Midi Monitoring ready.\n");
		global_active = true;
	}


    //PaStreamParameters  inputParameters;
    //PaStreamParameters  outputParameters;
    //PaStream*           stream;
    //PaError             err = paNoError;
    //paTestData          data = {0};
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
			err = startThread(&data, threadFunctionReadFromWavFile);
			if( err != paNoError ) goto done;
            err = Pa_StartStream( stream );
            if( err != paNoError ) goto done;
            printf("Waiting for playback to finish.\n"); fflush(stdout);
 
            // The playback will end when EOF is reached 
            //while( ( err = Pa_IsStreamActive( stream ) ) == 1 ) 
			float delayCntr=0;
			if(fSecondsPlay>0)
			{
				while( ( (err=Pa_IsStreamActive(stream)) == 1) && (delayCntr < fSecondsPlay) ) 
				{
					//printf("index = %d\n", data.frameIndex ); fflush(stdout);
					printf("play time = %f\n", delayCntr ); fflush(stdout);
					if(_kbhit() && _getch()=='p')
					{
						if(global_pauseplayback==false)
						{
							global_pauseplayback=true;
							printf("pause pressed\n"); fflush(stdout);
						}
						else
						{
							global_pauseplayback=false;
							printf("unpause pressed\n"); fflush(stdout);
						}
					}
					Pa_Sleep(1000);
					if(!global_pauseplayback) delayCntr++;
				}
			}
			else
			{
				while( (err=Pa_IsStreamActive(stream)) == 1)  
				{
					//printf("index = %d\n", data.frameIndex ); fflush(stdout);
					//delayCntr++;
					printf("play time = %f\n", delayCntr ); fflush(stdout);
					if(_kbhit() && _getch()=='p')
					{
						if(global_pauseplayback==false)
						{
							global_pauseplayback=true;
							printf("pause pressed\n"); fflush(stdout);
						}
						else
						{
							global_pauseplayback=false;
							printf("unpause pressed\n"); fflush(stdout);
						}
					}
					Pa_Sleep(1000);
					if(!global_pauseplayback) delayCntr++;
				}
			}
            if( err < 0 ) goto done;
        }
		/*
        err = Pa_CloseStream( stream );
        if( err != paNoError ) goto done;

        printf("Done.\n"); fflush(stdout);
		*/
    }
	
    printf("Done.\n"); fflush(stdout);

 
done:
	/*
    Pa_Terminate();
    if( data.ringBufferData )       // Sure it is NULL or valid. 
        PaUtil_FreeMemory( data.ringBufferData );
	*/
	Terminate();
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
	////////////////////
	//terminate portmidi
	////////////////////
	if(global_receivemidi)
	{
		global_active = false;
		Pm_Close(global_pPmStreamMIDIIN);
		Pt_Stop();
		Pm_Terminate();
	}
    err = Pa_CloseStream( stream );
    if( err != paNoError )
	{
        fprintf( stderr, "An error occured while using the portaudio stream\n" );
        fprintf( stderr, "Error number: %d\n", err );
        fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
        err = 1;          /* Always return 0 or 1, but no other return codes. */
		return err;
	}
    printf("Done.\n"); fflush(stdout);


    Pa_Terminate();
    if( data.ringBufferData )       // Sure it is NULL or valid. 
        PaUtil_FreeMemory( data.ringBufferData );

	printf("Exiting!\n"); fflush(stdout);

	int nShowCmd = false;
	ShellExecuteA(NULL, "open", "end.bat", "", NULL, nShowCmd);
	return 0;
}
 
//Called by the operating system in a separate thread to handle an app-terminating event. 
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT ||
        dwCtrlType == CTRL_BREAK_EVENT ||
        dwCtrlType == CTRL_CLOSE_EVENT)
    {
        // CTRL_C_EVENT - Ctrl+C was pressed 
        // CTRL_BREAK_EVENT - Ctrl+Break was pressed 
        // CTRL_CLOSE_EVENT - Console window was closed 
		Terminate();
        // Tell the main thread to exit the app 
        ::SetEvent(g_hTerminateEvent);
        return TRUE;
    }

    //Not an event handled by this function.
    //The only events that should be able to
	//reach this line of code are events that
    //should only be sent to services. 
    return FALSE;
}


///////////////////////////////////////////////////////////////////////////////
//               doascii
// Inputs:
//    char c: input character
// Effect: interpret to revise flags
///////////////////////////////////////////////////////////////////////////////

private void doascii(char c)
{
    if (isupper(c)) c = tolower(c);
    if (c == 'q') done = true;
    else if (c == 'b') {
        bender = !bender;
        filter ^= PM_FILT_PITCHBEND;
        if (inited)
            printf("Pitch Bend, etc. %s\n", (bender ? "ON" : "OFF"));
    } else if (c == 'c') {
        controls = !controls;
        filter ^= PM_FILT_CONTROL;
        if (inited)
            printf("Control Change %s\n", (controls ? "ON" : "OFF"));
    } else if (c == 'h') {
        pgchanges = !pgchanges;
        filter ^= PM_FILT_PROGRAM;
        if (inited)
            printf("Program Changes %s\n", (pgchanges ? "ON" : "OFF"));
    } else if (c == 'n') {
        notes = !notes;
        filter ^= PM_FILT_NOTE;
        if (inited)
            printf("Notes %s\n", (notes ? "ON" : "OFF"));
    } else if (c == 'x') {
        excldata = !excldata;
        filter ^= PM_FILT_SYSEX;
        if (inited)
            printf("System Exclusive data %s\n", (excldata ? "ON" : "OFF"));
    } else if (c == 'r') {
        realdata = !realdata;
        filter ^= (PM_FILT_PLAY | PM_FILT_RESET | PM_FILT_TICK | PM_FILT_UNDEFINED);
        if (inited)
            printf("Real Time messages %s\n", (realdata ? "ON" : "OFF"));
    } else if (c == 'k') {
        clksencnt = !clksencnt;
        filter ^= PM_FILT_CLOCK;
        if (inited)
            printf("Clock and Active Sense Counting %s\n", (clksencnt ? "ON" : "OFF"));
        if (!clksencnt) clockcount = actsensecount = 0;
    } else if (c == 's') {
        if (clksencnt) {
            if (inited)
                printf("Clock Count %ld\nActive Sense Count %ld\n", 
                        (long) clockcount, (long) actsensecount);
        } else if (inited) {
            printf("Clock Counting not on\n");
        }
    } else if (c == 't') {
        notestotal+=notescount;
        if (inited)
            printf("This Note Count %ld\nTotal Note Count %ld\n",
                    (long) notescount, (long) notestotal);
        notescount=0;
    } else if (c == 'v') {
        verbose = !verbose;
        if (inited)
            printf("Verbose %s\n", (verbose ? "ON" : "OFF"));
    } else if (c == 'm') {
        chmode = !chmode;
        if (inited)
            printf("Channel Mode Messages %s", (chmode ? "ON" : "OFF"));
    } else {
        if (inited) {
            if (c == ' ') {
                PmEvent event;
                while (Pm_Read(global_pPmStreamMIDIIN, &event, 1)) ;	// flush midi input 
                printf("...FLUSHED MIDI INPUT\n\n");
            } else showhelp();
        }
    }
    if (inited) Pm_SetFilter(global_pPmStreamMIDIIN, filter);
}

///////////////////////////////////////////////////////////////////////////////
//               output
// Inputs:
//    data: midi message buffer holding one command or 4 bytes of sysex msg
// Effect: format and print  midi data
///////////////////////////////////////////////////////////////////////////////

char vel_format[] = "    Vel %d\n";

private void output(PmMessage data)
{
    int command;    // the current command 
    int chan;   // the midi channel of the current event 
    int len;    // used to get constant field width 

    // printf("output data %8x; ", data); 

    command = Pm_MessageStatus(data) & MIDI_CODE_MASK;
    chan = Pm_MessageStatus(data) & MIDI_CHN_MASK;

    if (in_sysex || Pm_MessageStatus(data) == MIDI_SYSEX) {
#define sysex_max 16
        int i;
        PmMessage data_copy = data;
        in_sysex = true;
        // look for MIDI_EOX in first 3 bytes 
        // if realtime messages are embedded in sysex message, they will
        // be printed as if they are part of the sysex message
        //
        for (i = 0; (i < 4) && ((data_copy & 0xFF) != MIDI_EOX); i++) 
            data_copy >>= 8;
        if (i < 4) {
            in_sysex = false;
            i++; // include the EOX byte in output 
        }
        showbytes(data, i, verbose);
        if (verbose) printf("System Exclusive\n");
    } else if (command == MIDI_ON_NOTE && Pm_MessageData2(data) != 0) {
        notescount++;
        if (notes) {
            showbytes(data, 3, verbose);
            if (verbose) {
                printf("NoteOn  Chan %2d Key %3d ", chan, Pm_MessageData1(data));
                len = put_pitch(Pm_MessageData1(data));
                printf(vel_format + len, Pm_MessageData2(data));
            }
        }
    } else if ((command == MIDI_ON_NOTE // && Pm_MessageData2(data) == 0
                || command == MIDI_OFF_NOTE) && notes) {
        showbytes(data, 3, verbose);
        if (verbose) {
            printf("NoteOff Chan %2d Key %3d ", chan, Pm_MessageData1(data));
            len = put_pitch(Pm_MessageData1(data));
            printf(vel_format + len, Pm_MessageData2(data));
        }
    } else if (command == MIDI_CH_PROGRAM && pgchanges) {
        showbytes(data, 2, verbose);
        if (verbose) {
            printf("  ProgChg Chan %2d Prog %2d\n", chan, Pm_MessageData1(data) + 1);
        }
    } else if (command == MIDI_CTRL) {
               // controls 121 (MIDI_RESET_CONTROLLER) to 127 are channel
               // mode messages. 
        if (Pm_MessageData1(data) < MIDI_ALL_SOUND_OFF) {
            showbytes(data, 3, verbose);
            if (verbose) {
                printf("CtrlChg Chan %2d Ctrl %2d Val %2d\n",
                       chan, Pm_MessageData1(data), Pm_MessageData2(data));
            }
        } else if (chmode) { // channel mode 
            showbytes(data, 3, verbose);
            if (verbose) {
                switch (Pm_MessageData1(data)) {
                  case MIDI_ALL_SOUND_OFF:
                      printf("All Sound Off, Chan %2d\n", chan);
                    break;
                  case MIDI_RESET_CONTROLLERS:
                    printf("Reset All Controllers, Chan %2d\n", chan);
                    break;
                  case MIDI_LOCAL:
                    printf("LocCtrl Chan %2d %s\n",
                            chan, Pm_MessageData2(data) ? "On" : "Off");
                    break;
                  case MIDI_ALL_OFF:
                    printf("All Off Chan %2d\n", chan);
                    break;
                  case MIDI_OMNI_OFF:
                    printf("OmniOff Chan %2d\n", chan);
                    break;
                  case MIDI_OMNI_ON:
                    printf("Omni On Chan %2d\n", chan);
                    break;
                  case MIDI_MONO_ON:
                    printf("Mono On Chan %2d\n", chan);
                    if (Pm_MessageData2(data))
                        printf(" to %d received channels\n", Pm_MessageData2(data));
                    else
                        printf(" to all received channels\n");
                    break;
                  case MIDI_POLY_ON:
                    printf("Poly On Chan %2d\n", chan);
                    break;
                }
            }
        }
    } else if (command == MIDI_POLY_TOUCH && bender) {
        showbytes(data, 3, verbose);
        if (verbose) {
            printf("P.Touch Chan %2d Key %2d ", chan, Pm_MessageData1(data));
            len = put_pitch(Pm_MessageData1(data));
            printf(val_format + len, Pm_MessageData2(data));
        }
    } else if (command == MIDI_TOUCH && bender) {
        showbytes(data, 2, verbose);
        if (verbose) {
            printf("  A.Touch Chan %2d Val %2d\n", chan, Pm_MessageData1(data));
        }
    } else if (command == MIDI_BEND && bender) {
        showbytes(data, 3, verbose);
        if (verbose) {
            printf("P.Bend  Chan %2d Val %2d\n", chan,
                    (Pm_MessageData1(data) + (Pm_MessageData2(data)<<7)));
        }
    } else if (Pm_MessageStatus(data) == MIDI_SONG_POINTER) {
        showbytes(data, 3, verbose);
        if (verbose) {
            printf("    Song Position %d\n",
                    (Pm_MessageData1(data) + (Pm_MessageData2(data)<<7)));
        }
    } else if (Pm_MessageStatus(data) == MIDI_SONG_SELECT) {
        showbytes(data, 2, verbose);
        if (verbose) {
            printf("    Song Select %d\n", Pm_MessageData1(data));
        }
    } else if (Pm_MessageStatus(data) == MIDI_TUNE_REQ) {
        showbytes(data, 1, verbose);
        if (verbose) {
            printf("    Tune Request\n");
        }
    } else if (Pm_MessageStatus(data) == MIDI_Q_FRAME && realdata) {
        showbytes(data, 2, verbose);
        if (verbose) {
            printf("    Time Code Quarter Frame Type %d Values %d\n",
                    (Pm_MessageData1(data) & 0x70) >> 4, Pm_MessageData1(data) & 0xf);
        }
    } else if (Pm_MessageStatus(data) == MIDI_START && realdata) {
        showbytes(data, 1, verbose);
        if (verbose) {
            printf("    Start\n");
        }
    } else if (Pm_MessageStatus(data) == MIDI_CONTINUE && realdata) {
        showbytes(data, 1, verbose);
        if (verbose) {
            printf("    Continue\n");
        }
    } else if (Pm_MessageStatus(data) == MIDI_STOP && realdata) {
        showbytes(data, 1, verbose);
        if (verbose) {
            printf("    Stop\n");
        }
    } else if (Pm_MessageStatus(data) == MIDI_SYS_RESET && realdata) {
        showbytes(data, 1, verbose);
        if (verbose) {
            printf("    System Reset\n");
        }
    } else if (Pm_MessageStatus(data) == MIDI_TIME_CLOCK) {
        if (clksencnt) clockcount++;
        else if (realdata) {
            showbytes(data, 1, verbose);
            if (verbose) {
                printf("    Clock\n");
            }
        }
    } else if (Pm_MessageStatus(data) == MIDI_ACTIVE_SENSING) {
        if (clksencnt) actsensecount++;
        else if (realdata) {
            showbytes(data, 1, verbose);
            if (verbose) {
                printf("    Active Sensing\n");
            }
        }
    } else showbytes(data, 3, verbose);
    fflush(stdout);
}


/////////////////////////////////////////////////////////////////////////////
//               put_pitch
// Inputs:
//    int p: pitch number
// Effect: write out the pitch name for a given number
/////////////////////////////////////////////////////////////////////////////

private int put_pitch(int p)
{
    char result[8];
    static char *ptos[] = {
        "c", "cs", "d", "ef", "e", "f", "fs", "g",
        "gs", "a", "bf", "b"    };
    // note octave correction below 
    sprintf(result, "%s%d", ptos[p % 12], (p / 12) - 1);
    printf(result);
    return strlen(result);
}


/////////////////////////////////////////////////////////////////////////////
//               showbytes
// Effect: print hex data, precede with newline if asked
/////////////////////////////////////////////////////////////////////////////

char nib_to_hex[] = "0123456789ABCDEF";

private void showbytes(PmMessage data, int len, boolean newline)
{
    int count = 0;
    int i;

//    if (newline) {
//        putchar('\n');
//        count++;
//    } 
    for (i = 0; i < len; i++) {
        putchar(nib_to_hex[(data >> 4) & 0xF]);
        putchar(nib_to_hex[data & 0xF]);
        count += 2;
        if (count > 72) {
            putchar('.');
            putchar('.');
            putchar('.');
            break;
        }
        data >>= 8;
    }
    putchar(' ');
}



/////////////////////////////////////////////////////////////////////////////
//               showhelp
// Effect: print help text
/////////////////////////////////////////////////////////////////////////////

private void showhelp()
{
    printf("\n");
    printf("   Item Reported  Range     Item Reported  Range\n");
    printf("   -------------  -----     -------------  -----\n");
    printf("   Channels       1 - 16    Programs       1 - 128\n");
    printf("   Controllers    0 - 127   After Touch    0 - 127\n");
    printf("   Loudness       0 - 127   Pitch Bend     0 - 16383, center = 8192\n");
    printf("   Pitches        0 - 127, 60 = c4 = middle C\n");
    printf(" \n");
    printf("n toggles notes");
    showstatus(notes);
    printf("t displays noteon count since last t\n");
    printf("b toggles pitch bend, aftertouch");
    showstatus(bender);
    printf("c toggles continuous control");
    showstatus(controls);
    printf("h toggles program changes");
    showstatus(pgchanges);
    printf("x toggles system exclusive");
    showstatus(excldata);
    printf("k toggles clock and sense counting only");
    showstatus(clksencnt);
    printf("r toggles other real time messages & SMPTE");
    showstatus(realdata);
    printf("s displays clock and sense count since last k\n");
    printf("m toggles channel mode messages");
    showstatus(chmode);
    printf("v toggles verbose text");
    showstatus(verbose);
    printf("q quits\n");
    printf("\n");
}

/////////////////////////////////////////////////////////////////////////////
//               showstatus
// Effect: print status of flag
/////////////////////////////////////////////////////////////////////////////

private void showstatus(boolean flag)
{
    printf(", now %s\n", flag ? "ON" : "OFF" );
}
