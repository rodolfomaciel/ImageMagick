/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%                   OOO   PPPP   EEEEE  N   N   CCCC  L                       %
%                  O   O  P   P  E      NN  N  C      L                       %
%                  O   O  PPPP   EEE    N N N  C      L                       %
%                  O   O  P      E      N  NN  C      L                       %
%                   OOO   P      EEEEE  N   N   CCCC  LLLLL                   %
%                                                                             %
%                                                                             %
%                         MagickCore OpenCL Methods                           %
%                                                                             %
%                              Software Design                                %
%                                   Cristy                                    %
%                                 March 2000                                  %
%                                                                             %
%                                                                             %
%  Copyright 1999-2016 ImageMagick Studio LLC, a non-profit organization      %
%  dedicated to making software imaging solutions freely available.           %
%                                                                             %
%  You may not use this file except in compliance with the License.  You may  %
%  obtain a copy of the License at                                            %
%                                                                             %
%    http://www.imagemagick.org/script/license.php                            %
%                                                                             %
%  Unless required by applicable law or agreed to in writing, software        %
%  distributed under the License is distributed on an "AS IS" BASIS,          %
%  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   %
%  See the License for the specific language governing permissions and        %
%  limitations under the License.                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%
%
*/

/*
  Include declarations.
*/
#include "MagickCore/studio.h"
#include "MagickCore/artifact.h"
#include "MagickCore/cache.h"
#include "MagickCore/color.h"
#include "MagickCore/compare.h"
#include "MagickCore/constitute.h"
#include "MagickCore/distort.h"
#include "MagickCore/draw.h"
#include "MagickCore/effect.h"
#include "MagickCore/exception.h"
#include "MagickCore/exception-private.h"
#include "MagickCore/fx.h"
#include "MagickCore/gem.h"
#include "MagickCore/geometry.h"
#include "MagickCore/image.h"
#include "MagickCore/image-private.h"
#include "MagickCore/layer.h"
#include "MagickCore/mime-private.h"
#include "MagickCore/memory_.h"
#include "MagickCore/monitor.h"
#include "MagickCore/montage.h"
#include "MagickCore/morphology.h"
#include "MagickCore/nt-base.h"
#include "MagickCore/nt-base-private.h"
#include "MagickCore/opencl.h"
#include "MagickCore/opencl-private.h"
#include "MagickCore/option.h"
#include "MagickCore/policy.h"
#include "MagickCore/property.h"
#include "MagickCore/quantize.h"
#include "MagickCore/quantum.h"
#include "MagickCore/random_.h"
#include "MagickCore/random-private.h"
#include "MagickCore/resample.h"
#include "MagickCore/resource_.h"
#include "MagickCore/splay-tree.h"
#include "MagickCore/semaphore.h"
#include "MagickCore/statistic.h"
#include "MagickCore/string_.h"
#include "MagickCore/token.h"
#include "MagickCore/utility.h"

#ifdef MAGICKCORE_CLPERFMARKER
#include "CLPerfMarker.h"
#endif


#if defined(MAGICKCORE_OPENCL_SUPPORT)

#ifdef MAGICKCORE_HAVE_OPENCL_CL_H
#define MAGICKCORE_OPENCL_MACOSX  1
#endif

#define NUM_CL_RAND_GENERATORS 1024  /* number of random number generators running in parallel */
#define PROFILE_OCL_KERNELS 0

typedef struct
{
  cl_ulong min;
  cl_ulong max;
  cl_ulong total;
  cl_ulong count;
} KernelProfileRecord;

static const char *kernelNames[] = {
  "AddNoise",
  "BlurRow",
  "BlurColumn",
  "Composite",
  "ComputeFunction",
  "Contrast",
  "ContrastStretch",
  "Convolve",
  "Equalize",
  "GrayScale",
  "Histogram",
  "HullPass1",
  "HullPass2",
  "LocalContrastBlurRow",
  "LocalContrastBlurApplyColumn",
  "Modulate",
  "MotionBlur",
  "RandomNumberGenerator",
  "ResizeHorizontal",
  "ResizeVertical",
  "RotationalBlur",
  "UnsharpMaskBlurColumn",
  "UnsharpMask",
  "WaveletDenoise",
  "NONE" };

KernelProfileRecord
  profileRecords[KERNEL_COUNT];

typedef struct _AccelerateTimer {
  long long _freq;
  long long _clocks;
  long long _start;
} AccelerateTimer;

void startAccelerateTimer(AccelerateTimer* timer) {
#ifdef _WIN32
      QueryPerformanceCounter((LARGE_INTEGER*)&timer->_start);	


#else
      struct timeval s;
      gettimeofday(&s, 0);
      timer->_start = (long long)s.tv_sec * (long long)1.0E3 + (long long)s.tv_usec / (long long)1.0E3;
#endif
}

void stopAccelerateTimer(AccelerateTimer* timer) {
      long long n=0;
#ifdef _WIN32
      QueryPerformanceCounter((LARGE_INTEGER*)&(n));	
#else
      struct timeval s;
      gettimeofday(&s, 0);
      n = (long long)s.tv_sec * (long long)1.0E3+ (long long)s.tv_usec / (long long)1.0E3;
#endif
      n -= timer->_start;
      timer->_start = 0;
      timer->_clocks += n;
}

void resetAccelerateTimer(AccelerateTimer* timer) {
   timer->_clocks = 0;
   timer->_start = 0;
}

void initAccelerateTimer(AccelerateTimer* timer) {
#ifdef _WIN32
    QueryPerformanceFrequency((LARGE_INTEGER*)&timer->_freq);
#else
    timer->_freq = (long long)1.0E3;
#endif
   resetAccelerateTimer(timer);
}

double readAccelerateTimer(AccelerateTimer* timer) {
  return (double)timer->_clocks/(double)timer->_freq;
};

MagickPrivate void RecordProfileData(MagickCLEnv clEnv, ProfiledKernels kernel, cl_event event)
{
#if PROFILE_OCL_KERNELS
  cl_int status;
  cl_ulong start = 0;
  cl_ulong end = 0;
  cl_ulong elapsed = 0;
  clEnv->library->clWaitForEvents(1, &event);
  status = clEnv->library->clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  status &= clEnv->library->clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  if (status == CL_SUCCESS) {
    start /= 1000;	// usecs
    end /= 1000;	// usecs
    elapsed = end - start;
    /* we can use the commandQueuesLock to make the code below thread safe */
    LockSemaphoreInfo(clEnv->commandQueuesLock);
    if ((elapsed < profileRecords[kernel].min) || (profileRecords[kernel].count == 0))
      profileRecords[kernel].min = elapsed;
    if (elapsed > profileRecords[kernel].max)
      profileRecords[kernel].max = elapsed;
    profileRecords[kernel].total += elapsed;
    profileRecords[kernel].count += 1;
    UnlockSemaphoreInfo(clEnv->commandQueuesLock);
  }
#else
  magick_unreferenced(clEnv);
  magick_unreferenced(kernel);
  magick_unreferenced(event);
#endif
}

void DumpProfileData()
{
#if PROFILE_OCL_KERNELS
  int i;

  OpenCLLog("====================================================");

  // Write out the device info to the profile
  if (0 == 1)
  {
    MagickCLEnv clEnv;
    char buff[2048];
    cl_int status;

    clEnv = GetDefaultOpenCLEnv();

    status = clEnv->library->clGetDeviceInfo(clEnv->device, CL_DEVICE_VENDOR, 2048, buff, NULL);
    OpenCLLog(buff);

    status = clEnv->library->clGetDeviceInfo(clEnv->device, CL_DEVICE_NAME, 2048, buff, NULL);
    OpenCLLog(buff);

    status = clEnv->library->clGetDeviceInfo(clEnv->device, CL_DRIVER_VERSION, 2048, buff, NULL);
    OpenCLLog(buff);
  }

  OpenCLLog("====================================================");
  OpenCLLog("                        ave\tcalls   \tmin -> max");
  OpenCLLog("                        ---\t-----   \t----------");
  for (i = 0; i < KERNEL_COUNT; ++i) {
    char buf[4096];
    char indent[160];
    strcpy(indent, "                              ");
    strncpy(indent, kernelNames[i], min(strlen(kernelNames[i]), strlen(indent) - 1));
    sprintf(buf, "%s%d\t(%d calls)   \t%d -> %d", indent, profileRecords[i].count > 0 ? (profileRecords[i].total / profileRecords[i].count) : 0, profileRecords[i].count, profileRecords[i].min, profileRecords[i].max);
    //printf("%s%d\t(%d calls)   \t%d -> %d\n", indent, profileRecords[i].count > 0 ? (profileRecords[i].total / profileRecords[i].count) : 0, profileRecords[i].count, profileRecords[i].min, profileRecords[i].max);
    OpenCLLog(buf);
  }
  OpenCLLog("====================================================");
#endif
}

/*
 *
 * Dynamic library loading functions
 *
 */
#ifdef MAGICKCORE_WINDOWS_SUPPORT
#else
#include <dlfcn.h>
#endif

// dynamically load a library.  returns NULL on failure
void *OsLibraryLoad(const char *libraryName)
{
#ifdef MAGICKCORE_WINDOWS_SUPPORT
    return (void *)LoadLibraryA(libraryName);
#else
    return (void *)dlopen(libraryName, RTLD_NOW);
#endif
}

// get a function pointer from a loaded library.  returns NULL on failure.
void *OsLibraryGetFunctionAddress(void *library, const char *functionName)
{
#ifdef MAGICKCORE_WINDOWS_SUPPORT
    if (!library || !functionName)
    {
        return NULL;
    }
    return (void *) GetProcAddress( (HMODULE)library, functionName);
#else
    if (!library || !functionName)
    {
        return NULL;
    }
    return (void *)dlsym(library, functionName);
#endif
}

// unload a library.
void OsLibraryUnload(void *library)
{
#ifdef MAGICKCORE_WINDOWS_SUPPORT
    FreeLibrary( (HMODULE)library);
#else
    dlclose(library);
#endif
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   A c q u i r e M a g i c k O p e n C L E n v                               %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
% AcquireMagickOpenCLEnv() allocates the MagickCLEnv structure
%
*/

MagickExport MagickCLEnv AcquireMagickOpenCLEnv(void)
{
  MagickCLEnv clEnv;
  clEnv = (MagickCLEnv) AcquireMagickMemory(sizeof(struct _MagickCLEnv));
  if (clEnv != NULL)
  {
    memset(clEnv, 0, sizeof(struct _MagickCLEnv));
    clEnv->commandQueuesPos=-1;
    ActivateSemaphoreInfo(&clEnv->lock);
    ActivateSemaphoreInfo(&clEnv->commandQueuesLock);
  }
  return clEnv;
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   R e l i n q u i s h M a g i c k O p e n C L E n v                         %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  RelinquishMagickOpenCLEnv() destroy the MagickCLEnv structure
%
%  The format of the RelinquishMagickOpenCLEnv method is:
%
%      MagickBooleanType RelinquishMagickOpenCLEnv(MagickCLEnv clEnv)
%
%  A description of each parameter follows:
%
%    o clEnv: MagickCLEnv structure to destroy
%
*/

MagickExport MagickBooleanType RelinquishMagickOpenCLEnv(MagickCLEnv clEnv)
{
  if (clEnv != (MagickCLEnv) NULL)
  {
    while (clEnv->commandQueuesPos >= 0)
    {
      clEnv->library->clReleaseCommandQueue(
        clEnv->commandQueues[clEnv->commandQueuesPos--]);
    }
    RelinquishSemaphoreInfo(&clEnv->lock);
    RelinquishSemaphoreInfo(&clEnv->commandQueuesLock);
    RelinquishMagickMemory(clEnv);
    return MagickTrue;
  }
  return MagickFalse;
}


/*
* Default OpenCL environment
*/
MagickCLEnv defaultCLEnv;
SemaphoreInfo* defaultCLEnvLock;

/*
* OpenCL library
*/
MagickLibrary * OpenCLLib;
SemaphoreInfo* OpenCLLibLock;


static MagickBooleanType bindOpenCLFunctions(void* library)
{
#ifdef MAGICKCORE_OPENCL_MACOSX
#define BIND(X) OpenCLLib->X= &X;
#else
#define BIND(X)\
  if ((OpenCLLib->X=(MAGICKpfn_##X)OsLibraryGetFunctionAddress(library,#X)) == NULL)\
  return MagickFalse;
#endif

  BIND(clGetPlatformIDs);
  BIND(clGetPlatformInfo);

  BIND(clGetDeviceIDs);
  BIND(clGetDeviceInfo);

  BIND(clCreateContext);

  BIND(clCreateBuffer);
  BIND(clReleaseMemObject);

  BIND(clCreateProgramWithSource);
  BIND(clCreateProgramWithBinary);
  BIND(clBuildProgram);
  BIND(clGetProgramInfo);
  BIND(clGetProgramBuildInfo);

  BIND(clCreateKernel);
  BIND(clReleaseKernel);
  BIND(clSetKernelArg);

  BIND(clFlush);
  BIND(clFinish);

  BIND(clEnqueueNDRangeKernel);
  BIND(clEnqueueReadBuffer);
  BIND(clEnqueueMapBuffer);
  BIND(clEnqueueUnmapMemObject);

  BIND(clCreateCommandQueue);
  BIND(clReleaseCommandQueue);

  BIND(clGetEventProfilingInfo);
  BIND(clWaitForEvents);
  BIND(clReleaseEvent);

  return MagickTrue;
}

MagickLibrary * GetOpenCLLib()
{
  if (OpenCLLib == NULL)
  {
    if (OpenCLLibLock == NULL)
    {
      ActivateSemaphoreInfo(&OpenCLLibLock);
    }

    LockSemaphoreInfo(OpenCLLibLock);

    OpenCLLib = (MagickLibrary *) AcquireMagickMemory (sizeof (MagickLibrary));

    if (OpenCLLib != NULL)
    {
      MagickBooleanType status = MagickFalse;
      void * library = NULL;

#ifdef MAGICKCORE_OPENCL_MACOSX
      status = bindOpenCLFunctions(library);
#else

      memset(OpenCLLib, 0, sizeof(MagickLibrary));
#ifdef MAGICKCORE_WINDOWS_SUPPORT
      library = OsLibraryLoad("OpenCL.dll");
#else
      library = OsLibraryLoad("libOpenCL.so");
#endif
      if (library)
        status = bindOpenCLFunctions(library);

      if (status==MagickTrue)
        OpenCLLib->base=library;
      else
        OpenCLLib=(MagickLibrary *)RelinquishMagickMemory(OpenCLLib);
#endif
    }

    UnlockSemaphoreInfo(OpenCLLibLock);
  }


  return OpenCLLib;
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   G e t D e f a u l t O p e n C L E n v                                     %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  GetDefaultOpenCLEnv() returns the default OpenCL env
%
%  The format of the GetDefaultOpenCLEnv method is:
%
%      MagickCLEnv GetDefaultOpenCLEnv()
%
%  A description of each parameter follows:
%
%    o exception: return any errors or warnings.
%
*/

MagickExport MagickCLEnv GetDefaultOpenCLEnv(void)
{
  if (defaultCLEnv == NULL)
  {
    if (defaultCLEnvLock == NULL)
    {
      ActivateSemaphoreInfo(&defaultCLEnvLock);
    }
    LockSemaphoreInfo(defaultCLEnvLock);
    if (defaultCLEnv == NULL)
        defaultCLEnv = AcquireMagickOpenCLEnv();
    UnlockSemaphoreInfo(defaultCLEnvLock);
  }
  return defaultCLEnv;
}

static void LockDefaultOpenCLEnv() {
  if (defaultCLEnvLock == NULL)
  {
    ActivateSemaphoreInfo(&defaultCLEnvLock);
  }
  LockSemaphoreInfo(defaultCLEnvLock);
}

static void UnlockDefaultOpenCLEnv() {
  if (defaultCLEnvLock == NULL)
  {
    ActivateSemaphoreInfo(&defaultCLEnvLock);
  }
  else
    UnlockSemaphoreInfo(defaultCLEnvLock);
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   S e t D e f a u l t O p e n C L E n v                                     %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  SetDefaultOpenCLEnv() sets the new OpenCL environment as default
%  and returns the old OpenCL environment
%
%  The format of the SetDefaultOpenCLEnv() method is:
%
%      MagickCLEnv SetDefaultOpenCLEnv(MagickCLEnv clEnv)
%
%  A description of each parameter follows:
%
%    o clEnv: the new default OpenCL environment.
%
*/
MagickExport MagickCLEnv SetDefaultOpenCLEnv(MagickCLEnv clEnv)
{
  MagickCLEnv oldEnv;
  LockDefaultOpenCLEnv();
  oldEnv = defaultCLEnv;
  defaultCLEnv = clEnv;
  UnlockDefaultOpenCLEnv();
  return oldEnv;
}



/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   S e t M a g i c k O p e n C L E n v P a r a m                             %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  SetMagickOpenCLEnvParam() sets the parameters in the OpenCL environment
%
%  The format of the SetMagickOpenCLEnvParam() method is:
%
%      MagickBooleanType SetMagickOpenCLEnvParam(MagickCLEnv clEnv,
%        MagickOpenCLEnvParam param, size_t dataSize, void* data,
%        ExceptionInfo* exception)
%
%  A description of each parameter follows:
%
%    o clEnv: the OpenCL environment.
%
%    o param: the parameter to be set.
%
%    o dataSize: the data size of the parameter value.
%
%    o data:  the pointer to the new parameter value
%
%    o exception: return any errors or warnings
%
*/

static MagickBooleanType SetMagickOpenCLEnvParamInternal(MagickCLEnv clEnv, MagickOpenCLEnvParam param
                                          , size_t dataSize, void* data, ExceptionInfo* exception)
{
  MagickBooleanType status = MagickFalse;

  if (clEnv == NULL
    || data == NULL)
    goto cleanup;

  switch(param)
  {
  case MAGICK_OPENCL_ENV_PARAM_DEVICE:
    if (dataSize != sizeof(clEnv->device))
      goto cleanup;
    clEnv->device = *((cl_device_id*)data);
    clEnv->OpenCLInitialized = MagickFalse;
    status = MagickTrue;
    break;

  case MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED:
    if (dataSize != sizeof(clEnv->OpenCLDisabled))
      goto cleanup;
    clEnv->OpenCLDisabled =  *((MagickBooleanType*)data);
    clEnv->OpenCLInitialized = MagickFalse;
    status = MagickTrue;
    break;

  case MAGICK_OPENCL_ENV_PARAM_OPENCL_INITIALIZED:
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleWarning, "SetMagickOpenCLEnvParm cannot modify the OpenCL initialization state.", "'%s'", ".");
    break;

  case MAGICK_OPENCL_ENV_PARAM_PROGRAM_CACHE_DISABLED:
    if (dataSize != sizeof(clEnv->disableProgramCache))
      goto cleanup;
    clEnv->disableProgramCache =  *((MagickBooleanType*)data);
    clEnv->OpenCLInitialized = MagickFalse;
    status = MagickTrue;
    break;

  case MAGICK_OPENCL_ENV_PARAM_REGENERATE_PROFILE:
    if (dataSize != sizeof(clEnv->regenerateProfile))
      goto cleanup;
    clEnv->regenerateProfile =  *((MagickBooleanType*)data);
    clEnv->OpenCLInitialized = MagickFalse;
    status = MagickTrue;
    break;

  default:
    goto cleanup;
  };

cleanup:
  return status;
}

MagickExport
  MagickBooleanType SetMagickOpenCLEnvParam(MagickCLEnv clEnv, MagickOpenCLEnvParam param
                                          , size_t dataSize, void* data, ExceptionInfo* exception) {
  MagickBooleanType status = MagickFalse;
  if (clEnv!=NULL) {
    LockSemaphoreInfo(clEnv->lock);
    status = SetMagickOpenCLEnvParamInternal(clEnv,param,dataSize,data,exception);
    UnlockSemaphoreInfo(clEnv->lock);
  }
  return status;
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   G e t M a g i c k O p e n C L E n v P a r a m                             %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  GetMagickOpenCLEnvParam() gets the parameters in the OpenCL environment
%
%  The format of the GetMagickOpenCLEnvParam() method is:
%
%      MagickBooleanType GetMagickOpenCLEnvParam(MagickCLEnv clEnv,
%        MagickOpenCLEnvParam param, size_t dataSize, void* data,
%        ExceptionInfo* exception)
%
%  A description of each parameter follows:
%
%    o clEnv: the OpenCL environment.
%
%    o param: the parameter to be returned.
%
%    o dataSize: the data size of the parameter value.
%
%    o data:  the location where the returned parameter value will be stored
%
%    o exception: return any errors or warnings
%
*/

MagickExport
  MagickBooleanType GetMagickOpenCLEnvParam(MagickCLEnv clEnv, MagickOpenCLEnvParam param
                                          , size_t dataSize, void* data, ExceptionInfo* exception)
{
  MagickBooleanType
   status;

  magick_unreferenced(exception);

  status = MagickFalse;

  if (clEnv == NULL
    || data == NULL)
    goto cleanup;

  switch(param)
  {
  case MAGICK_OPENCL_ENV_PARAM_DEVICE:
    if (dataSize != sizeof(cl_device_id))
      goto cleanup;
    *((cl_device_id*)data) = clEnv->device;
    status = MagickTrue;
    break;

  case MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED:
    if (dataSize != sizeof(clEnv->OpenCLDisabled))
      goto cleanup;
    *((MagickBooleanType*)data) = clEnv->OpenCLDisabled;
    status = MagickTrue;
    break;

  case MAGICK_OPENCL_ENV_PARAM_OPENCL_INITIALIZED:
    if (dataSize != sizeof(clEnv->OpenCLDisabled))
      goto cleanup;
    *((MagickBooleanType*)data) = clEnv->OpenCLInitialized;
    status = MagickTrue;
    break;

  case MAGICK_OPENCL_ENV_PARAM_PROGRAM_CACHE_DISABLED:
    if (dataSize != sizeof(clEnv->disableProgramCache))
      goto cleanup;
    *((MagickBooleanType*)data) = clEnv->disableProgramCache;
    status = MagickTrue;
    break;

  case MAGICK_OPENCL_ENV_PARAM_REGENERATE_PROFILE:
    if (dataSize != sizeof(clEnv->regenerateProfile))
      goto cleanup;
    *((MagickBooleanType*)data) = clEnv->regenerateProfile;
    status = MagickTrue;
    break;

  default:
    goto cleanup;
  };

cleanup:
  return status;
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   G e t O p e n C L C o n t e x t                                           %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  GetOpenCLContext() returns the OpenCL context
%
%  The format of the GetOpenCLContext() method is:
%
%      cl_context GetOpenCLContext(MagickCLEnv clEnv)
%
%  A description of each parameter follows:
%
%    o clEnv: OpenCL environment
%
*/

MagickPrivate
cl_context GetOpenCLContext(MagickCLEnv clEnv) {
  if (clEnv == NULL)
    return NULL;
  else
    return clEnv->context;
}

static char* getBinaryCLProgramName(MagickCLEnv clEnv, MagickOpenCLProgram prog, unsigned int signature)
{
  char* name;
  char* ptr;
  char path[MagickPathExtent];
  char deviceName[MagickPathExtent];
  const char* prefix = "magick_opencl";
  clEnv->library->clGetDeviceInfo(clEnv->device, CL_DEVICE_NAME, MagickPathExtent, deviceName, NULL);
  ptr=deviceName;
  /* strip out illegal characters for file names */
  while (*ptr != '\0')
  {
    if ( *ptr == ' ' || *ptr == '\\' || *ptr == '/' || *ptr == ':' || *ptr == '*'
        || *ptr == '?' || *ptr == '"' || *ptr == '<' || *ptr == '>' || *ptr == '|')
    {
      *ptr = '_';
    }
    ptr++;
  }
  (void) FormatLocaleString(path,MagickPathExtent,"%s%s%s_%s_%02d_%08x_%.20g.bin",
         GetOpenCLCachedFilesDirectory(),DirectorySeparator,prefix,deviceName,
         (unsigned int) prog,signature,(double) sizeof(char*)*8);
  name = (char*)AcquireMagickMemory(strlen(path)+1);
  CopyMagickString(name,path,strlen(path)+1);
  return name;
}

static MagickBooleanType saveBinaryCLProgram(MagickCLEnv clEnv, MagickOpenCLProgram prog, unsigned int signature, ExceptionInfo* exception)
{
  MagickBooleanType saveSuccessful;
  cl_int clStatus;
  size_t binaryProgramSize;
  unsigned char* binaryProgram;
  char* binaryFileName;
  FILE* fileHandle;

#ifdef MAGICKCORE_CLPERFMARKER
  clBeginPerfMarkerAMD(__FUNCTION__,"");
#endif

  binaryProgram = NULL;
  binaryFileName = NULL;
  fileHandle = NULL;
  saveSuccessful = MagickFalse;

  clStatus = clEnv->library->clGetProgramInfo(clEnv->programs[prog], CL_PROGRAM_BINARY_SIZES, sizeof(size_t), &binaryProgramSize, NULL);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError, "clGetProgramInfo failed.", "'%s'", ".");
    goto cleanup;
  }

  binaryProgram = (unsigned char*) AcquireMagickMemory(binaryProgramSize);
  clStatus = clEnv->library->clGetProgramInfo(clEnv->programs[prog], CL_PROGRAM_BINARIES, sizeof(char*), &binaryProgram, NULL);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError, "clGetProgramInfo failed.", "'%s'", ".");
    goto cleanup;
  }

  binaryFileName = getBinaryCLProgramName(clEnv, prog, signature);
  fileHandle = fopen(binaryFileName, "wb");
  if (fileHandle != NULL)
  {
    fwrite(binaryProgram, sizeof(char), binaryProgramSize, fileHandle);
    saveSuccessful = MagickTrue;
  }
  else
  {
    (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
      "Saving binary kernel failed.", "'%s'", ".");
  }

cleanup:
  if (fileHandle != NULL)
    fclose(fileHandle);
  if (binaryProgram != NULL)
    RelinquishMagickMemory(binaryProgram);
  if (binaryFileName != NULL)
    free(binaryFileName);

#ifdef MAGICKCORE_CLPERFMARKER
  clEndPerfMarkerAMD();
#endif

  return saveSuccessful;
}

static MagickBooleanType loadBinaryCLProgram(MagickCLEnv clEnv, MagickOpenCLProgram prog, unsigned int signature)
{
  MagickBooleanType loadSuccessful;
  unsigned char* binaryProgram;
  char* binaryFileName;
  FILE* fileHandle;

#ifdef MAGICKCORE_CLPERFMARKER
  clBeginPerfMarkerAMD(__FUNCTION__,"");
#endif

  binaryProgram = NULL;
  binaryFileName = NULL;
  fileHandle = NULL;
  loadSuccessful = MagickFalse;

  binaryFileName = getBinaryCLProgramName(clEnv, prog, signature);
  fileHandle = fopen(binaryFileName, "rb");
  if (fileHandle != NULL)
  {
    int b_error;
    size_t length;
    cl_int clStatus;
    cl_int clBinaryStatus;

    b_error = 0 ;
    length = 0;
    b_error |= fseek( fileHandle, 0, SEEK_END ) < 0;
    b_error |= ( length = ftell( fileHandle ) ) <= 0;
    b_error |= fseek( fileHandle, 0, SEEK_SET ) < 0;
    if( b_error )
      goto cleanup;

    binaryProgram = (unsigned char*)AcquireMagickMemory(length);
    if (binaryProgram == NULL)
      goto cleanup;

    memset(binaryProgram, 0, length);
    b_error |= fread(binaryProgram, 1, length, fileHandle) != length;

    clEnv->programs[prog] = clEnv->library->clCreateProgramWithBinary(clEnv->context, 1, &clEnv->device, &length, (const unsigned char**)&binaryProgram, &clBinaryStatus, &clStatus);
    if (clStatus != CL_SUCCESS
        || clBinaryStatus != CL_SUCCESS)
      goto cleanup;

    loadSuccessful = MagickTrue;
  }

cleanup:
  if (fileHandle != NULL)
    fclose(fileHandle);
  if (binaryFileName != NULL)
    free(binaryFileName);
  if (binaryProgram != NULL)
    RelinquishMagickMemory(binaryProgram);

#ifdef MAGICKCORE_CLPERFMARKER
  clEndPerfMarkerAMD();
#endif

  return loadSuccessful;
}

static unsigned int stringSignature(const char* string)
{
  unsigned int stringLength;
  unsigned int n,i,j;
  unsigned int signature;
  union
  {
    const char* s;
    const unsigned int* u;
  }p;

#ifdef MAGICKCORE_CLPERFMARKER
  clBeginPerfMarkerAMD(__FUNCTION__,"");
#endif

  stringLength = (unsigned int) strlen(string);
  signature = stringLength;
  n = stringLength/sizeof(unsigned int);
  p.s = string;
  for (i = 0; i < n; i++)
  {
    signature^=p.u[i];
  }
  if (n * sizeof(unsigned int) != stringLength)
  {
    char padded[4];
    j = n * sizeof(unsigned int);
    for (i = 0; i < 4; i++,j++)
    {
      if (j < stringLength)
        padded[i] = p.s[j];
      else
        padded[i] = 0;
    }
    p.s = padded;
    signature^=p.u[0];
  }

#ifdef MAGICKCORE_CLPERFMARKER
  clEndPerfMarkerAMD();
#endif

  return signature;
}

/* OpenCL kernels for accelerate.c */
extern const char *accelerateKernels, *accelerateKernels2;

static MagickBooleanType CompileOpenCLKernels(MagickCLEnv clEnv, ExceptionInfo* exception)
{
  MagickBooleanType status = MagickFalse;
  cl_int clStatus;
  unsigned int i;
  char* accelerateKernelsBuffer = NULL;

  /* The index of the program strings in this array has to match the value of the enum MagickOpenCLProgram */
  const char* MagickOpenCLProgramStrings[MAGICK_OPENCL_NUM_PROGRAMS];

  char options[MagickPathExtent];
  unsigned int optionsSignature;

#ifdef MAGICKCORE_CLPERFMARKER
  clBeginPerfMarkerAMD(__FUNCTION__,"");
#endif

  /* Get additional options */
  (void) FormatLocaleString(options, MagickPathExtent, CLOptions, (float)QuantumRange,
    (float)QuantumScale, (float)CLCharQuantumScale, (float)MagickEpsilon, (float)MagickPI, (unsigned int)MaxMap, (unsigned int)MAGICKCORE_QUANTUM_DEPTH);

  /*
  if (getenv("MAGICK_OCL_DEF"))
  {
    strcat(options," ");
    strcat(options,getenv("MAGICK_OCL_DEF"));
  }
  */

  /*
  if (getenv("MAGICK_OCL_BUILD"))
    printf("options: %s\n", options);
  */

  optionsSignature = stringSignature(options);

  /* get all the OpenCL program strings here */
  accelerateKernelsBuffer = (char*) AcquireMagickMemory(strlen(accelerateKernels)+strlen(accelerateKernels2)+1);
  sprintf(accelerateKernelsBuffer,"%s%s",accelerateKernels,accelerateKernels2);
  MagickOpenCLProgramStrings[MAGICK_OPENCL_ACCELERATE] = accelerateKernelsBuffer;

  for (i = 0; i < MAGICK_OPENCL_NUM_PROGRAMS; i++)
  {
    MagickBooleanType loadSuccessful = MagickFalse;
    unsigned int programSignature = stringSignature(MagickOpenCLProgramStrings[i]) ^ optionsSignature;

    /* try to load the binary first */
    if (clEnv->disableProgramCache != MagickTrue
        && !getenv("MAGICK_OCL_REC"))
      loadSuccessful = loadBinaryCLProgram(clEnv, (MagickOpenCLProgram)i, programSignature);

    if (loadSuccessful == MagickFalse)
    {
      /* Binary CL program unavailable, compile the program from source */
      size_t programLength = strlen(MagickOpenCLProgramStrings[i]);
      clEnv->programs[i] = clEnv->library->clCreateProgramWithSource(clEnv->context, 1, &(MagickOpenCLProgramStrings[i]), &programLength, &clStatus);
      if (clStatus!=CL_SUCCESS)
      {
        (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
          "clCreateProgramWithSource failed.", "(%d)", (int)clStatus);

        goto cleanup;
      }
    }

    clStatus = clEnv->library->clBuildProgram(clEnv->programs[i], 1, &clEnv->device, options, NULL, NULL);
    if (clStatus!=CL_SUCCESS)
    {
      (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
        "clBuildProgram failed.", "(%d)", (int)clStatus);

      if (loadSuccessful == MagickFalse)
      {
        char path[MagickPathExtent];
        FILE* fileHandle;

        /*  dump the source into a file */
        (void) FormatLocaleString(path,MagickPathExtent,"%s%s%s"
         ,GetOpenCLCachedFilesDirectory()
         ,DirectorySeparator,"magick_badcl.cl");
        fileHandle = fopen(path, "wb");	
        if (fileHandle != NULL)
        {
          fwrite(MagickOpenCLProgramStrings[i], sizeof(char), strlen(MagickOpenCLProgramStrings[i]), fileHandle);
          fclose(fileHandle);
        }

        /* dump the build log */
        {
          char* log;
          size_t logSize;
          clEnv->library->clGetProgramBuildInfo(clEnv->programs[i], clEnv->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &logSize);
          log = (char*)AcquireMagickMemory(logSize);
          clEnv->library->clGetProgramBuildInfo(clEnv->programs[i], clEnv->device, CL_PROGRAM_BUILD_LOG, logSize, log, &logSize);

          (void) FormatLocaleString(path,MagickPathExtent,"%s%s%s"
           ,GetOpenCLCachedFilesDirectory()
           ,DirectorySeparator,"magick_badcl_build.log");
          fileHandle = fopen(path, "wb");	
          if (fileHandle != NULL)
          {
            const char* buildOptionsTitle = "build options: ";
            fwrite(buildOptionsTitle, sizeof(char), strlen(buildOptionsTitle), fileHandle);
            fwrite(options, sizeof(char), strlen(options), fileHandle);
            fwrite("\n",sizeof(char), 1, fileHandle);
            fwrite(log, sizeof(char), logSize, fileHandle);
            fclose(fileHandle);
          }
          RelinquishMagickMemory(log);
        }
      }
      goto cleanup;
    }

    if (loadSuccessful == MagickFalse)
    {
      /* Save the binary to a file to avoid re-compilation of the kernels in the future */
      saveBinaryCLProgram(clEnv, (MagickOpenCLProgram)i, programSignature, exception);
    }

  }
  status = MagickTrue;

cleanup:

  if (accelerateKernelsBuffer!=NULL) RelinquishMagickMemory(accelerateKernelsBuffer);

#ifdef MAGICKCORE_CLPERFMARKER
  clEndPerfMarkerAMD();
#endif

  return status;
}

static MagickBooleanType InitOpenCLPlatformDevice(MagickCLEnv clEnv, ExceptionInfo* exception) {
  int i,j;
  cl_int status;
  cl_uint numPlatforms = 0;
  cl_platform_id *platforms = NULL;
  char* MAGICK_OCL_DEVICE = NULL;
  MagickBooleanType OpenCLAvailable = MagickFalse;

#ifdef MAGICKCORE_CLPERFMARKER
  clBeginPerfMarkerAMD(__FUNCTION__,"");
#endif

  /* check if there's an environment variable overriding the device selection */
  MAGICK_OCL_DEVICE = getenv("MAGICK_OCL_DEVICE");
  if (MAGICK_OCL_DEVICE != NULL)
  {
    if (strcmp(MAGICK_OCL_DEVICE, "CPU") == 0)
    {
      clEnv->deviceType = CL_DEVICE_TYPE_CPU;
    }
    else if (strcmp(MAGICK_OCL_DEVICE, "GPU") == 0)
    {
      clEnv->deviceType = CL_DEVICE_TYPE_GPU;
    }
    else if (strcmp(MAGICK_OCL_DEVICE, "OFF") == 0)
    {
      /* OpenCL disabled */
      goto cleanup;
    }
  }
  else if (clEnv->deviceType == 0) {
    clEnv->deviceType = CL_DEVICE_TYPE_ALL;
  }

  if (clEnv->device != NULL)
  {
    status = clEnv->library->clGetDeviceInfo(clEnv->device, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &clEnv->platform, NULL);
    if (status != CL_SUCCESS) {
      (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
          "Failed to get OpenCL platform from the selected device.", "(%d)", status);
    }
    goto cleanup;
  }
  else if (clEnv->platform != NULL)
  {
    numPlatforms = 1;
    platforms = (cl_platform_id *) AcquireMagickMemory(numPlatforms * sizeof(cl_platform_id));
    if (platforms == (cl_platform_id *) NULL)
    {
      (void) ThrowMagickException(exception, GetMagickModule(), ResourceLimitError,
        "AcquireMagickMemory failed.",".");
      goto cleanup;
    }
    platforms[0] = clEnv->platform;
  }
  else
  {
    clEnv->device = NULL;

    /* Get the number of OpenCL platforms available */
    status = clEnv->library->clGetPlatformIDs(0, NULL, &numPlatforms);
    if (status != CL_SUCCESS)
    {
      (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
        "clGetplatformIDs failed.", "(%d)", status);
      goto cleanup;
    }

    /* No OpenCL available, just leave */
    if (numPlatforms == 0) {
      goto cleanup;
    }

    platforms = (cl_platform_id *) AcquireMagickMemory(numPlatforms * sizeof(cl_platform_id));
    if (platforms == (cl_platform_id *) NULL)
    {
      (void) ThrowMagickException(exception, GetMagickModule(), ResourceLimitError,
        "AcquireMagickMemory failed.",".");
      goto cleanup;
    }

    status = clEnv->library->clGetPlatformIDs(numPlatforms, platforms, NULL);
    if (status != CL_SUCCESS)
    {
      (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
        "clGetPlatformIDs failed.", "(%d)", status);
      goto cleanup;
    }
  }

  /* Device selection */
  clEnv->device = NULL;
  for (j = 0; j < 2; j++)
  {

    cl_device_type deviceType;
    if (clEnv->deviceType == CL_DEVICE_TYPE_ALL)
    {
      if (j == 0)
        deviceType = CL_DEVICE_TYPE_GPU;
      else
        deviceType = CL_DEVICE_TYPE_CPU;
    }
    else if (j == 1)
    {
      break;
    }
    else
      deviceType = clEnv->deviceType;

    for (i = 0; i < numPlatforms; i++)
    {
      char version[MagickPathExtent];
      cl_uint numDevices;
      status = clEnv->library->clGetPlatformInfo(clEnv->platform, CL_PLATFORM_VERSION, MagickPathExtent, version, NULL);
      if (status != CL_SUCCESS)
      {
        (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
          "clGetPlatformInfo failed.", "(%d)", status);
        goto cleanup;
      }
      if (strncmp(version,"OpenCL 1.0 ",11) == 0)
        continue;
      status = clEnv->library->clGetDeviceIDs(platforms[i], deviceType, 1, &(clEnv->device), &numDevices);
      if (status != CL_SUCCESS)
      {
        (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
          "clGetDeviceIDs failed.", "(%d)", status);
        goto cleanup;
      }
      if (clEnv->device != NULL)
      {
        clEnv->platform = platforms[i];
  goto cleanup;
      }
    }
  }

cleanup:
  if (platforms!=NULL)
    RelinquishMagickMemory(platforms);

  OpenCLAvailable = (clEnv->platform!=NULL
          && clEnv->device!=NULL)?MagickTrue:MagickFalse;

#ifdef MAGICKCORE_CLPERFMARKER
  clEndPerfMarkerAMD();
#endif

  return OpenCLAvailable;
}

static MagickBooleanType EnableOpenCLInternal(MagickCLEnv clEnv) {
  if (clEnv->OpenCLInitialized != MagickFalse
    && clEnv->platform != NULL
    && clEnv->device != NULL) {
      clEnv->OpenCLDisabled = MagickFalse;
      return MagickTrue;
  }
  clEnv->OpenCLDisabled = MagickTrue;
  return MagickFalse;
}


static MagickBooleanType autoSelectDevice(MagickCLEnv clEnv, ExceptionInfo* exception);
/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   I n i t O p e n C L E n v                                                 %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  InitOpenCLEnv() initialize the OpenCL environment
%
%  The format of the RelinquishMagickOpenCLEnv method is:
%
%      MagickBooleanType InitOpenCLEnv(MagickCLEnv clEnv, ExceptionInfo* exception)
%
%  A description of each parameter follows:
%
%    o clEnv: OpenCL environment structure
%
%    o exception: return any errors or warnings.
%
*/

MagickExport
MagickBooleanType InitOpenCLEnvInternal(MagickCLEnv clEnv, ExceptionInfo* exception) {
  MagickBooleanType status = MagickTrue;
  cl_int clStatus;
  cl_context_properties cps[3];

#ifdef MAGICKCORE_CLPERFMARKER
  {
    int status = clInitializePerfMarkerAMD();
    if (status == AP_SUCCESS) {
      //printf("PerfMarker successfully initialized\n");
    }
  }
#endif
  clEnv->OpenCLInitialized = MagickTrue;

  /* check and init the global lib */
  OpenCLLib=GetOpenCLLib();
  if (OpenCLLib)
  {
    clEnv->library=OpenCLLib;
  }
  else
  {
    /* turn off opencl */
    MagickBooleanType flag;
    flag = MagickTrue;
    SetMagickOpenCLEnvParamInternal(clEnv, MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED
        , sizeof(MagickBooleanType), &flag, exception);
  }

  if (clEnv->OpenCLDisabled != MagickFalse)
    goto cleanup;

  clEnv->OpenCLDisabled = MagickTrue;
  /* setup the OpenCL platform and device */
  status = InitOpenCLPlatformDevice(clEnv, exception);
  if (status == MagickFalse) {
    /* No OpenCL device available */
    goto cleanup;
  }

  /* create an OpenCL context */
  cps[0] = CL_CONTEXT_PLATFORM;
  cps[1] = (cl_context_properties)clEnv->platform;
  cps[2] = 0;
  clEnv->context = clEnv->library->clCreateContext(cps, 1, &(clEnv->device), NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
        "clCreateContext failed.", "(%d)", clStatus);
    status = MagickFalse;
    goto cleanup;
  }

  status = CompileOpenCLKernels(clEnv, exception);
  if (status == MagickFalse) {
   (void) ThrowMagickException(exception, GetMagickModule(), DelegateWarning,
        "clCreateCommandQueue failed.", "(%d)", status);

    status = MagickFalse;
    goto cleanup;
  }

  status = EnableOpenCLInternal(clEnv);

cleanup:
  return status;
}


MagickExport
MagickBooleanType InitOpenCLEnv(MagickCLEnv clEnv, ExceptionInfo* exception) {
  MagickBooleanType status = MagickFalse;

  if (clEnv == NULL)
    return MagickFalse;

#ifdef MAGICKCORE_CLPERFMARKER
  clBeginPerfMarkerAMD(__FUNCTION__,"");
#endif

  LockSemaphoreInfo(clEnv->lock);
  if (clEnv->OpenCLInitialized == MagickFalse) {
    if (clEnv->device==NULL
        && clEnv->OpenCLDisabled == MagickFalse)
      status = autoSelectDevice(clEnv, exception);
    else
      status = InitOpenCLEnvInternal(clEnv, exception);
  }
  UnlockSemaphoreInfo(clEnv->lock);

#ifdef MAGICKCORE_CLPERFMARKER
  clEndPerfMarkerAMD();
#endif
  return status;
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   A c q u i r e O p e n C L C o m m a n d Q u e u e                         %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  AcquireOpenCLCommandQueue() acquires an OpenCL command queue
%
%  The format of the AcquireOpenCLCommandQueue method is:
%
%      cl_command_queue AcquireOpenCLCommandQueue(MagickCLEnv clEnv)
%
%  A description of each parameter follows:
%
%    o clEnv: the OpenCL environment.
%
*/

MagickPrivate cl_command_queue AcquireOpenCLCommandQueue(MagickCLEnv clEnv)
{
  cl_command_queue
    queue;

  cl_command_queue_properties
    properties;

  if (clEnv == (MagickCLEnv) NULL)
    return (cl_command_queue) NULL;
  LockSemaphoreInfo(clEnv->commandQueuesLock);
  if (clEnv->commandQueuesPos >= 0) {
    queue=clEnv->commandQueues[clEnv->commandQueuesPos--];
    UnlockSemaphoreInfo(clEnv->commandQueuesLock);
  }
  else {
    UnlockSemaphoreInfo(clEnv->commandQueuesLock);
    properties=0;
#if PROFILE_OCL_KERNELS
    properties=CL_QUEUE_PROFILING_ENABLE;
#endif
    queue=clEnv->library->clCreateCommandQueue(clEnv->context,clEnv->device,
      properties,NULL);
  }
  return(queue);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   R e l i n q u i s h O p e n C L C o m m a n d Q u e u e                   %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  RelinquishOpenCLCommandQueue() releases the OpenCL command queue
%
%  The format of the RelinquishOpenCLCommandQueue method is:
%
%      MagickBooleanType RelinquishOpenCLCommandQueue(MagickCLEnv clEnv,
%        cl_command_queue queue)
%
%  A description of each parameter follows:
%
%    o clEnv: the OpenCL environment.
%
%    o queue: the OpenCL queue to be released.
%
%
*/

MagickPrivate MagickBooleanType RelinquishOpenCLCommandQueue(MagickCLEnv clEnv,
  cl_command_queue queue)
{
  MagickBooleanType
    status;

  if (clEnv == NULL)
    return(MagickFalse);

  LockSemaphoreInfo(clEnv->commandQueuesLock);

  if (clEnv->commandQueuesPos >= MAX_COMMAND_QUEUES)
    status=(clEnv->library->clReleaseCommandQueue(queue) == CL_SUCCESS) ?
      MagickTrue : MagickFalse;
  else
    {
      clEnv->commandQueues[++clEnv->commandQueuesPos]=queue;
      status=MagickTrue;
    }

  UnlockSemaphoreInfo(clEnv->commandQueuesLock);

  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   A c q u i r e O p e n C L K e r n e l                                     %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  AcquireOpenCLKernel() acquires an OpenCL kernel
%
%  The format of the AcquireOpenCLKernel method is:
%
%      cl_kernel AcquireOpenCLKernel(MagickCLEnv clEnv,
%        MagickOpenCLProgram program, const char* kernelName)
%
%  A description of each parameter follows:
%
%    o clEnv: the OpenCL environment.
%
%    o program: the OpenCL program module that the kernel belongs to.
%
%    o kernelName:  the name of the kernel
%
*/

MagickPrivate
  cl_kernel AcquireOpenCLKernel(MagickCLEnv clEnv, MagickOpenCLProgram program, const char* kernelName)
{
  cl_int clStatus;
  cl_kernel kernel = NULL;
  if (clEnv != NULL && kernelName!=NULL)
  {
    kernel = clEnv->library->clCreateKernel(clEnv->programs[program], kernelName, &clStatus);
  }
  return kernel;
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   R e l i n q u i s h O p e n C L K e r n e l                               %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  RelinquishOpenCLKernel() releases an OpenCL kernel
%
%  The format of the RelinquishOpenCLKernel method is:
%
%    MagickBooleanType RelinquishOpenCLKernel(MagickCLEnv clEnv,
%      cl_kernel kernel)
%
%  A description of each parameter follows:
%
%    o clEnv: the OpenCL environment.
%
%    o kernel: the OpenCL kernel object to be released.
%
%
*/

MagickPrivate
  MagickBooleanType RelinquishOpenCLKernel(MagickCLEnv clEnv, cl_kernel kernel)
{
  MagickBooleanType status = MagickFalse;
  if (clEnv != NULL && kernel != NULL)
  {
    status = ((clEnv->library->clReleaseKernel(kernel) == CL_SUCCESS)?MagickTrue:MagickFalse);
  }
  return status;
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   G e t O p e n C L D e v i c e L o c a l M e m o r y S i z e               %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  GetOpenCLDeviceLocalMemorySize() returns local memory size of the device
%
%  The format of the GetOpenCLDeviceLocalMemorySize method is:
%
%    unsigned long GetOpenCLDeviceLocalMemorySize(MagickCLEnv clEnv)
%
%  A description of each parameter follows:
%
%    o clEnv: the OpenCL environment.
%
%
*/

MagickPrivate
 unsigned long GetOpenCLDeviceLocalMemorySize(MagickCLEnv clEnv)
{
  cl_ulong localMemorySize;
  clEnv->library->clGetDeviceInfo(clEnv->device, CL_DEVICE_LOCAL_MEM_SIZE, sizeof(cl_ulong), &localMemorySize, NULL);
  return (unsigned long)localMemorySize;
}

MagickPrivate
  unsigned long GetOpenCLDeviceMaxMemAllocSize(MagickCLEnv clEnv)
{
  cl_ulong maxMemAllocSize;
  clEnv->library->clGetDeviceInfo(clEnv->device, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(cl_ulong), &maxMemAllocSize, NULL);
  return (unsigned long)maxMemAllocSize;
}


/*
 Beginning of the OpenCL device selection infrastructure
*/


typedef enum {
  DS_SUCCESS = 0
 ,DS_INVALID_PROFILE = 1000
 ,DS_MEMORY_ERROR
 ,DS_INVALID_PERF_EVALUATOR_TYPE
 ,DS_INVALID_PERF_EVALUATOR
 ,DS_PERF_EVALUATOR_ERROR
 ,DS_FILE_ERROR
 ,DS_UNKNOWN_DEVICE_TYPE
 ,DS_PROFILE_FILE_ERROR
 ,DS_SCORE_SERIALIZER_ERROR
 ,DS_SCORE_DESERIALIZER_ERROR
} ds_status;

/* device type */
typedef enum {
  DS_DEVICE_NATIVE_CPU = 0
 ,DS_DEVICE_OPENCL_DEVICE
} ds_device_type;


typedef struct {
  ds_device_type  type;
  cl_device_type  oclDeviceType;
  cl_device_id    oclDeviceID;
  char*           oclDeviceName;
  char*           oclDriverVersion;
  cl_uint         oclMaxClockFrequency;
  cl_uint         oclMaxComputeUnits;
  void*           score;            /* a pointer to the score data, the content/format is application defined */
} ds_device;

typedef struct {
  unsigned int  numDevices;
  ds_device*    devices;
  const char*   version;
} ds_profile;

/* deallocate memory used by score */
typedef ds_status (*ds_score_release)(void* score);

static ds_status releaseDeviceResource(ds_device* device, ds_score_release sr) {
  ds_status status = DS_SUCCESS;
  if (device) {
    if (device->oclDeviceName)      free(device->oclDeviceName);
    if (device->oclDriverVersion)   free(device->oclDriverVersion);
    if (device->score)              status = sr(device->score);
  }
  return status;
}

static ds_status releaseDSProfile(ds_profile* profile, ds_score_release sr) {
  ds_status status = DS_SUCCESS;
  if (profile!=NULL) {
    if (profile->devices!=NULL && sr!=NULL) {
      unsigned int i;
      for (i = 0; i < profile->numDevices; i++) {
        status = releaseDeviceResource(profile->devices+i,sr);
        if (status != DS_SUCCESS)
          break;
      }
      free(profile->devices);
    }
    free(profile);
  }
  return status;
}


static ds_status initDSProfile(ds_profile** p, const char* version) {
  int numDevices = 0;
  cl_uint numPlatforms = 0;
  cl_platform_id* platforms = NULL;
  cl_device_id*   devices = NULL;
  ds_status status = DS_SUCCESS;
  ds_profile* profile = NULL;
  unsigned int next = 0;
  unsigned int i;

  if (p == NULL)
    return DS_INVALID_PROFILE;

  profile = (ds_profile*)malloc(sizeof(ds_profile));
  if (profile == NULL)
    return DS_MEMORY_ERROR;

  memset(profile, 0, sizeof(ds_profile));

  OpenCLLib->clGetPlatformIDs(0, NULL, &numPlatforms);
  if (numPlatforms > 0) {
    platforms = (cl_platform_id*)malloc(numPlatforms*sizeof(cl_platform_id));
    if (platforms == NULL) {
      status = DS_MEMORY_ERROR;
      goto cleanup;
    }
    OpenCLLib->clGetPlatformIDs(numPlatforms, platforms, NULL);
    for (i = 0; i < (unsigned int)numPlatforms; i++) {
      cl_uint num;
      if (OpenCLLib->clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_CPU | CL_DEVICE_TYPE_GPU, 0, NULL, &num) == CL_SUCCESS)
        numDevices+=num;
    }
  }

  profile->numDevices = numDevices+1;     /* +1 to numDevices to include the native CPU */

  profile->devices = (ds_device*)malloc(profile->numDevices*sizeof(ds_device));
  if (profile->devices == NULL) {
    profile->numDevices = 0;
    status = DS_MEMORY_ERROR;
    goto cleanup;
  }
  memset(profile->devices, 0, profile->numDevices*sizeof(ds_device));

  if (numDevices > 0) {
    devices = (cl_device_id*)malloc(numDevices*sizeof(cl_device_id));
    if (devices == NULL) {
      status = DS_MEMORY_ERROR;
      goto cleanup;
    }
    for (i = 0; i < (unsigned int)numPlatforms; i++) {
      cl_uint num;

      int d;
      for (d = 0; d < 2; d++) {
        unsigned int j;
        cl_device_type deviceType;
        switch(d) {
        case 0:
          deviceType = CL_DEVICE_TYPE_GPU;
          break;
        case 1:
          deviceType = CL_DEVICE_TYPE_CPU;
          break;
        default:
          continue;
          break;
        }
        if (OpenCLLib->clGetDeviceIDs(platforms[i], deviceType, numDevices, devices, &num) != CL_SUCCESS)
          continue;
        for (j = 0; j < num; j++, next++) {
          size_t length;

          profile->devices[next].type = DS_DEVICE_OPENCL_DEVICE;
          profile->devices[next].oclDeviceID = devices[j];

          OpenCLLib->clGetDeviceInfo(profile->devices[next].oclDeviceID, CL_DEVICE_NAME
            , 0, NULL, &length);
          profile->devices[next].oclDeviceName = (char*)malloc(sizeof(char)*length);
          OpenCLLib->clGetDeviceInfo(profile->devices[next].oclDeviceID, CL_DEVICE_NAME
            , length, profile->devices[next].oclDeviceName, NULL);

          OpenCLLib->clGetDeviceInfo(profile->devices[next].oclDeviceID, CL_DRIVER_VERSION
            , 0, NULL, &length);
          profile->devices[next].oclDriverVersion = (char*)malloc(sizeof(char)*length);
          OpenCLLib->clGetDeviceInfo(profile->devices[next].oclDeviceID, CL_DRIVER_VERSION
            , length, profile->devices[next].oclDriverVersion, NULL);

          OpenCLLib->clGetDeviceInfo(profile->devices[next].oclDeviceID, CL_DEVICE_MAX_CLOCK_FREQUENCY
            , sizeof(cl_uint), &profile->devices[next].oclMaxClockFrequency, NULL);

          OpenCLLib->clGetDeviceInfo(profile->devices[next].oclDeviceID, CL_DEVICE_MAX_COMPUTE_UNITS
            , sizeof(cl_uint), &profile->devices[next].oclMaxComputeUnits, NULL);

          OpenCLLib->clGetDeviceInfo(profile->devices[next].oclDeviceID, CL_DEVICE_TYPE
            , sizeof(cl_device_type), &profile->devices[next].oclDeviceType, NULL);
        }
      }
    }
  }

  profile->devices[next].type = DS_DEVICE_NATIVE_CPU;
  profile->version = version;

cleanup:
  if (platforms)  free(platforms);
  if (devices)    free(devices);
  if (status == DS_SUCCESS) {
    *p = profile;
  }
  else {
    if (profile) {
      if (profile->devices)
        free(profile->devices);
      free(profile);
    }
  }
  return status;
}

/* Pointer to a function that calculates the score of a device (ex: device->score)
 update the data size of score. The encoding and the format of the score data
 is implementation defined. The function should return DS_SUCCESS if there's no error to be reported.
 */
typedef ds_status (*ds_perf_evaluator)(ds_device* device, void* data);

typedef enum {
  DS_EVALUATE_ALL
  ,DS_EVALUATE_NEW_ONLY
} ds_evaluation_type;

static ds_status profileDevices(ds_profile* profile, const ds_evaluation_type type
                         ,ds_perf_evaluator evaluator, void* evaluatorData, unsigned int* numUpdates) {
  ds_status status = DS_SUCCESS;
  unsigned int i;
  unsigned int updates = 0;

  if (profile == NULL) {
    return DS_INVALID_PROFILE;
  }
  if (evaluator == NULL) {
    return DS_INVALID_PERF_EVALUATOR;
  }

  for (i = 0; i < profile->numDevices; i++) {
    ds_status evaluatorStatus;

    switch (type) {
    case DS_EVALUATE_NEW_ONLY:
      if (profile->devices[i].score != NULL)
        break;
      /*  else fall through */
    case DS_EVALUATE_ALL:
      evaluatorStatus = evaluator(profile->devices+i,evaluatorData);
      if (evaluatorStatus != DS_SUCCESS) {
        status = evaluatorStatus;
        return status;
      }
      updates++;
      break;
    default:
      return DS_INVALID_PERF_EVALUATOR_TYPE;
      break;
    };
  }
  if (numUpdates)
    *numUpdates = updates;
  return status;
}


#define DS_TAG_VERSION                      "<version>"
#define DS_TAG_VERSION_END                  "</version>"
#define DS_TAG_DEVICE                       "<device>"
#define DS_TAG_DEVICE_END                   "</device>"
#define DS_TAG_SCORE                        "<score>"
#define DS_TAG_SCORE_END                    "</score>"
#define DS_TAG_DEVICE_TYPE                  "<type>"
#define DS_TAG_DEVICE_TYPE_END              "</type>"
#define DS_TAG_DEVICE_NAME                  "<name>"
#define DS_TAG_DEVICE_NAME_END              "</name>"
#define DS_TAG_DEVICE_DRIVER_VERSION        "<driver>"
#define DS_TAG_DEVICE_DRIVER_VERSION_END    "</driver>"
#define DS_TAG_DEVICE_MAX_COMPUTE_UNITS     "<max cu>"
#define DS_TAG_DEVICE_MAX_COMPUTE_UNITS_END "</max cu>"
#define DS_TAG_DEVICE_MAX_CLOCK_FREQ        "<max clock>"
#define DS_TAG_DEVICE_MAX_CLOCK_FREQ_END    "</max clock>"

#define DS_DEVICE_NATIVE_CPU_STRING  "native_cpu"



typedef ds_status (*ds_score_serializer)(ds_device* device, void** serializedScore, unsigned int* serializedScoreSize);
static ds_status writeProfileToFile(ds_profile* profile, ds_score_serializer serializer, const char* file) {
  ds_status status = DS_SUCCESS;
  FILE* profileFile = NULL;


  if (profile == NULL)
    return DS_INVALID_PROFILE;

  profileFile = fopen(file, "wb");
  if (profileFile==NULL) {
    status = DS_FILE_ERROR;
  }
  else {
    unsigned int i;

    /* write version string */
    fwrite(DS_TAG_VERSION, sizeof(char), strlen(DS_TAG_VERSION), profileFile);
    fwrite(profile->version, sizeof(char), strlen(profile->version), profileFile);
    fwrite(DS_TAG_VERSION_END, sizeof(char), strlen(DS_TAG_VERSION_END), profileFile);
    fwrite("\n", sizeof(char), 1, profileFile);

    for (i = 0; i < profile->numDevices && status == DS_SUCCESS; i++) {
      void* serializedScore;
      unsigned int serializedScoreSize;

      fwrite(DS_TAG_DEVICE, sizeof(char), strlen(DS_TAG_DEVICE), profileFile);

      fwrite(DS_TAG_DEVICE_TYPE, sizeof(char), strlen(DS_TAG_DEVICE_TYPE), profileFile);
      fwrite(&profile->devices[i].type,sizeof(ds_device_type),1, profileFile);
      fwrite(DS_TAG_DEVICE_TYPE_END, sizeof(char), strlen(DS_TAG_DEVICE_TYPE_END), profileFile);

      switch(profile->devices[i].type) {
      case DS_DEVICE_NATIVE_CPU:
        {
          /* There's no need to emit a device name for the native CPU device. */
          /*
          fwrite(DS_TAG_DEVICE_NAME, sizeof(char), strlen(DS_TAG_DEVICE_NAME), profileFile);
          fwrite(DS_DEVICE_NATIVE_CPU_STRING,sizeof(char),strlen(DS_DEVICE_NATIVE_CPU_STRING), profileFile);
          fwrite(DS_TAG_DEVICE_NAME_END, sizeof(char), strlen(DS_TAG_DEVICE_NAME_END), profileFile);
          */
        }
        break;
      case DS_DEVICE_OPENCL_DEVICE:
        {
          char tmp[16];

          fwrite(DS_TAG_DEVICE_NAME, sizeof(char), strlen(DS_TAG_DEVICE_NAME), profileFile);
          fwrite(profile->devices[i].oclDeviceName,sizeof(char),strlen(profile->devices[i].oclDeviceName), profileFile);
          fwrite(DS_TAG_DEVICE_NAME_END, sizeof(char), strlen(DS_TAG_DEVICE_NAME_END), profileFile);

          fwrite(DS_TAG_DEVICE_DRIVER_VERSION, sizeof(char), strlen(DS_TAG_DEVICE_DRIVER_VERSION), profileFile);
          fwrite(profile->devices[i].oclDriverVersion,sizeof(char),strlen(profile->devices[i].oclDriverVersion), profileFile);
          fwrite(DS_TAG_DEVICE_DRIVER_VERSION_END, sizeof(char), strlen(DS_TAG_DEVICE_DRIVER_VERSION_END), profileFile);

          fwrite(DS_TAG_DEVICE_MAX_COMPUTE_UNITS, sizeof(char), strlen(DS_TAG_DEVICE_MAX_COMPUTE_UNITS), profileFile);
          sprintf(tmp,"%d",profile->devices[i].oclMaxComputeUnits);
          fwrite(tmp,sizeof(char),strlen(tmp), profileFile);
          fwrite(DS_TAG_DEVICE_MAX_COMPUTE_UNITS_END, sizeof(char), strlen(DS_TAG_DEVICE_MAX_COMPUTE_UNITS_END), profileFile);

          fwrite(DS_TAG_DEVICE_MAX_CLOCK_FREQ, sizeof(char), strlen(DS_TAG_DEVICE_MAX_CLOCK_FREQ), profileFile);
          sprintf(tmp,"%d",profile->devices[i].oclMaxClockFrequency);
          fwrite(tmp,sizeof(char),strlen(tmp), profileFile);
          fwrite(DS_TAG_DEVICE_MAX_CLOCK_FREQ_END, sizeof(char), strlen(DS_TAG_DEVICE_MAX_CLOCK_FREQ_END), profileFile);
        }
        break;
      default:
        status = DS_UNKNOWN_DEVICE_TYPE;
        break;
      };

      fwrite(DS_TAG_SCORE, sizeof(char), strlen(DS_TAG_SCORE), profileFile);
      status = serializer(profile->devices+i, &serializedScore, &serializedScoreSize);
      if (status == DS_SUCCESS && serializedScore!=NULL && serializedScoreSize > 0) {
        fwrite(serializedScore, sizeof(char), serializedScoreSize, profileFile);
        free(serializedScore);
      }
      fwrite(DS_TAG_SCORE_END, sizeof(char), strlen(DS_TAG_SCORE_END), profileFile);
      fwrite(DS_TAG_DEVICE_END, sizeof(char), strlen(DS_TAG_DEVICE_END), profileFile);
      fwrite("\n",sizeof(char),1,profileFile);
    }
    fclose(profileFile);
  }
  return status;
}


static ds_status readProFile(const char* fileName, char** content, size_t* contentSize) {
  ds_status status = DS_SUCCESS;
  FILE * input = NULL;
  size_t size = 0;
  size_t rsize = 0;
  char* binary = NULL;

  *contentSize = 0;
  *content = NULL;

  input = fopen(fileName, "rb");
  if(input == NULL) {
    return DS_FILE_ERROR;
  }

  fseek(input, 0L, SEEK_END);
  size = ftell(input);
  rewind(input);
  binary = (char*)malloc(size);
  if(binary == NULL) {
    status = DS_FILE_ERROR;
    goto cleanup;
  }
  rsize = fread(binary, sizeof(char), size, input);
  if (rsize!=size
      || ferror(input)) {
    status = DS_FILE_ERROR;
    goto cleanup;
  }
  *contentSize = size;
  *content = binary;

cleanup:
  if (input != NULL) fclose(input);
  if (status != DS_SUCCESS
      && binary != NULL) {
      free(binary);
      *content = NULL;
      *contentSize = 0;
  }
  return status;
}


static const char* findString(const char* contentStart, const char* contentEnd, const char* string) {
  size_t stringLength;
  const char* currentPosition;
  const char* found;
  found = NULL;
  stringLength = strlen(string);
  currentPosition = contentStart;
  for(currentPosition = contentStart; currentPosition < contentEnd; currentPosition++) {
    if (*currentPosition == string[0]) {
      if (currentPosition+stringLength < contentEnd) {
        if (strncmp(currentPosition, string, stringLength) == 0) {
          found = currentPosition;
          break;
        }
      }
    }
  }
  return found;
}


typedef ds_status (*ds_score_deserializer)(ds_device* device, const unsigned char* serializedScore, unsigned int serializedScoreSize);
static ds_status readProfileFromFile(ds_profile* profile, ds_score_deserializer deserializer, const char* file) {

  ds_status status = DS_SUCCESS;
  char* contentStart = NULL;
  const char* contentEnd = NULL;
  size_t contentSize;

  if (profile==NULL)
    return DS_INVALID_PROFILE;

  status = readProFile(file, &contentStart, &contentSize);
  if (status == DS_SUCCESS) {
    const char* currentPosition;
    const char* dataStart;
    const char* dataEnd;
    size_t versionStringLength;

    contentEnd = contentStart + contentSize;
    currentPosition = contentStart;


    /* parse the version string */
    dataStart = findString(currentPosition, contentEnd, DS_TAG_VERSION);
    if (dataStart == NULL) {
      status = DS_PROFILE_FILE_ERROR;
      goto cleanup;
    }
    dataStart += strlen(DS_TAG_VERSION);

    dataEnd = findString(dataStart, contentEnd, DS_TAG_VERSION_END);
    if (dataEnd==NULL) {
      status = DS_PROFILE_FILE_ERROR;
      goto cleanup;
    }

    versionStringLength = strlen(profile->version);
    if (versionStringLength!=(size_t)(dataEnd-dataStart)
        || strncmp(profile->version, dataStart, versionStringLength)!=(int)0) {
      /* version mismatch */
      status = DS_PROFILE_FILE_ERROR;
      goto cleanup;
    }
    currentPosition = dataEnd+strlen(DS_TAG_VERSION_END);

    /* parse the device information */
DisableMSCWarning(4127)
    while (1) {
RestoreMSCWarning
      unsigned int i;

      const char* deviceTypeStart;
      const char* deviceTypeEnd;
      ds_device_type deviceType;

      const char* deviceNameStart;
      const char* deviceNameEnd;

      const char* deviceScoreStart;
      const char* deviceScoreEnd;

      const char* deviceDriverStart;
      const char* deviceDriverEnd;

      const char* tmpStart;
      const char* tmpEnd;
      char tmp[16];

      cl_uint maxClockFrequency;
      cl_uint maxComputeUnits;

      dataStart = findString(currentPosition, contentEnd, DS_TAG_DEVICE);
      if (dataStart==NULL) {
        /* nothing useful remain, quit...*/
        break;
      }
      dataStart+=strlen(DS_TAG_DEVICE);
      dataEnd = findString(dataStart, contentEnd, DS_TAG_DEVICE_END);
      if (dataEnd==NULL) {
        status = DS_PROFILE_FILE_ERROR;
        goto cleanup;
      }

      /* parse the device type */
      deviceTypeStart = findString(dataStart, contentEnd, DS_TAG_DEVICE_TYPE);
      if (deviceTypeStart==NULL) {
        status = DS_PROFILE_FILE_ERROR;
        goto cleanup;
      }
      deviceTypeStart+=strlen(DS_TAG_DEVICE_TYPE);
      deviceTypeEnd = findString(deviceTypeStart, contentEnd, DS_TAG_DEVICE_TYPE_END);
      if (deviceTypeEnd==NULL) {
        status = DS_PROFILE_FILE_ERROR;
        goto cleanup;
      }
      memcpy(&deviceType, deviceTypeStart, sizeof(ds_device_type));


      /* parse the device name */
      if (deviceType == DS_DEVICE_OPENCL_DEVICE) {

        deviceNameStart = findString(dataStart, contentEnd, DS_TAG_DEVICE_NAME);
        if (deviceNameStart==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }
        deviceNameStart+=strlen(DS_TAG_DEVICE_NAME);
        deviceNameEnd = findString(deviceNameStart, contentEnd, DS_TAG_DEVICE_NAME_END);
        if (deviceNameEnd==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }


        deviceDriverStart = findString(dataStart, contentEnd, DS_TAG_DEVICE_DRIVER_VERSION);
        if (deviceDriverStart==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }
        deviceDriverStart+=strlen(DS_TAG_DEVICE_DRIVER_VERSION);
        deviceDriverEnd = findString(deviceDriverStart, contentEnd, DS_TAG_DEVICE_DRIVER_VERSION_END);
        if (deviceDriverEnd ==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }


        tmpStart = findString(dataStart, contentEnd, DS_TAG_DEVICE_MAX_COMPUTE_UNITS);
        if (tmpStart==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }
        tmpStart+=strlen(DS_TAG_DEVICE_MAX_COMPUTE_UNITS);
        tmpEnd = findString(tmpStart, contentEnd, DS_TAG_DEVICE_MAX_COMPUTE_UNITS_END);
        if (tmpEnd ==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }
        memcpy(tmp,tmpStart,tmpEnd-tmpStart);
        tmp[tmpEnd-tmpStart] = '\0';
        maxComputeUnits = strtol(tmp,(char **) NULL,10);


        tmpStart = findString(dataStart, contentEnd, DS_TAG_DEVICE_MAX_CLOCK_FREQ);
        if (tmpStart==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }
        tmpStart+=strlen(DS_TAG_DEVICE_MAX_CLOCK_FREQ);
        tmpEnd = findString(tmpStart, contentEnd, DS_TAG_DEVICE_MAX_CLOCK_FREQ_END);
        if (tmpEnd ==NULL) {
          status = DS_PROFILE_FILE_ERROR;
          goto cleanup;
        }
        memcpy(tmp,tmpStart,tmpEnd-tmpStart);
        tmp[tmpEnd-tmpStart] = '\0';
        maxClockFrequency = strtol(tmp,(char **) NULL,10);


        /* check if this device is on the system */
        for (i = 0; i < profile->numDevices; i++) {
          if (profile->devices[i].type == DS_DEVICE_OPENCL_DEVICE) {
            size_t actualDeviceNameLength;
            size_t driverVersionLength;

            actualDeviceNameLength = strlen(profile->devices[i].oclDeviceName);
            driverVersionLength = strlen(profile->devices[i].oclDriverVersion);
            if (actualDeviceNameLength == (size_t)(deviceNameEnd - deviceNameStart)
               && driverVersionLength == (size_t)(deviceDriverEnd - deviceDriverStart)
               && maxComputeUnits == profile->devices[i].oclMaxComputeUnits
               && maxClockFrequency == profile->devices[i].oclMaxClockFrequency
               && strncmp(profile->devices[i].oclDeviceName, deviceNameStart, actualDeviceNameLength)==(int)0
               && strncmp(profile->devices[i].oclDriverVersion, deviceDriverStart, driverVersionLength)==(int)0) {

              deviceScoreStart = findString(dataStart, contentEnd, DS_TAG_SCORE);
              if (deviceNameStart==NULL) {
                status = DS_PROFILE_FILE_ERROR;
                goto cleanup;
              }
              deviceScoreStart+=strlen(DS_TAG_SCORE);
              deviceScoreEnd = findString(deviceScoreStart, contentEnd, DS_TAG_SCORE_END);
              status = deserializer(profile->devices+i, (const unsigned char*)deviceScoreStart, deviceScoreEnd-deviceScoreStart);
              if (status != DS_SUCCESS) {
                goto cleanup;
              }
            }
          }
        }

      }
      else if (deviceType == DS_DEVICE_NATIVE_CPU) {
        for (i = 0; i < profile->numDevices; i++) {
          if (profile->devices[i].type == DS_DEVICE_NATIVE_CPU) {
            deviceScoreStart = findString(dataStart, contentEnd, DS_TAG_SCORE);
            if (deviceScoreStart==NULL) {
              status = DS_PROFILE_FILE_ERROR;
              goto cleanup;
            }
            deviceScoreStart+=strlen(DS_TAG_SCORE);
            deviceScoreEnd = findString(deviceScoreStart, contentEnd, DS_TAG_SCORE_END);
            status = deserializer(profile->devices+i, (const unsigned char*)deviceScoreStart, deviceScoreEnd-deviceScoreStart);
            if (status != DS_SUCCESS) {
              goto cleanup;
            }
          }
        }
      }

      /* skip over the current one to find the next device */
      currentPosition = dataEnd+strlen(DS_TAG_DEVICE_END);
    }
  }
cleanup:
  if (contentStart!=NULL) free(contentStart);
  return status;
}


#if 0
static ds_status getNumDeviceWithEmptyScore(ds_profile* profile, unsigned int* num) {
  unsigned int i;
  if (profile == NULL || num==NULL)
    return DS_MEMORY_ERROR;
  *num=0;
  for (i = 0; i < profile->numDevices; i++) {
    if (profile->devices[i].score == NULL) {
      (*num)++;
    }
  }
  return DS_SUCCESS;
}
#endif

/*
 End of the OpenCL device selection infrastructure
*/


typedef double AccelerateScoreType;

static ds_status AcceleratePerfEvaluator(ds_device *device,
  void *magick_unused(data))
{
#define ACCELERATE_PERF_DIMEN "2048x1536"
#define NUM_ITER  2
#define ReturnStatus(status) \
{ \
  if (clEnv!=NULL) \
    RelinquishMagickOpenCLEnv(clEnv); \
  if (oldClEnv!=NULL) \
    defaultCLEnv = oldClEnv; \
  return status; \
}

  AccelerateTimer
    timer;

  ExceptionInfo
    *exception=NULL;

  MagickCLEnv
    clEnv=NULL,
    oldClEnv=NULL;

  magick_unreferenced(data);

  if (device == NULL)
    ReturnStatus(DS_PERF_EVALUATOR_ERROR);

  clEnv=AcquireMagickOpenCLEnv();
  exception=AcquireExceptionInfo();

  if (device->type == DS_DEVICE_NATIVE_CPU)
    {
      /* CPU device */
      MagickBooleanType flag=MagickTrue;
      SetMagickOpenCLEnvParamInternal(clEnv,
        MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED,sizeof(MagickBooleanType),
        &flag,exception);
    }
  else if (device->type == DS_DEVICE_OPENCL_DEVICE)
    {
      /* OpenCL device */
      SetMagickOpenCLEnvParamInternal(clEnv,MAGICK_OPENCL_ENV_PARAM_DEVICE,
        sizeof(cl_device_id),&device->oclDeviceID,exception);
    }
  else
    ReturnStatus(DS_PERF_EVALUATOR_ERROR);

  /* recompile the OpenCL kernels if it needs to */
  clEnv->disableProgramCache = defaultCLEnv->disableProgramCache;

  InitOpenCLEnvInternal(clEnv,exception);
  oldClEnv=defaultCLEnv;
  defaultCLEnv=clEnv;

  /* microbenchmark */
  {
    Image
      *inputImage;

    ImageInfo
      *imageInfo;

    int
      i;

    imageInfo=AcquireImageInfo();
    CloneString(&imageInfo->size,ACCELERATE_PERF_DIMEN);
    CopyMagickString(imageInfo->filename,"xc:none",MagickPathExtent);
    inputImage=ReadImage(imageInfo,exception);

    initAccelerateTimer(&timer);

    for (i=0; i<=NUM_ITER; i++)
    {
      Image
        *bluredImage,
        *resizedImage,
        *unsharpedImage;

      if (i > 0)
        startAccelerateTimer(&timer);

#ifdef MAGICKCORE_CLPERFMARKER
      clBeginPerfMarkerAMD("PerfEvaluatorRegion","");
#endif

      bluredImage=BlurImage(inputImage,10.0f,3.5f,exception);
      unsharpedImage=UnsharpMaskImage(bluredImage,2.0f,2.0f,50.0f,10.0f,
        exception);
      resizedImage=ResizeImage(unsharpedImage,640,480,LanczosFilter,
        exception);

#ifdef MAGICKCORE_CLPERFMARKER
      clEndPerfMarkerAMD();
#endif

      if (i > 0)
        stopAccelerateTimer(&timer);

      if (bluredImage)
        DestroyImage(bluredImage);
      if (unsharpedImage)
        DestroyImage(unsharpedImage);
      if (resizedImage)
        DestroyImage(resizedImage);
    }
    DestroyImage(inputImage);
  }
  /* end of microbenchmark */

  if (device->score == NULL)
    device->score=malloc(sizeof(AccelerateScoreType));
  *(AccelerateScoreType*)device->score=readAccelerateTimer(&timer);

  ReturnStatus(DS_SUCCESS);
}

ds_status AccelerateScoreSerializer(ds_device* device, void** serializedScore, unsigned int* serializedScoreSize) {
  if (device
     && device->score) {
    /* generate a string from the score */
    char* s = (char*)malloc(sizeof(char)*256);
    sprintf(s,"%.4f",*((AccelerateScoreType*)device->score));
    *serializedScore = (void*)s;
    *serializedScoreSize = (unsigned int) strlen(s);
    return DS_SUCCESS;
  }
  else {
    return DS_SCORE_SERIALIZER_ERROR;
  }
}

ds_status AccelerateScoreDeserializer(ds_device* device, const unsigned char* serializedScore, unsigned int serializedScoreSize) {
  if (device) {
    /* convert the string back to an int */
    char* s = (char*)malloc(serializedScoreSize+1);
    memcpy(s, serializedScore, serializedScoreSize);
    s[serializedScoreSize] = (char)'\0';
    device->score = malloc(sizeof(AccelerateScoreType));
    *((AccelerateScoreType*)device->score) = (AccelerateScoreType)
       strtod(s,(char **) NULL);
    free(s);
    return DS_SUCCESS;
  }
  else {
    return DS_SCORE_DESERIALIZER_ERROR;
  }
}

ds_status AccelerateScoreRelease(void* score) {
  if (score!=NULL) {
    free(score);
  }
  return DS_SUCCESS;
}

ds_status canWriteProfileToFile(const char *path)
{
  FILE* profileFile = fopen(path, "ab");

  if (profileFile==NULL)
    return DS_FILE_ERROR;

  fclose(profileFile);
  return DS_SUCCESS;
}

#define IMAGEMAGICK_PROFILE_VERSION "ImageMagick Device Selection v0.9"
#define IMAGEMAGICK_PROFILE_FILE    "ImagemagickOpenCLDeviceProfile"
static MagickBooleanType autoSelectDevice(MagickCLEnv clEnv, ExceptionInfo* exception) {

  MagickBooleanType mStatus = MagickFalse;
  ds_status status;
  ds_profile* profile;
  unsigned int numDeviceProfiled = 0;
  unsigned int i;
  unsigned int bestDeviceIndex;
  AccelerateScoreType bestScore;
  char path[MagickPathExtent];
  MagickBooleanType flag;
  ds_evaluation_type profileType;

  LockDefaultOpenCLEnv();

  /* Initially, just set OpenCL to off */
  flag = MagickTrue;
  SetMagickOpenCLEnvParamInternal(clEnv, MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED
    , sizeof(MagickBooleanType), &flag, exception);

  /* check and init the global lib */
  OpenCLLib=GetOpenCLLib();
  if (OpenCLLib==NULL)
  {
    mStatus=InitOpenCLEnvInternal(clEnv, exception);
    goto cleanup;
  }

  status = initDSProfile(&profile, IMAGEMAGICK_PROFILE_VERSION);
  if (status!=DS_SUCCESS) {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError, "Error when initializing the profile", "'%s'", ".");
    goto cleanup;
  }

  (void) FormatLocaleString(path,MagickPathExtent,"%s%s%s"
         ,GetOpenCLCachedFilesDirectory()
         ,DirectorySeparator,IMAGEMAGICK_PROFILE_FILE);

  if (canWriteProfileToFile(path) != DS_SUCCESS) {
    /* We can not write out a device profile, so don't run the benchmark */
    /* select the first GPU device */

    bestDeviceIndex = 0;
    for (i = 1; i < profile->numDevices; i++) {
      if ((profile->devices[i].type == DS_DEVICE_OPENCL_DEVICE) && (profile->devices[i].oclDeviceType == CL_DEVICE_TYPE_GPU)) {
        bestDeviceIndex = i;
        break;
      }
    }
  }
  else {
    if (clEnv->regenerateProfile != MagickFalse) {
      profileType = DS_EVALUATE_ALL;
    }
    else {
      readProfileFromFile(profile, AccelerateScoreDeserializer, path);
      profileType = DS_EVALUATE_NEW_ONLY;
    }
    status = profileDevices(profile, profileType, AcceleratePerfEvaluator, NULL, &numDeviceProfiled);

    if (status!=DS_SUCCESS) {
      (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError, "Error when initializing the profile", "'%s'", ".");
      goto cleanup;
    }
    if (numDeviceProfiled > 0) {
      status = writeProfileToFile(profile, AccelerateScoreSerializer, path);
      if (status!=DS_SUCCESS) {
        (void) ThrowMagickException(exception, GetMagickModule(), ModuleWarning, "Error when saving the profile into a file", "'%s'", ".");
      }
    }

    /* pick the best device */
    bestDeviceIndex = 0;
    bestScore = *(AccelerateScoreType*)profile->devices[bestDeviceIndex].score;
    for (i = 1; i < profile->numDevices; i++) {
      AccelerateScoreType score = *(AccelerateScoreType*)profile->devices[i].score;
      if (score < bestScore) {
        bestDeviceIndex = i;
        bestScore = score;
      }
    }
  }

  /* set up clEnv with the best device */
  if (profile->devices[bestDeviceIndex].type == DS_DEVICE_NATIVE_CPU) {
    /* CPU device */
    flag = MagickTrue;
    SetMagickOpenCLEnvParamInternal(clEnv, MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED
                                  , sizeof(MagickBooleanType), &flag, exception);
  }
  else if (profile->devices[bestDeviceIndex].type == DS_DEVICE_OPENCL_DEVICE) {
    /* OpenCL device */
    flag = MagickFalse;
    SetMagickOpenCLEnvParamInternal(clEnv, MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED
      , sizeof(MagickBooleanType), &flag, exception);
    SetMagickOpenCLEnvParamInternal(clEnv, MAGICK_OPENCL_ENV_PARAM_DEVICE
      , sizeof(cl_device_id), &profile->devices[bestDeviceIndex].oclDeviceID,exception);
  }
  else {
    status = DS_PERF_EVALUATOR_ERROR;
    goto cleanup;
  }
  mStatus=InitOpenCLEnvInternal(clEnv, exception);

  status = releaseDSProfile(profile, AccelerateScoreRelease);
  if (status!=DS_SUCCESS) {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleWarning, "Error when releasing the profile", "'%s'", ".");
  }

cleanup:

  UnlockDefaultOpenCLEnv();
  return mStatus;
}


/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
+   I n i t I m a g e M a g i c k O p e n C L                                 %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%  InitImageMagickOpenCL() provides a simplified interface to initialize
%  the OpenCL environtment in ImageMagick
%
%  The format of the InitImageMagickOpenCL() method is:
%
%      MagickBooleanType InitImageMagickOpenCL(ImageMagickOpenCLMode mode,
%                                        void* userSelectedDevice,
%                                        void* selectedDevice)
%
%  A description of each parameter follows:
%
%    o mode: OpenCL mode in ImageMagick, could be off,auto,user
%
%    o userSelectedDevice:  when in user mode, a pointer to the selected
%                           cl_device_id
%
%    o selectedDevice: a pointer to cl_device_id where the selected
%                      cl_device_id by ImageMagick could be returned
%
%    o exception: exception
%
*/
MagickExport MagickBooleanType InitImageMagickOpenCL(
  ImageMagickOpenCLMode mode,void *userSelectedDevice,void *selectedDevice,
  ExceptionInfo *exception)
{
  MagickBooleanType status = MagickFalse;
  MagickCLEnv clEnv = NULL;
  MagickBooleanType flag;

  clEnv = GetDefaultOpenCLEnv();
  if (clEnv!=NULL) {
    switch(mode) {

    case MAGICK_OPENCL_OFF:
      flag = MagickTrue;
      SetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED
        , sizeof(MagickBooleanType), &flag, exception);
      status = InitOpenCLEnv(clEnv, exception);

      if (selectedDevice)
        *(cl_device_id*)selectedDevice = NULL;
      break;

    case MAGICK_OPENCL_DEVICE_SELECT_USER:

      if (userSelectedDevice == NULL)
        return MagickFalse;

      flag = MagickFalse;
      SetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED
        , sizeof(MagickBooleanType), &flag, exception);

      SetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_DEVICE
        , sizeof(cl_device_id), userSelectedDevice,exception);

      status = InitOpenCLEnv(clEnv, exception);
      if (selectedDevice) {
        GetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_DEVICE
          , sizeof(cl_device_id), selectedDevice, exception);
      }
      break;

    case MAGICK_OPENCL_DEVICE_SELECT_AUTO_CLEAR_CACHE:
        flag = MagickTrue;
        SetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_PROGRAM_CACHE_DISABLED
          , sizeof(MagickBooleanType), &flag, exception);
        flag = MagickTrue;
        SetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_REGENERATE_PROFILE
          , sizeof(MagickBooleanType), &flag, exception);

    /* fall through here!! */
    case MAGICK_OPENCL_DEVICE_SELECT_AUTO:
    default:
      {
        cl_device_id d = NULL;
        flag = MagickFalse;
        SetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED
          , sizeof(MagickBooleanType), &flag, exception);
        SetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_DEVICE
          , sizeof(cl_device_id), &d,exception);
        status = InitOpenCLEnv(clEnv, exception);
        if (selectedDevice) {
          GetMagickOpenCLEnvParam(clEnv, MAGICK_OPENCL_ENV_PARAM_DEVICE
            , sizeof(cl_device_id),  selectedDevice, exception);
        }
      }
      break;
    };
  }
  return status;
}


MagickPrivate
MagickBooleanType OpenCLThrowMagickException(ExceptionInfo *exception,
  const char *module,const char *function,const size_t line,
  const ExceptionType severity,const char *tag,const char *format,...) {
  MagickBooleanType
    status;

  MagickCLEnv clEnv;

  status = MagickTrue;

  clEnv = GetDefaultOpenCLEnv();

  assert(exception != (ExceptionInfo *) NULL);
  assert(exception->signature == MagickCoreSignature);

  if (severity!=0) {
    cl_device_type dType;
    clEnv->library->clGetDeviceInfo(clEnv->device,CL_DEVICE_TYPE ,sizeof(cl_device_type),&dType,NULL);
    if (dType == CL_DEVICE_TYPE_CPU) {
      char buffer[MagickPathExtent];
      clEnv->library->clGetPlatformInfo(clEnv->platform, CL_PLATFORM_NAME, MagickPathExtent, buffer, NULL);

      /* Workaround for Intel OpenCL CPU runtime bug */
      /* Turn off OpenCL when a problem is detected! */
      if (strncmp(buffer, "Intel",5) == 0) {

        InitImageMagickOpenCL(MAGICK_OPENCL_OFF, NULL, NULL, exception);
      }
    }
  }

#ifdef OPENCLLOG_ENABLED
  {
    va_list
      operands;
    va_start(operands,format);
    status=ThrowMagickExceptionList(exception,module,function,line,severity,tag, format,operands);
    va_end(operands);
  }
#else
  magick_unreferenced(module);
  magick_unreferenced(function);
  magick_unreferenced(line);
  magick_unreferenced(tag);
  magick_unreferenced(format);
#endif

  return(status);
}

MagickPrivate cl_mem GetAndLockRandSeedBuffer(MagickCLEnv clEnv)
{
  LockSemaphoreInfo(clEnv->lock);
  if (clEnv->seedsLock == NULL)
  {
    ActivateSemaphoreInfo(&clEnv->seedsLock);
  }
  LockSemaphoreInfo(clEnv->seedsLock);

  if (clEnv->seeds == NULL)
  {
    cl_int clStatus;
    clEnv->numGenerators = NUM_CL_RAND_GENERATORS;
    clEnv->seeds = clEnv->library->clCreateBuffer(clEnv->context, CL_MEM_READ_WRITE,
                                  clEnv->numGenerators*4*sizeof(unsigned int),
                                  NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      clEnv->seeds = NULL;
    }
    else
    {
      unsigned int i;
      cl_command_queue queue = NULL;
      unsigned int *seeds;

      queue = AcquireOpenCLCommandQueue(clEnv);
      seeds = (unsigned int*) clEnv->library->clEnqueueMapBuffer(queue, clEnv->seeds, CL_TRUE,
                                                  CL_MAP_WRITE, 0,
                                                  clEnv->numGenerators*4
                                                  *sizeof(unsigned int),
                                                  0, NULL, NULL, &clStatus);
      if (clStatus!=CL_SUCCESS)
      {
        clEnv->library->clReleaseMemObject(clEnv->seeds);
        goto cleanup;
      }

      for (i = 0; i < clEnv->numGenerators; i++) {
        RandomInfo* randomInfo = AcquireRandomInfo();
        const unsigned long* s = GetRandomInfoSeed(randomInfo);
        if (i == 0)
          clEnv->randNormalize = GetRandomInfoNormalize(randomInfo);

        seeds[i*4]   = (unsigned int) s[0];
        seeds[i*4+1] = (unsigned int) 0x50a7f451;
        seeds[i*4+2] = (unsigned int) 0x5365417e;
        seeds[i*4+3] = (unsigned int) 0xc3a4171a;

        randomInfo = DestroyRandomInfo(randomInfo);
      }
      clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, clEnv->seeds, seeds, 0,
                                          NULL, NULL);
      clEnv->library->clFinish(queue);
cleanup:
      if (queue != NULL)
        RelinquishOpenCLCommandQueue(clEnv, queue);
    }
  }
  UnlockSemaphoreInfo(clEnv->lock);
  return clEnv->seeds;
}

MagickPrivate void UnlockRandSeedBuffer(MagickCLEnv clEnv) {
  if (clEnv->seedsLock == NULL)
  {
    ActivateSemaphoreInfo(&clEnv->seedsLock);
  }
  else
    UnlockSemaphoreInfo(clEnv->seedsLock);
}

MagickPrivate unsigned int GetNumRandGenerators(MagickCLEnv clEnv)
{
  return clEnv->numGenerators;
}


MagickPrivate float GetRandNormalize(MagickCLEnv clEnv)
{
  return clEnv->randNormalize;
}

#else

struct _MagickCLEnv {
  MagickBooleanType OpenCLInitialized;  /* whether OpenCL environment is initialized. */
};

MagickExport MagickCLEnv AcquireMagickOpenCLEnv()
{
  return NULL;
}

MagickExport MagickBooleanType RelinquishMagickOpenCLEnv(
  MagickCLEnv magick_unused(clEnv))
{
  magick_unreferenced(clEnv);

  return MagickFalse;
}

/*
* Return the OpenCL environment
*/
MagickExport MagickCLEnv GetDefaultOpenCLEnv()
{
  return (MagickCLEnv) NULL;
}

MagickExport MagickCLEnv SetDefaultOpenCLEnv(
  MagickCLEnv magick_unused(clEnv))
{
  magick_unreferenced(clEnv);

  return (MagickCLEnv) NULL;
}

MagickExport MagickBooleanType SetMagickOpenCLEnvParam(
  MagickCLEnv magick_unused(clEnv),MagickOpenCLEnvParam magick_unused(param),
  size_t magick_unused(dataSize),void *magick_unused(data),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(clEnv);
  magick_unreferenced(param);
  magick_unreferenced(dataSize);
  magick_unreferenced(data);
  magick_unreferenced(exception);

  return MagickFalse;
}

MagickExport MagickBooleanType GetMagickOpenCLEnvParam(
  MagickCLEnv magick_unused(clEnv),MagickOpenCLEnvParam magick_unused(param),
  size_t magick_unused(dataSize),void *magick_unused(data),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(clEnv);
  magick_unreferenced(param);
  magick_unreferenced(dataSize);
  magick_unreferenced(data);
  magick_unreferenced(exception);

  return MagickFalse;
}

MagickExport MagickBooleanType InitOpenCLEnv(MagickCLEnv magick_unused(clEnv),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(clEnv);
  magick_unreferenced(exception);

  return MagickFalse;
}

MagickPrivate cl_command_queue AcquireOpenCLCommandQueue(
  MagickCLEnv magick_unused(clEnv))
{
  magick_unreferenced(clEnv);

  return (cl_command_queue) NULL;
}

MagickPrivate MagickBooleanType RelinquishCommandQueue(
  MagickCLEnv magick_unused(clEnv),cl_command_queue magick_unused(queue))
{
  magick_unreferenced(clEnv);
  magick_unreferenced(queue);

  return MagickFalse;
}

MagickPrivate cl_kernel AcquireOpenCLKernel(
  MagickCLEnv magick_unused(clEnv),MagickOpenCLProgram magick_unused(program),
  const char *magick_unused(kernelName))
{
  magick_unreferenced(clEnv);
  magick_unreferenced(program);
  magick_unreferenced(kernelName);

  return (cl_kernel) NULL;
}

MagickPrivate MagickBooleanType RelinquishOpenCLKernel(
  MagickCLEnv magick_unused(clEnv),cl_kernel magick_unused(kernel))
{
  magick_unreferenced(clEnv);
  magick_unreferenced(kernel);

  return MagickFalse;
}

MagickPrivate unsigned long GetOpenCLDeviceLocalMemorySize(
  MagickCLEnv magick_unused(clEnv))
{
  magick_unreferenced(clEnv);

  return 0;
}

MagickExport MagickBooleanType InitImageMagickOpenCL(
  ImageMagickOpenCLMode magick_unused(mode),
  void *magick_unused(userSelectedDevice),void *magick_unused(selectedDevice),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(mode);
  magick_unreferenced(userSelectedDevice);
  magick_unreferenced(selectedDevice);
  magick_unreferenced(exception);
  return MagickFalse;
}


MagickPrivate
MagickBooleanType OpenCLThrowMagickException(ExceptionInfo *exception,
  const char *module,const char *function,const size_t line,
  const ExceptionType severity,const char *tag,const char *format,...)
{
  magick_unreferenced(exception);
  magick_unreferenced(module);
  magick_unreferenced(function);
  magick_unreferenced(line);
  magick_unreferenced(severity);
  magick_unreferenced(tag);
  magick_unreferenced(format);
  return(MagickFalse);
}


MagickPrivate cl_mem GetAndLockRandSeedBuffer(MagickCLEnv clEnv)
{
  magick_unreferenced(clEnv);
  return NULL;
}


MagickPrivate void UnlockRandSeedBuffer(MagickCLEnv clEnv)
{
  magick_unreferenced(clEnv);
}

MagickPrivate unsigned int GetNumRandGenerators(MagickCLEnv clEnv)
{
  magick_unreferenced(clEnv);
  return 0;
}

MagickPrivate float GetRandNormalize(MagickCLEnv clEnv)
{
  magick_unreferenced(clEnv);
  return 0.0f;
}

#endif /* MAGICKCORE_OPENCL_SUPPORT */

char* openclCachedFilesDirectory;
SemaphoreInfo* openclCachedFilesDirectoryLock;

MagickPrivate
const char* GetOpenCLCachedFilesDirectory() {
  if (openclCachedFilesDirectory == NULL) {
    if (openclCachedFilesDirectoryLock == NULL)
    {
      ActivateSemaphoreInfo(&openclCachedFilesDirectoryLock);
    }
    LockSemaphoreInfo(openclCachedFilesDirectoryLock);
    if (openclCachedFilesDirectory == NULL) {
      char path[MagickPathExtent];
      char *home = NULL;
      char *temp = NULL;
      struct stat attributes;
      MagickBooleanType status;
      int mkdirStatus = 0;



      home=GetEnvironmentValue("MAGICK_OPENCL_CACHE_DIR");
      if (home == (char *) NULL)
      {
        home=GetEnvironmentValue("XDG_CACHE_HOME");
        if (home == (char *) NULL)
          home=GetEnvironmentValue("LOCALAPPDATA");
        if (home == (char *) NULL)
          home=GetEnvironmentValue("APPDATA");
        if (home == (char *) NULL)
          home=GetEnvironmentValue("USERPROFILE");
      }

      if (home != (char *) NULL)
      {
        /* first check if $HOME exists */
        (void) FormatLocaleString(path,MagickPathExtent,"%s",home);
        status=GetPathAttributes(path,&attributes);
        if (status == MagickFalse)
        {

#ifdef MAGICKCORE_WINDOWS_SUPPORT
          mkdirStatus = mkdir(path);
#else
          mkdirStatus = mkdir(path, 0777);
#endif
        }

        /* first check if $HOME/ImageMagick exists */
        if (mkdirStatus==0)
        {
            (void) FormatLocaleString(path,MagickPathExtent,
              "%s%sImageMagick",home,DirectorySeparator);

            status=GetPathAttributes(path,&attributes);
            if (status == MagickFalse)
            {
#ifdef MAGICKCORE_WINDOWS_SUPPORT
              mkdirStatus = mkdir(path);
#else
              mkdirStatus = mkdir(path, 0777);
#endif
            }
        }

        if (mkdirStatus==0)
        {
          temp = (char*)AcquireMagickMemory(strlen(path)+1);
          CopyMagickString(temp,path,strlen(path)+1);
        }
        home=DestroyString(home);
      } else {
        home=GetEnvironmentValue("HOME");
        if (home != (char *) NULL)
        {
          /* first check if $HOME/.cache exists */
          (void) FormatLocaleString(path,MagickPathExtent,"%s%s.cache",
            home,DirectorySeparator);
          status=GetPathAttributes(path,&attributes);
          if (status == MagickFalse)
          {

#ifdef   MAGICKCORE_WINDOWS_SUPPORT
            mkdirStatus = mkdir(path);
#else
            mkdirStatus = mkdir(path, 0777);
#endif
          }

          /* first check if $HOME/.cache/ImageMagick exists */
          if (mkdirStatus==0)
          {
              (void) FormatLocaleString(path,MagickPathExtent,
                "%s%s.cache%sImageMagick",home,DirectorySeparator,
                DirectorySeparator);

              status=GetPathAttributes(path,&attributes);
              if (status == MagickFalse)
              {
#ifdef   MAGICKCORE_WINDOWS_SUPPORT
                mkdirStatus = mkdir(path);
#else
                mkdirStatus = mkdir(path, 0777);
#endif
              }
          }

          if (mkdirStatus==0)
          {
            temp = (char*)AcquireMagickMemory(strlen(path)+1);
            CopyMagickString(temp,path,strlen(path)+1);
          }
          home=DestroyString(home);
        }
      }
      openclCachedFilesDirectory = temp;
    }
    UnlockSemaphoreInfo(openclCachedFilesDirectoryLock);
  }
  return openclCachedFilesDirectory;
}

/* create a function for OpenCL log */
MagickPrivate
void OpenCLLog(const char* message) {

#ifdef OPENCLLOG_ENABLED
#define OPENCL_LOG_FILE "ImageMagickOpenCL.log"

  FILE* log;
  if (getenv("MAGICK_OCL_LOG"))
  {
    if (message) {
      char path[MagickPathExtent];
      unsigned long allocSize;

      MagickCLEnv clEnv;

      clEnv = GetDefaultOpenCLEnv();

      /*  dump the source into a file */
      (void) FormatLocaleString(path,MagickPathExtent,"%s%s%s"
        ,GetOpenCLCachedFilesDirectory()
        ,DirectorySeparator,OPENCL_LOG_FILE);


      log = fopen(path, "ab");
      fwrite(message, sizeof(char), strlen(message), log);
      fwrite("\n", sizeof(char), 1, log);

      if (clEnv->OpenCLInitialized && !clEnv->OpenCLDisabled)
      {
        allocSize = GetOpenCLDeviceMaxMemAllocSize(clEnv);
        fprintf(log, "Devic Max Memory Alloc Size: %lu\n", allocSize);
      }

      fclose(log);
    }
  }
#else
  magick_unreferenced(message);
#endif
}

MagickPrivate void OpenCLTerminus()
{
#if MAGICKCORE_OPENCL_SUPPORT
  DumpProfileData();
  if (openclCachedFilesDirectory != (char *) NULL)
    openclCachedFilesDirectory=DestroyString(openclCachedFilesDirectory);
  if (openclCachedFilesDirectoryLock != (SemaphoreInfo*)NULL)
    RelinquishSemaphoreInfo(&openclCachedFilesDirectoryLock);
  if (defaultCLEnv != (MagickCLEnv) NULL)
    {
       (void) RelinquishMagickOpenCLEnv(defaultCLEnv);
       defaultCLEnv=(MagickCLEnv)NULL;
    }
  if (defaultCLEnvLock != (SemaphoreInfo*) NULL)
    RelinquishSemaphoreInfo(&defaultCLEnvLock);
  if (OpenCLLib != (MagickLibrary *)NULL)
    OpenCLLib=(MagickLibrary *)RelinquishMagickMemory(OpenCLLib);
  if (OpenCLLibLock != (SemaphoreInfo*)NULL)
    RelinquishSemaphoreInfo(&OpenCLLibLock);
#endif
}