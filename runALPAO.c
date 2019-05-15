/*
To compile:
>>>gcc runALPAO.c -o build/runALPAO -lImageStreamIO -lasdk -lpthread -lrt -lcfitsio

(You must already have the ALPAO SDK and cacao/milk installed.)

Usage:
To run with defaults
>>>./runALPAO <serialnumber>
To run with bias and normalization conventions disabled (not yet implemented):
>>>./runALPAO <serialnumber> --nobias --nonorm

For help:
>>>./runALPAO --help

What it does:
Connects to the ALPAO DM (indicated by its serial number), initializes the
shared memory image (if it doesn't already exist), and then commands the DM
from the image when the associated semaphores post.

Requires:
export ACECFG=$HOME/ALPAO/Config
where Config contains the ALPAO configuration files as well as the user-defined
calibration file <serial>_userconfig.txt

Still to be implemented or determined:
-Multiplexed virtual DM
*/

/* System Headers */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <signal.h>
#include <argp.h>
#include <string.h>

/* cacao */
#include "ImageStruct.h"   // cacao data structure definition
#include "ImageStreamIO.h" // function ImageStreamIO_read_sharedmem_image_toIMAGE()

/* Alpao SDK C Header */
#include "asdkWrapper.h"

/* FITS */
#include "fitsio.h"

// interrupt signal handling for safe DM shutdown
volatile sig_atomic_t stop;

void handle_signal(int signal)
{
    if (signal == SIGINT)
    {
        printf("\nExiting the ALPAO control loop.\n");
        stop = 1;
    }
}

// Initialize the shared memory image
void initializeSharedMemory(const char * shm_name, int ax1, int ax2)
{
    long naxis; // number of axis
    uint8_t atype;     // data type
    uint32_t *imsize;  // image size 
    int shared;        // 1 if image in shared memory
    int NBkw;          // number of keywords supported
    IMAGE* SMimage;

    SMimage = (IMAGE*) malloc(sizeof(IMAGE));

    naxis = 2;
    imsize = (uint32_t *) malloc(sizeof(uint32_t)*naxis);
    imsize[0] = ax1;
    imsize[1] = ax2;
    
    // image will be float type
    // see file ImageStruct.h for list of supported types
    atype = _DATATYPE_FLOAT;
    // image will be in shared memory
    shared = 1;
    // allocate space for 10 keywords
    NBkw = 10;
    // create an image in shared memory
    ImageStreamIO_createIm(&SMimage[0], shm_name, naxis, imsize, atype, shared, NBkw);

    /* flush all semaphores to avoid commanding the DM from a 
    backlog in shared memory */
    ImageStreamIO_semflush(&SMimage[0], -1);
    
    // write 0s to the image
    SMimage[0].md[0].write = 1; // set this flag to 1 when writing data
    int i;
    for (i = 0; i < 11*11; i++)
    {
      SMimage[0].array.F[i] = 0.;
    }

    // post all semaphores
    ImageStreamIO_sempost(&SMimage[0], -1);
        
    SMimage[0].md[0].write = 0; // Done writing data
    SMimage[0].md[0].cnt0++;
    SMimage[0].md[0].cnt1++;
}


/* Convert any DM inputs with an absolute fractional stroke
> 1 to 1 to avoid exceeding safe DM operation. */
void clip_to_limits(Scalar * dminputs, int nbAct)
{
    int idx;
    // check each actuator and clip if needed
    for ( idx = 0 ; idx < nbAct ; idx++)
    {
        if (dminputs[idx] > 1)
        {
            printf("Actuator %d saturated!\n", idx + 1);
            dminputs[idx] = 1;
        } else if (dminputs[idx] < -1)
        {
            printf("Actuator %d saturated!\n", idx + 1);
            dminputs[idx] = - 1;
        }
    }
}

/* ASDK expects inputs between -1 and +1, but we'd like to provide
stroke values in physical units. This function converts from microns
of stroke to fractional stroke. This requires DM calibration. */
void microns_to_fractional_stroke(Scalar * dminputs, int nbAct, Scalar max_stroke)
{
    int idx;
    // normalize each actuator stroke
    for ( idx = 0 ; idx < nbAct ; idx++)
    {
        dminputs[idx] /= max_stroke;
    }
}

/* Normalize inputs such that volume displaced by the requested command roughly
matches the equivalent volume that would be displaced by a cuboid of dimensions
actuator-pitch x actuator-pitch x normalized-stroke. This is a constant factor 
that's found by calculating the volume under the DM influence function. */
void normalize_inputs(Scalar * dminputs, int nbAct, Scalar volume_factor)
{
    int idx;
    // normalize each actuator stroke
    for ( idx = 0 ; idx < nbAct ; idx++)
    {
        dminputs[idx] *= volume_factor;
    }
}

/* Remove DC bias in inputs to maximize actuator range */
void bias_inputs(Scalar * dminputs, int nbAct)
{
    int idx;
    Scalar mean;

    // calculate mean value
    mean = 0;
    for ( idx = 0 ; idx < nbAct ; idx++)
    {
        mean += dminputs[idx];
    }
    mean /= nbAct;

    // remove mean from each actuator input
    for ( idx = 0 ; idx < nbAct ; idx++)
    {
        dminputs[idx] -= mean;
    }
}

/* Read in a configuration file with user-calibrated
values to determine the conversion from physical to
fractional stroke as well as the volume displaced by
the influence function. */
int parse_calibration_file(const char * serial, Scalar *max_stroke, Scalar *volume_factor)
{
    char * alpao_calib;
    char calibname[1000];
    char calibpath[1000];
    char serial_lc[1000];
    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;
    char * token;
    Scalar * calibvals;

    // force serial to be lower case
    for(int i = 0; serial[i]; i++){
      serial_lc[i] = tolower(serial[i]);
    }

    // find calibration file location from alpao_calib env variable
    alpao_calib = getenv("alpao_calib");
    strcpy(calibpath, alpao_calib);
    sprintf(calibname, "/alpao_%s/%s_userconfig.txt", serial_lc, serial_lc);
    strcat(calibpath, calibname);

    // open file
    fp = fopen(calibpath, "r");
    if (fp == NULL)
    {
        printf("Could not read configuration file at %s!\n", calibpath);
        return -1;
    }

    calibvals = (Scalar*) malloc(2*sizeof(Scalar));
    int idx = 0;
    while ((read = getline(&line, &len, fp)) != -1)
    {
        // grab first value from each line
        calibvals[idx] = strtod(line, NULL);
        idx++;
    }

    fclose(fp);

    // assign stroke and volume factors
    (*max_stroke) = calibvals[0];
    (*volume_factor) = calibvals[1];

    printf("ALPAO %s: Using stroke and volume calibration from %s\n", serial, calibpath);
    return 0;
}

int get_actuator_mapping(const char * serial, int nbAct, int * actuator_mapping)
{
    /* This function closely follows the CFITSIO imstat
    example */

    fitsfile *fptr;  /* FITS file pointer */
    int status = 0;  /* CFITSIO status value MUST be initialized to zero! */
    int hdutype, naxis, ii;
    long naxes[2], totpix, fpixel[2];
    int *pix;
    int ij = 0; /* actuator mapping index */

    char * alpao_calib;
    char calibname[1000];
    char calibpath[1000];
    char serial_lc[1000];

    // get file path to actuator map

    // force serial to be lower case
    for(int i = 0; serial[i]; i++){
      serial_lc[i] = tolower(serial[i]);
    }

    alpao_calib = getenv("alpao_calib");
    strcpy(calibpath, alpao_calib);
    sprintf(calibname, "/alpao_%s/%s_actuator_mapping.fits", serial_lc, serial_lc);
    strcat(calibpath, calibname);

    if ( !fits_open_image(&fptr, calibpath, READONLY, &status) )
    {
      if (fits_get_hdu_type(fptr, &hdutype, &status) || hdutype != IMAGE_HDU) { 
        printf("Error: this program only works on images, not tables\n");
        return(1);
      }

      fits_get_img_dim(fptr, &naxis, &status);
      fits_get_img_size(fptr, 2, naxes, &status);

      if (status || naxis != 2) { 
        printf("Error: NAXIS = %d.  Only 2-D images are supported.\n", naxis);
        return(1);
      }

      pix = (int *) malloc(naxes[0] * sizeof(int)); /* memory for 1 row */

      if (pix == NULL) {
        printf("Memory allocation error\n");
        return(1);
      }

      totpix = naxes[0] * naxes[1];
      fpixel[0] = 1;  /* read starting with first pixel in each row */

      /* process image one row at a time; increment row # in each loop */
      //for (fpixel[1] = 1; fpixel[1] <= naxes[1]; fpixel[1]++)
      for (fpixel[1] = naxes[1]; fpixel[1] >= 1; fpixel[1]--)
      {  
         /* give starting pixel coordinate and number of pixels to read */
         if (fits_read_pix(fptr, TINT, fpixel, naxes[0],0, pix,0, &status))
            break;   /* jump out of loop on error */

         // get indices of active actuators in order
         for (ii = 0; ii < naxes[0]; ii++) {
           if (pix[ii] > 0) {
                actuator_mapping[ij] = (fpixel[1]-1) * naxes[0] + ii;
                ij++;
           }
         }
      }
      fits_close_file(fptr, &status);
    }

    if (status)  {
        fits_report_error(stderr, status); /* print any error message */
    }

    free(pix);

    printf("ALPAO %s: Using actuator mapping from %s\n", serial, calibpath);
    return 0;
}

/* Send command to mirror from shared memory */
int sendCommand(asdkDM * dm, IMAGE * SMimage, int nbAct, int nobias, int nonorm, int fractional, Scalar max_stroke, Scalar volume_factor, int * actuator_mapping)
{
    COMPL_STAT ret;
    int idx;
    Scalar * dminputs;

    // Cast to array type ALPAO expects
    // Scalar = double
    // Shared memory image = float
    dminputs = (Scalar*) calloc( nbAct, sizeof( Scalar ) );
    for ( idx = 0 ; idx < nbAct ; idx++ )
    {
        // use actuator mapping to pull correct element of shared memory image
        dminputs[idx] = (Scalar)SMimage[0].array.F[actuator_mapping[idx]];
    }

    // First, convert raw displacements to volume-normalized displacements (microns)
    if (nonorm != 1)
    {
        normalize_inputs(dminputs, nbAct, volume_factor);
    }

    /* Second, convert from displacement (in microns) to fractional
    stroke (-1 to +1) that the ALPAO SDK expects */
    if (fractional != 1)
    {
        microns_to_fractional_stroke(dminputs, nbAct, max_stroke);
    }
    // Third, remove DC bias in inputs
    if (nobias != 1)
    {
        bias_inputs(dminputs, nbAct);
    }

    /* Fourth, clip to fractional values between -1 and 1.
    The ALPAO SDK doesn't seem to check for this, which
    is scary and a little odd. */
    clip_to_limits(dminputs, nbAct);

    //for (idx = 0; idx < nbAct; idx++) {
    //    printf("Act %d: %f\n", idx, dminputs[idx]);
    //} 

    /* Finally, send the command to the DM */
    ret = asdkSend(dm, dminputs);

    /* Release memory */
    free( dminputs );

    return ret;
}

// intialize DM and shared memory and enter DM command loop
int controlLoop(const char * serial, const char * shm_name, int nobias, int nonorm, int fractional)
{
    int n, idx;
    UInt nbAct;
    COMPL_STAT ret;
    Scalar     tmp;
    IMAGE * SMimage;
    Scalar max_stroke;
    Scalar volume_factor;
    int *actuator_mapping;
    int shm_dim = 11;

    /* get max stroke and volume normalization factor from
    the user-defined config file */
    ret = parse_calibration_file(serial, &max_stroke, &volume_factor);

    if (ret == -1)
    {
        return -1;
    }

    //initialize DM
    asdkDM * dm = NULL;
    dm = asdkInit(serial);
    if (dm == NULL)
    {
        return -1;
    }

    // Get number of actuators
    ret = asdkGet( dm, "NbOfActuator", &tmp );
    if (ret == -1)
    {
        return -1;
    }
    nbAct = (UInt) tmp;

    /* get actuator mapping from 2D cacao image to 1D vector for
    ALPAO input */
    actuator_mapping = (int *) malloc(nbAct * sizeof(int)); /* memory for actuator mapping */
    get_actuator_mapping(serial, nbAct, actuator_mapping);

    // initialize shared memory image to 0s
    initializeSharedMemory(shm_name, shm_dim, shm_dim);

    // connect to shared memory image (SMimage)
    SMimage = (IMAGE*) malloc(sizeof(IMAGE));
    ImageStreamIO_read_sharedmem_image_toIMAGE(shm_name, &SMimage[0]);

    // Validate SMimage dimensionality and size against DM
    if (SMimage[0].md[0].naxis != 2) {
        printf("SM image naxis = %d\n", SMimage[0].md[0].naxis);
        return -1;
    }
    if (SMimage[0].md[0].size[0] != shm_dim) {
        printf("SM image size (axis 1) = %d", SMimage[0].md[0].size[0]);
        return -1;
    }
    if (SMimage[0].md[0].size[1] != shm_dim) {
        printf("SM image size (axis 2) = %d", SMimage[0].md[0].size[1]);
        return -1;
    }

    // set DM to all-0 state to begin
    printf("ALPAO %s: initializing all actuators to 0.\n", serial);
    ImageStreamIO_semwait(&SMimage[0], 0);
    //printf("%f\n%f\n", max_stroke, volume_factor);
    ret = sendCommand(dm, SMimage, nbAct, nobias, nonorm, fractional, max_stroke, volume_factor, actuator_mapping);
    if (ret == -1)
    {
        return -1;
    }

    // SIGINT handling
    struct sigaction action;
    action.sa_flags = SA_SIGINFO;
    action.sa_handler = handle_signal;
    sigaction(SIGINT, &action, NULL);
    stop = 0;

    // control loop
    while (!stop)
    {
        //printf("ALPAO %s: waiting on commands.\n", serial);
        // Wait on semaphore update
        ImageStreamIO_semwait(&SMimage[0], 0);
        
        // Send Command to DM
        if (!stop) // Skip DM on interrupt signal
        {
            //printf("ALPAO %s: sending command with nobias=%d, nonorm=%d, and fractional=%d.\n", serial, nobias, nonorm, fractional);
            ret = sendCommand(dm, SMimage, nbAct, nobias, nonorm, fractional, max_stroke, volume_factor, actuator_mapping);
            if (ret == -1)
            {
                return -1;
            }
        }
    }

    // Safe DM shutdown on interrupt
    printf("ALPAO %s: resetting and releasing DM.\n", serial);
    // Reset and release ALPAO
    asdkReset(dm);
    ret = asdkRelease(dm);
    dm = NULL;

    return ret;
}

/*
Argument parsing
*/

/* Program documentation. */
static char doc[] =
  "runALPAO-- enter the ALPAO DM <serial> command loop and wait for cacao shared memory images to be posted at <shm_name>";

/* A description of the arguments we accept. */
static char args_doc[] = "serial shm_name";

/* The options we understand. */
static struct argp_option options[] = {
  {"nobias",     'b', 0, 0,  "Disable automatically biasing the DM (enabled by default)" },
  {"nonorm",     'n', 0, 0,  "Disable displacement normalization (enabled by default)" },
  {"fractional", 'f', 0, 0,  "Give inputs in fractional stroke (-1 to +1) rather than microns" },
  { 0 }
};

/* Used by main to communicate with parse_opt. */
struct arguments
{
  const char *args[2];    /* serial and shared memory name */
  int nobias, nonorm, fractional;
};

/* Parse a single option. */
static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
  /* Get the input argument from argp_parse, which we
     know is a pointer to our arguments structure. */
  struct arguments *arguments = state->input;

  switch (key)
    {
    case 'b':
      arguments->nobias = 1;
      break;
    case 'n':
      arguments->nonorm = 1;
      break;
    case 'f':
      arguments->fractional = 1;

    case ARGP_KEY_ARG:
      if (state->arg_num >= 2)
        /* Too many arguments. */
        argp_usage (state);

      arguments->args[state->arg_num] = arg;

      break;

    case ARGP_KEY_END:
      if (state->arg_num < 2)
        /* Not enough arguments. */
        argp_usage (state);
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

/* Our argp parser. */
static struct argp argp = { options, parse_opt, args_doc, doc };

/* Main program */
int main( int argc, char ** argv )
{
    struct arguments arguments;
    const char * serial;
    const char * shm_name;

    /* Default values. */
    arguments.nobias = 0;
    arguments.nonorm = 0;
    arguments.fractional = 0;

    /* Parse our arguments; every option seen by parse_opt will
     be reflected in arguments. */
    argp_parse (&argp, argc, argv, 0, 0, &arguments);

    serial = arguments.args[0];
    shm_name = arguments.args[1];

    // enter the control loop
    int ret = controlLoop(serial, shm_name, arguments.nobias, arguments.nonorm, arguments.fractional);
    asdkPrintLastError();

    return ret;
}
