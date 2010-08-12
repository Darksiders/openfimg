/**
 * libsgl/egl.cpp
 *
 * SAMSUNG S3C6410 FIMG-3DSE (PROPER) EGL IMPLEMENTATION
 *
 * Copyrights:	2010 by Tomasz Figa < tomasz.figa at gmail.com >
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <sys/types.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <cutils/native_handle.h>

#include <utils/threads.h>
#include <pthread.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

#include <private/ui/sw_gralloc_handle.h>
#include <ui/android_native_buffer.h>
#include <hardware/copybit.h>
#include <hardware/gralloc.h>

#include "common.h"
#include "types.h"
#include "state.h"

#undef NELEM
#define NELEM(x) (sizeof(x)/sizeof(*(x)))

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

#define FGL_EGL_MAJOR		1
#define FGL_EGL_MINOR		3

using namespace android;

template<typename T>
static inline T max(T a, T b)
{
	return (a > b) ? a : b;
}

template<typename T>
static inline T min(T a, T b)
{
	return (a < b) ? a : b;
}

static char const * const gVendorString     = "notSamsung";
static char const * const gVersionString    = "1.4 S3C6410 Android 0.0.1";
static char const * const gClientApisString  = "OpenGL_ES";
static char const * const gExtensionsString =
	"EGL_KHR_image_base "
	"EGL_KHR_image_pixmap "
	"EGL_ANDROID_image_native_buffer "
	"EGL_ANDROID_swap_rectangle "
	"EGL_ANDROID_get_render_buffer "
	;

static pthread_mutex_t eglContextKeyMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t eglContextKey = -1;

struct FGLDisplay {
	EGLBoolean initialized;
};

#define FGL_MAX_DISPLAYS	1
static FGLDisplay displays[FGL_MAX_DISPLAYS];

static inline EGLBoolean isDisplayInitialized(EGLDisplay dpy)
{
	FGLDisplay *disp = (FGLDisplay *)dpy;

	return disp->initialized;
}

static inline EGLBoolean isDisplayValid(EGLDisplay dpy)
{
	EGLint disp = (EGLint)dpy;

	if(likely(disp == 1))
		return EGL_TRUE;

	return EGL_FALSE;
}

static inline FGLDisplay *getDisplay(EGLDisplay dpy)
{
	EGLint disp = (EGLint)dpy;

	return &displays[disp - 1];
}

static pthread_mutex_t eglErrorKeyMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t eglErrorKey = -1;

/**
	Error handling
*/

EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
	if(unlikely(eglErrorKey == -1))
		return EGL_SUCCESS;

	EGLint error = (EGLint)pthread_getspecific(eglErrorKey);
	pthread_setspecific(eglErrorKey, (void *)EGL_SUCCESS);
	return error;
}

static void setError(EGLint error)
{
	if(unlikely(eglErrorKey == -1)) {
		pthread_mutex_lock(&eglErrorKeyMutex);
		if(eglErrorKey == -1)
			pthread_key_create(&eglErrorKey, NULL);
		pthread_mutex_unlock(&eglErrorKeyMutex);
	}

	pthread_setspecific(eglErrorKey, (void *)error);
}

/**
	Initialization
*/

EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType display_id)
{
	if(unlikely(eglContextKey == -1)) {
		pthread_mutex_lock(&eglContextKeyMutex);
		if(eglContextKey == -1)
			pthread_key_create(&eglErrorKey, NULL);
		pthread_mutex_unlock(&eglContextKeyMutex);
	}

	if(display_id != EGL_DEFAULT_DISPLAY)
		return EGL_NO_DISPLAY;

	return (EGLDisplay)1;
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
	if(!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	FGLDisplay *disp = getDisplay(dpy);
	disp->initialized = EGL_TRUE;

	if(major != NULL)
		*major = FGL_EGL_MAJOR;

	if(minor != NULL)
		*minor = FGL_EGL_MINOR;

	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay dpy)
{
	if(!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	FGLDisplay *disp = getDisplay(dpy);
	disp->initialized = EGL_FALSE;

	return EGL_TRUE;
}

EGLAPI const char * EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name)
{
	if(!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return NULL;
	}

	if(!isDisplayInitialized(dpy)) {
		setError(EGL_NOT_INITIALIZED);
		return NULL;
	}

	switch(name) {
	case EGL_CLIENT_APIS:
		return gClientApisString;
	case EGL_EXTENSIONS:
		return gExtensionsString;
	case EGL_VENDOR:
		return gVendorString;
	case EGL_VERSION:
		return gVersionString;
	}

	setError(EGL_BAD_PARAMETER);
	return NULL;
}

/**
	Configurations
*/

struct FGLConfigPair {
	EGLint key;
	EGLint value;
};

struct FGLConfigs {
	const FGLConfigPair* array;
	int size;
};

struct FGLConfigMatcher {
	GLint key;
	bool (*match)(GLint reqValue, GLint confValue);

	static bool atLeast(GLint reqValue, GLint confValue)
	{
		return (reqValue == EGL_DONT_CARE) || (confValue >= reqValue);
	}
	static bool exact(GLint reqValue, GLint confValue)
	{
		return (reqValue == EGL_DONT_CARE) || (confValue == reqValue);
	}
	static bool mask(GLint reqValue, GLint confValue)
	{
		return (confValue & reqValue) == reqValue;
	}
};

/*
* In the lists below, attributes names MUST be sorted.
* Additionally, all configs must be sorted according to
* the EGL specification.
*/

#define FGL_MAX_VIEWPORT_DIMS		2048
#define FGL_MAX_VIEWPORT_PIXELS		(FGL_MAX_VIEWPORT_DIMS*FGL_MAX_VIEWPORT_DIMS)

static FGLConfigPair const baseConfigAttributes[] = {
	{ EGL_CONFIG_CAVEAT,              0                                 },
	{ EGL_LEVEL,                      0                                 },
	{ EGL_MAX_PBUFFER_HEIGHT,         FGL_MAX_VIEWPORT_DIMS             },
	{ EGL_MAX_PBUFFER_PIXELS,         FGL_MAX_VIEWPORT_PIXELS           },
	{ EGL_MAX_PBUFFER_WIDTH,          FGL_MAX_VIEWPORT_DIMS             },
	{ EGL_NATIVE_RENDERABLE,          EGL_FALSE                         },
	{ EGL_NATIVE_VISUAL_ID,           0                                 },
	{ EGL_NATIVE_VISUAL_TYPE,         0                                 },
	{ EGL_SAMPLES,                    0                                 },
	{ EGL_SAMPLE_BUFFERS,             0                                 },
	{ EGL_TRANSPARENT_TYPE,           EGL_NONE                          },
	{ EGL_TRANSPARENT_BLUE_VALUE,     0                                 },
	{ EGL_TRANSPARENT_GREEN_VALUE,    0                                 },
	{ EGL_TRANSPARENT_RED_VALUE,      0                                 },
	{ EGL_BIND_TO_TEXTURE_RGBA,       EGL_FALSE                         },
	{ EGL_BIND_TO_TEXTURE_RGB,        EGL_FALSE                         },
	{ EGL_MIN_SWAP_INTERVAL,          1                                 },
	{ EGL_MAX_SWAP_INTERVAL,          1                                 },
	{ EGL_LUMINANCE_SIZE,             0                                 },
	{ EGL_ALPHA_MASK_SIZE,            0                                 },
	{ EGL_COLOR_BUFFER_TYPE,          EGL_RGB_BUFFER                    },
	{ EGL_RENDERABLE_TYPE,            EGL_OPENGL_ES_BIT                 },
	{ EGL_CONFORMANT,                 0                                 }
};

// These configs can override the base attribute list
// NOTE: when adding a config here, don't forget to update eglCreate*Surface()

// RGB 565 configs
static FGLConfigPair const configAttributes0[] = {
	{ EGL_BUFFER_SIZE,     16 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        5 },
	{ EGL_GREEN_SIZE,       6 },
	{ EGL_RED_SIZE,         5 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_CONFIG_ID,        0 },
	{ EGL_STENCIL_SIZE,     0 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

static FGLConfigPair const configAttributes1[] = {
	{ EGL_BUFFER_SIZE,     16 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        5 },
	{ EGL_GREEN_SIZE,       6 },
	{ EGL_RED_SIZE,         5 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_CONFIG_ID,        1 },
	{ EGL_STENCIL_SIZE,     8 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

// RGB 888 configs
static FGLConfigPair const configAttributes2[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_CONFIG_ID,        6 },
	{ EGL_STENCIL_SIZE,     0 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

static FGLConfigPair const configAttributes3[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_CONFIG_ID,        7 },
	{ EGL_STENCIL_SIZE,     8 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

// ARGB 8888 configs
static FGLConfigPair const configAttributes4[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_CONFIG_ID,        2 },
	{ EGL_STENCIL_SIZE,     0 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

static FGLConfigPair const configAttributes5[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_CONFIG_ID,        3 },
	{ EGL_STENCIL_SIZE,     8 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

// A 8 configs
static FGLConfigPair const configAttributes6[] = {
	{ EGL_BUFFER_SIZE,      8 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        0 },
	{ EGL_GREEN_SIZE,       0 },
	{ EGL_RED_SIZE,         0 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_CONFIG_ID,        4 },
	{ EGL_STENCIL_SIZE,     0 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

static FGLConfigPair const configAttributes7[] = {
	{ EGL_BUFFER_SIZE,      8 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        0 },
	{ EGL_GREEN_SIZE,       0 },
	{ EGL_RED_SIZE,         0 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_CONFIG_ID,        5 },
	{ EGL_STENCIL_SIZE,     8 },
	{ EGL_SURFACE_TYPE,     EGL_WINDOW_BIT|EGL_PBUFFER_BIT|EGL_PIXMAP_BIT },
};

static FGLConfigs const gConfigs[] = {
	{ configAttributes0, NELEM(configAttributes0) },
	{ configAttributes1, NELEM(configAttributes1) },
	{ configAttributes2, NELEM(configAttributes2) },
	{ configAttributes3, NELEM(configAttributes3) },
	{ configAttributes4, NELEM(configAttributes4) },
	{ configAttributes5, NELEM(configAttributes5) },
	{ configAttributes6, NELEM(configAttributes6) },
	{ configAttributes7, NELEM(configAttributes7) },
};

static FGLConfigMatcher const gConfigManagement[] = {
	{ EGL_BUFFER_SIZE,                FGLConfigMatcher::atLeast },
	{ EGL_ALPHA_SIZE,                 FGLConfigMatcher::atLeast },
	{ EGL_BLUE_SIZE,                  FGLConfigMatcher::atLeast },
	{ EGL_GREEN_SIZE,                 FGLConfigMatcher::atLeast },
	{ EGL_RED_SIZE,                   FGLConfigMatcher::atLeast },
	{ EGL_DEPTH_SIZE,                 FGLConfigMatcher::atLeast },
	{ EGL_STENCIL_SIZE,               FGLConfigMatcher::atLeast },
	{ EGL_CONFIG_CAVEAT,              FGLConfigMatcher::exact   },
	{ EGL_CONFIG_ID,                  FGLConfigMatcher::exact   },
	{ EGL_LEVEL,                      FGLConfigMatcher::exact   },
	{ EGL_MAX_PBUFFER_HEIGHT,         FGLConfigMatcher::exact   },
	{ EGL_MAX_PBUFFER_PIXELS,         FGLConfigMatcher::exact   },
	{ EGL_MAX_PBUFFER_WIDTH,          FGLConfigMatcher::exact   },
	{ EGL_NATIVE_RENDERABLE,          FGLConfigMatcher::exact   },
	{ EGL_NATIVE_VISUAL_ID,           FGLConfigMatcher::exact   },
	{ EGL_NATIVE_VISUAL_TYPE,         FGLConfigMatcher::exact   },
	{ EGL_SAMPLES,                    FGLConfigMatcher::exact   },
	{ EGL_SAMPLE_BUFFERS,             FGLConfigMatcher::exact   },
	{ EGL_SURFACE_TYPE,               FGLConfigMatcher::mask    },
	{ EGL_TRANSPARENT_TYPE,           FGLConfigMatcher::exact   },
	{ EGL_TRANSPARENT_BLUE_VALUE,     FGLConfigMatcher::exact   },
	{ EGL_TRANSPARENT_GREEN_VALUE,    FGLConfigMatcher::exact   },
	{ EGL_TRANSPARENT_RED_VALUE,      FGLConfigMatcher::exact   },
	{ EGL_BIND_TO_TEXTURE_RGBA,       FGLConfigMatcher::exact   },
	{ EGL_BIND_TO_TEXTURE_RGB,        FGLConfigMatcher::exact   },
	{ EGL_MIN_SWAP_INTERVAL,          FGLConfigMatcher::exact   },
	{ EGL_MAX_SWAP_INTERVAL,          FGLConfigMatcher::exact   },
	{ EGL_LUMINANCE_SIZE,             FGLConfigMatcher::atLeast },
	{ EGL_ALPHA_MASK_SIZE,            FGLConfigMatcher::atLeast },
	{ EGL_COLOR_BUFFER_TYPE,          FGLConfigMatcher::exact   },
	{ EGL_RENDERABLE_TYPE,            FGLConfigMatcher::mask    },
	{ EGL_CONFORMANT,                 FGLConfigMatcher::mask    }
};


static FGLConfigPair const defaultConfigAttributes[] = {
// attributes that are not specified are simply ignored, if a particular
// one needs not be ignored, it must be specified here, eg:
// { EGL_SURFACE_TYPE, EGL_WINDOW_BIT },
};

// ----------------------------------------------------------------------------

#if 0
static FGLint getConfigFormatInfo(EGLint configID,
	int32_t *pixelFormat, int32_t *depthFormat)
{
	switch(configID) {
	case 0:
		pixelFormat = FGL_PIXEL_FORMAT_RGB_565;
		depthFormat = 0;
		break;
	case 1:
		pixelFormat = FGL_PIXEL_FORMAT_RGB_565;
		depthFormat = FGL_PIXEL_FORMAT_Z_16;
		break;
	case 2:
		pixelFormat = FGL_PIXEL_FORMAT_RGBX_8888;
		depthFormat = 0;
		break;
	case 3:
		pixelFormat = FGL_PIXEL_FORMAT_RGBX_8888;
		depthFormat = FGL_PIXEL_FORMAT_Z_16;
		break;
	case 4:
		pixelFormat = FGL_PIXEL_FORMAT_RGBA_8888;
		depthFormat = 0;
		break;
	case 5:
		pixelFormat = FGL_PIXEL_FORMAT_RGBA_8888;
		depthFormat = FGL_PIXEL_FORMAT_Z_16;
		break;
	case 6:
		pixelFormat = FGL_PIXEL_FORMAT_A_8;
		depthFormat = 0;
		break;
	case 7:
		pixelFormat = FGL_PIXEL_FORMAT_A_8;
		depthFormat = FGL_PIXEL_FORMAT_Z_16;
		break;
	default:
		return NAME_NOT_FOUND;
	}

	return FGL_NO_ERROR;
}
#endif

// ----------------------------------------------------------------------------

template<typename T>
static int binarySearch(T const sortedArray[], int first, int last, EGLint key)
{
	while (first <= last) {
		int mid = (first + last) / 2;

		if (key > sortedArray[mid].key) {
			first = mid + 1;
		} else if (key < sortedArray[mid].key) {
			last = mid - 1;
		} else {
			return mid;
		}
	}

	return -1;
}

static int isAttributeMatching(int i, EGLint attr, EGLint val)
{
	// look for the attribute in all of our configs
	FGLConfigPair const* configFound = gConfigs[i].array;
	int index = binarySearch<FGLConfigPair>(
		gConfigs[i].array,
		0, gConfigs[i].size-1,
		attr);

	if (index < 0) {
		configFound = baseConfigAttributes;
		index = binarySearch<FGLConfigPair>(
			baseConfigAttributes,
			0, NELEM(baseConfigAttributes)-1,
			attr);
	}

	if (index >= 0) {
		// attribute found, check if this config could match
		int cfgMgtIndex = binarySearch<FGLConfigMatcher>(
			gConfigManagement,
			0, NELEM(gConfigManagement)-1,
			attr);

		if (cfgMgtIndex >= 0) {
			bool match = gConfigManagement[cfgMgtIndex].match(
				val, configFound[index].value);
			if (match) {
				// this config matches
				return 1;
			}
		} else {
		// attribute not found. this should NEVER happen.
		}
	} else {
		// error, this attribute doesn't exist
	}

	return 0;
}

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
			EGLint config_size, EGLint *num_config)
{
	if(unlikely(!isDisplayValid(dpy))) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if(unlikely(!isDisplayInitialized(dpy))) {
		setError(EGL_NOT_INITIALIZED);
		return EGL_FALSE;
	}

	if(unlikely(num_config == NULL)) {
		setError(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}

	EGLint num = NELEM(gConfigs) - 1;

	if(configs == NULL) {
		*num_config = num;
		return EGL_TRUE;
	}

	EGLint i;
	for(i = 0; i < num && i < config_size; i++)
		*(configs)++ = (EGLConfig)i;

	*num_config = i;

	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
			EGLConfig *configs, EGLint config_size,
			EGLint *num_config)
{
	if (unlikely(!isDisplayValid(dpy))) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if (unlikely(num_config == NULL)) {
		setError(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}

	int numAttributes = 0;
	int numConfigs =  NELEM(gConfigs);
	uint32_t possibleMatch = (1<<numConfigs)-1;
	if(attrib_list) {
		while(possibleMatch && *attrib_list != EGL_NONE) {
			numAttributes++;
			EGLint attr = *attrib_list++;
			EGLint val  = *attrib_list++;
			for (int i=0 ; possibleMatch && i<numConfigs ; i++) {
				if (!(possibleMatch & (1<<i)))
					continue;
				if (isAttributeMatching(i, attr, val) == 0)
					possibleMatch &= ~(1<<i);
			}
		}
	}

	// now, handle the attributes which have a useful default value
	for (size_t j=0 ; possibleMatch && j<NELEM(defaultConfigAttributes) ; j++) {
		// see if this attribute was specified, if not, apply its
		// default value
		if (binarySearch<FGLConfigPair>(
			(FGLConfigPair const*)attrib_list,
			0, numAttributes-1,
			defaultConfigAttributes[j].key) < 0)
		{
			for (int i=0 ; possibleMatch && i<numConfigs ; i++) {
				if (!(possibleMatch & (1<<i)))
					continue;
				if (isAttributeMatching(i,
					defaultConfigAttributes[j].key,
					defaultConfigAttributes[j].value) == 0)
				{
					possibleMatch &= ~(1<<i);
				}
			}
		}
	}

	// return the configurations found
	int n=0;
	if (possibleMatch) {
		if (configs) {
			for (int i=0 ; config_size && i<numConfigs ; i++) {
				if (possibleMatch & (1<<i)) {
					*configs++ = (EGLConfig)i;
					config_size--;
					n++;
				}
			}
		} else {
			for (int i=0 ; i<numConfigs ; i++) {
				if (possibleMatch & (1<<i)) {
					n++;
				}
			}
		}
	}

	*num_config = n;
	return EGL_TRUE;
}

static EGLBoolean getConfigAttrib(EGLDisplay dpy, EGLConfig config,
	EGLint attribute, EGLint *value)
{
	size_t numConfigs =  NELEM(gConfigs);
	int index = (int)config;

	if (uint32_t(index) >= numConfigs) {
		setError(EGL_BAD_CONFIG);
		return EGL_FALSE;
	}

	int attrIndex;
	attrIndex = binarySearch<FGLConfigPair>(
		gConfigs[index].array,
		0, gConfigs[index].size-1,
		attribute);

	if (attrIndex>=0) {
		*value = gConfigs[index].array[attrIndex].value;
		return EGL_TRUE;
	}

	attrIndex = binarySearch<FGLConfigPair>(
		baseConfigAttributes,
		0, NELEM(baseConfigAttributes)-1,
		attribute);

	if (attrIndex>=0) {
		*value = baseConfigAttributes[attrIndex].value;
		return EGL_TRUE;
	}

	setError(EGL_BAD_ATTRIBUTE);
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
			EGLint attribute, EGLint *value)
{
	if (unlikely(!isDisplayValid(dpy))) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	return getConfigAttrib(dpy, config, attribute, value);
}

/**
	Surfaces
*/

struct FGLPlane {
	FGLuint		version;
	FGLuint		width;      // width in pixels
	FGLuint		height;     // height in pixels
	FGLuint		stride;     // stride in pixels
	FGLubyte	*data;      // pointer to the bits
	FGLint		format;     // pixel format
};

void *fimgAllocMemory(size_t size)
{
#warning Unimplemented critical function
	return NULL;
}

void fimgFreeMemory(void *mem)
{
#warning Unimplemented function
}

void fglSetColorBuffer(FGLContext *gl, FGLPlane *cbuf)
{
#warning Unimplemented function
}

void fglSetDepthBuffer(FGLContext *gl, FGLPlane *zbuf)
{
#warning Unimplemented function
}

void fglSetReadBuffer(FGLContext *gl, FGLPlane *rbuf)
{
#warning Unimplemented function
}

FGLint getBpp(int format)
{
#warning Unimplemented function
	return 0;
}

struct FGLSurface
{
	enum {
		PAGE_FLIP = 0x00000001,
		MAGIC     = 0x31415265
	};

	uint32_t            magic;
	EGLDisplay          dpy;
	EGLConfig           config;
	EGLContext          ctx;

	FGLSurface(EGLDisplay dpy, EGLConfig config, int32_t depthFormat);
	virtual     ~FGLSurface();
	            bool    isValid() const;
	virtual     bool    initCheck() const = 0;

	virtual     EGLBoolean  bindDrawSurface(FGLContext* gl) = 0;
	virtual     EGLBoolean  bindReadSurface(FGLContext* gl) = 0;
	virtual     EGLBoolean  connect() { return EGL_TRUE; }
	virtual     void        disconnect() {}
	virtual     EGLint      getWidth() const = 0;
	virtual     EGLint      getHeight() const = 0;

	virtual     EGLint      getHorizontalResolution() const;
	virtual     EGLint      getVerticalResolution() const;
	virtual     EGLint      getRefreshRate() const;
	virtual     EGLint      getSwapBehavior() const;
	virtual     EGLBoolean  swapBuffers();
	virtual     EGLBoolean  setSwapRectangle(EGLint l, EGLint t, EGLint w, EGLint h);
	virtual     EGLClientBuffer getRenderBuffer() const;
protected:
	FGLPlane              depth;
};

FGLSurface::FGLSurface(EGLDisplay dpy,
	EGLConfig config,
	int32_t depthFormat)
: magic(MAGIC), dpy(dpy), config(config), ctx(0)
{
	depth.version = sizeof(FGLPlane);
	depth.data = 0;
	depth.format = depthFormat;
}

FGLSurface::~FGLSurface()
{
	magic = 0;
	fimgFreeMemory(depth.data);
}

bool FGLSurface::isValid() const {
	LOGE_IF(magic != MAGIC, "invalid EGLSurface (%p)", this);
	return magic == MAGIC;
}

EGLBoolean FGLSurface::swapBuffers() {
	return EGL_FALSE;
}

EGLint FGLSurface::getHorizontalResolution() const {
	return (0 * EGL_DISPLAY_SCALING) * (1.0f / 25.4f);
}

EGLint FGLSurface::getVerticalResolution() const {
	return (0 * EGL_DISPLAY_SCALING) * (1.0f / 25.4f);
}

EGLint FGLSurface::getRefreshRate() const {
	return (60 * EGL_DISPLAY_SCALING);
}

EGLint FGLSurface::getSwapBehavior() const {
	return EGL_BUFFER_PRESERVED;
}

EGLBoolean FGLSurface::setSwapRectangle(
	EGLint l, EGLint t, EGLint w, EGLint h)
{
	return EGL_FALSE;
}

EGLClientBuffer FGLSurface::getRenderBuffer() const {
	return 0;
}

// ----------------------------------------------------------------------------

struct FGLWindowSurface : public FGLSurface
{
	FGLWindowSurface(
		EGLDisplay dpy, EGLConfig config,
		int32_t depthFormat,
		android_native_window_t* window);

	~FGLWindowSurface();

	virtual     bool        initCheck() const { return true; } // TODO: report failure if ctor fails
	virtual     EGLBoolean  swapBuffers();
	virtual     EGLBoolean  bindDrawSurface(FGLContext* gl);
	virtual     EGLBoolean  bindReadSurface(FGLContext* gl);
	virtual     EGLBoolean  connect();
	virtual     void        disconnect();
	virtual     EGLint      getWidth() const    { return width;  }
	virtual     EGLint      getHeight() const   { return height; }
	virtual     EGLint      getHorizontalResolution() const;
	virtual     EGLint      getVerticalResolution() const;
	virtual     EGLint      getRefreshRate() const;
	virtual     EGLint      getSwapBehavior() const;
	virtual     EGLBoolean  setSwapRectangle(EGLint l, EGLint t, EGLint w, EGLint h);
	virtual     EGLClientBuffer  getRenderBuffer() const;

private:
	FGLint lock(android_native_buffer_t* buf, int usage, void** vaddr);
	FGLint unlock(android_native_buffer_t* buf);
	android_native_window_t*   nativeWindow;
	android_native_buffer_t*   buffer;
	android_native_buffer_t*   previousBuffer;
	gralloc_module_t const*    module;
	copybit_device_t*          blitengine;
	int width;
	int height;
	void* bits;

	struct Rect {
		inline Rect() { };
		inline Rect(int32_t w, int32_t h)
		: left(0), top(0), right(w), bottom(h) { }
		inline Rect(int32_t l, int32_t t, int32_t r, int32_t b)
		: left(l), top(t), right(r), bottom(b) { }
		Rect& andSelf(const Rect& r) {
		left   = max(left, r.left);
		top    = max(top, r.top);
		right  = min(right, r.right);
		bottom = min(bottom, r.bottom);
		return *this;
		}
		bool isEmpty() const {
		return (left>=right || top>=bottom);
		}
		void dump(char const* what) {
		LOGD("%s { %5d, %5d, w=%5d, h=%5d }",
			what, left, top, right-left, bottom-top);
		}

		int32_t left;
		int32_t top;
		int32_t right;
		int32_t bottom;
	};

	struct Region {
		inline Region() : count(0) { }
		typedef Rect const* const_iterator;
		const_iterator begin() const { return storage; }
		const_iterator end() const { return storage+count; }
		static Region subtract(const Rect& lhs, const Rect& rhs) {
		Region reg;
		Rect* storage = reg.storage;
		if (!lhs.isEmpty()) {
			if (lhs.top < rhs.top) { // top rect
			storage->left   = lhs.left;
			storage->top    = lhs.top;
			storage->right  = lhs.right;
			storage->bottom = rhs.top;
			storage++;
			}
			const int32_t top = max(lhs.top, rhs.top);
			const int32_t bot = min(lhs.bottom, rhs.bottom);
			if (top < bot) {
			if (lhs.left < rhs.left) { // left-side rect
				storage->left   = lhs.left;
				storage->top    = top;
				storage->right  = rhs.left;
				storage->bottom = bot;
				storage++;
			}
			if (lhs.right > rhs.right) { // right-side rect
				storage->left   = rhs.right;
				storage->top    = top;
				storage->right  = lhs.right;
				storage->bottom = bot;
				storage++;
			}
			}
			if (lhs.bottom > rhs.bottom) { // bottom rect
			storage->left   = lhs.left;
			storage->top    = rhs.bottom;
			storage->right  = lhs.right;
			storage->bottom = lhs.bottom;
			storage++;
			}
			reg.count = storage - reg.storage;
		}
		return reg;
		}
		bool isEmpty() const {
		return count<=0;
		}
	private:
		Rect storage[4];
		ssize_t count;
	};

	struct region_iterator : public copybit_region_t {
		region_iterator(const Region& region)
		: b(region.begin()), e(region.end()) {
			this->next = iterate;
		}
	private:
		static int iterate(copybit_region_t const * self, copybit_rect_t* rect) {
			region_iterator const* me = static_cast<region_iterator const*>(self);
			if (me->b != me->e) {
				*reinterpret_cast<Rect*>(rect) = *me->b++;
				return 1;
			}
			return 0;
		}
		mutable Region::const_iterator b;
		Region::const_iterator const e;
	};

	void copyBlt(
		android_native_buffer_t* dst, void* dst_vaddr,
		android_native_buffer_t* src, void const* src_vaddr,
		const Region& clip);

	Rect dirtyRegion;
	Rect oldDirtyRegion;
};

FGLWindowSurface::FGLWindowSurface(EGLDisplay dpy,
	EGLConfig config,
	int32_t depthFormat,
	android_native_window_t* window)
	: FGLSurface(dpy, config, depthFormat),
	nativeWindow(window), buffer(0), previousBuffer(0), module(0),
	blitengine(0), bits(NULL)
{
	hw_module_t const* pModule;
	hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &pModule);
	module = reinterpret_cast<gralloc_module_t const*>(pModule);

	if (hw_get_module(COPYBIT_HARDWARE_MODULE_ID, &pModule) == 0) {
		copybit_open(pModule, &blitengine);
	}

	// keep a reference on the window
	nativeWindow->common.incRef(&nativeWindow->common);
	nativeWindow->query(nativeWindow, NATIVE_WINDOW_WIDTH, &width);
	nativeWindow->query(nativeWindow, NATIVE_WINDOW_HEIGHT, &height);
	}

	FGLWindowSurface::~FGLWindowSurface() {
	if (buffer) {
		buffer->common.decRef(&buffer->common);
	}
	if (previousBuffer) {
		previousBuffer->common.decRef(&previousBuffer->common);
	}
	nativeWindow->common.decRef(&nativeWindow->common);
	if (blitengine) {
		copybit_close(blitengine);
	}
}

EGLBoolean FGLWindowSurface::connect()
{
	// we're intending to do hardware rendering
	native_window_set_usage(nativeWindow,
		GRALLOC_USAGE_SW_READ_RARELY | GRALLOC_USAGE_SW_WRITE_NEVER | GRALLOC_USAGE_HW_RENDER);

	// dequeue a buffer
	if (nativeWindow->dequeueBuffer(nativeWindow, &buffer) != FGL_NO_ERROR) {
		setError(EGL_BAD_ALLOC);
		return EGL_FALSE;
	}

	// allocate a corresponding depth-buffer
	width = buffer->width;
	height = buffer->height;
	if (depth.format) {
		depth.width   = width;
		depth.height  = height;
		depth.stride  = depth.width; // use the width here
		depth.data    = (FGLubyte*)fimgAllocMemory(depth.stride*depth.height*4);
		if (depth.data == 0) {
		setError(EGL_BAD_ALLOC);
		return EGL_FALSE;
		}
	}

	// keep a reference on the buffer
	buffer->common.incRef(&buffer->common);

	// Lock the buffer
	nativeWindow->lockBuffer(nativeWindow, buffer);
	// pin the buffer down
	if (lock(buffer, GRALLOC_USAGE_SW_READ_OFTEN |
		GRALLOC_USAGE_SW_WRITE_OFTEN, &bits) != FGL_NO_ERROR) {
		LOGE("connect() failed to lock buffer %p (%ux%u)",
			buffer, buffer->width, buffer->height);
		setError(EGL_BAD_ACCESS);
		return EGL_FALSE;
		// FIXME: we should make sure we're not accessing the buffer anymore
	}
	return EGL_TRUE;
}

void FGLWindowSurface::disconnect()
{
	if (buffer && bits) {
		bits = NULL;
		unlock(buffer);
	}
	// enqueue the last frame
	nativeWindow->queueBuffer(nativeWindow, buffer);
	if (buffer) {
		buffer->common.decRef(&buffer->common);
		buffer = 0;
	}
	if (previousBuffer) {
		previousBuffer->common.decRef(&previousBuffer->common);
		previousBuffer = 0;
	}
}

FGLint FGLWindowSurface::lock(
	android_native_buffer_t* buf, int usage, void** vaddr)
{
	int err;
	if (sw_gralloc_handle_t::validate(buf->handle) < 0) {
		err = module->lock(module, buf->handle,
			usage, 0, 0, buf->width, buf->height, vaddr);
	} else {
		sw_gralloc_handle_t const* hnd =
			reinterpret_cast<sw_gralloc_handle_t const*>(buf->handle);
		*vaddr = (void*)hnd->base;
		err = FGL_NO_ERROR;
	}
	return err;
}

FGLint FGLWindowSurface::unlock(android_native_buffer_t* buf)
{
	if (!buf) return BAD_VALUE;
	int err = FGL_NO_ERROR;
	if (sw_gralloc_handle_t::validate(buf->handle) < 0) {
		err = module->unlock(module, buf->handle);
	}
	return err;
}

void FGLWindowSurface::copyBlt(
	android_native_buffer_t* dst, void* dst_vaddr,
	android_native_buffer_t* src, void const* src_vaddr,
	const Region& clip)
{
	// FIXME: use copybit if possible
	// NOTE: dst and src must be the same format

	FGLint err = FGL_NO_ERROR;
	copybit_device_t* const copybit = blitengine;
	if (copybit)  {
		copybit_image_t simg;
		simg.w = src->stride;
		simg.h = src->height;
		simg.format = src->format;
		simg.handle = const_cast<native_handle_t*>(src->handle);

		copybit_image_t dimg;
		dimg.w = dst->stride;
		dimg.h = dst->height;
		dimg.format = dst->format;
		dimg.handle = const_cast<native_handle_t*>(dst->handle);

		copybit->set_parameter(copybit, COPYBIT_TRANSFORM, 0);
		copybit->set_parameter(copybit, COPYBIT_PLANE_ALPHA, 255);
		copybit->set_parameter(copybit, COPYBIT_DITHER, COPYBIT_DISABLE);
		region_iterator it(clip);
		err = copybit->blit(copybit, &dimg, &simg, &it);
		if (err != FGL_NO_ERROR) {
		LOGE("copybit failed (%s)", strerror(err));
		}
	}

	if (!copybit || err) {
		Region::const_iterator cur = clip.begin();
		Region::const_iterator end = clip.end();

		const size_t bpp = getBpp(src->format);
		const size_t dbpr = dst->stride * bpp;
		const size_t sbpr = src->stride * bpp;

		uint8_t const * const src_bits = (uint8_t const *)src_vaddr;
		uint8_t       * const dst_bits = (uint8_t       *)dst_vaddr;

		while (cur != end) {
		const Rect& r(*cur++);
		ssize_t w = r.right - r.left;
		ssize_t h = r.bottom - r.top;
		if (w <= 0 || h<=0) continue;
		size_t size = w * bpp;
		uint8_t const * s = src_bits + (r.left + src->stride * r.top) * bpp;
		uint8_t       * d = dst_bits + (r.left + dst->stride * r.top) * bpp;
		if (dbpr==sbpr && size==sbpr) {
			size *= h;
			h = 1;
		}
		do {
			memcpy(d, s, size);
			d += dbpr;
			s += sbpr;
		} while (--h > 0);
		}
	}
}

EGLBoolean FGLWindowSurface::swapBuffers()
{
	if (!buffer) {
		setError(EGL_BAD_ACCESS);
		return EGL_FALSE;
	}

	/*
	* Handle eglSetSwapRectangleANDROID()
	* We copyback from the front buffer
	*/
	if (!dirtyRegion.isEmpty()) {
		dirtyRegion.andSelf(Rect(buffer->width, buffer->height));
		if (previousBuffer) {
		const Region copyBack(Region::subtract(oldDirtyRegion, dirtyRegion));
		if (!copyBack.isEmpty()) {
			void* prevBits;
			if (lock(previousBuffer,
				GRALLOC_USAGE_SW_READ_OFTEN, &prevBits) == FGL_NO_ERROR) {
			// copy from previousBuffer to buffer
			copyBlt(buffer, bits, previousBuffer, prevBits, copyBack);
			unlock(previousBuffer);
			}
		}
		}
		oldDirtyRegion = dirtyRegion;
	}

	if (previousBuffer) {
		previousBuffer->common.decRef(&previousBuffer->common);
		previousBuffer = 0;
	}

	unlock(buffer);
	previousBuffer = buffer;
	nativeWindow->queueBuffer(nativeWindow, buffer);
	buffer = 0;

	// dequeue a new buffer
	nativeWindow->dequeueBuffer(nativeWindow, &buffer);

	// TODO: lockBuffer should rather be executed when the very first
	// direct rendering occurs.
	nativeWindow->lockBuffer(nativeWindow, buffer);

	// reallocate the depth-buffer if needed
	if ((width != buffer->width) || (height != buffer->height)) {
		// TODO: we probably should reset the swap rect here
		// if the window size has changed
		width = buffer->width;
		height = buffer->height;
		if (depth.data) {
			fimgFreeMemory(depth.data);
			depth.width   = width;
			depth.height  = height;
			depth.stride  = buffer->stride;
			depth.data    = (FGLubyte*)fimgAllocMemory(depth.stride*depth.height*4);
			if (depth.data == 0) {
				setError(EGL_BAD_ALLOC);
				return EGL_FALSE;
			}
		}
	}

	// keep a reference on the buffer
	buffer->common.incRef(&buffer->common);

	// finally pin the buffer down
	if (lock(buffer, GRALLOC_USAGE_SW_READ_OFTEN |
		GRALLOC_USAGE_SW_WRITE_OFTEN, &bits) != FGL_NO_ERROR) {
		LOGE("eglSwapBuffers() failed to lock buffer %p (%ux%u)",
			buffer, buffer->width, buffer->height);
		setError(EGL_BAD_ACCESS);
		return EGL_FALSE;
		// FIXME: we should make sure we're not accessing the buffer anymore
	}

	return EGL_TRUE;
}

EGLBoolean FGLWindowSurface::setSwapRectangle(
	EGLint l, EGLint t, EGLint w, EGLint h)
{
	dirtyRegion = Rect(l, t, l+w, t+h);
	return EGL_TRUE;
}

EGLClientBuffer FGLWindowSurface::getRenderBuffer() const
{
	return buffer;
}

#ifdef LIBAGL_USE_GRALLOC_COPYBITS

static bool supportedCopybitsDestinationFormat(int format) {
	// Hardware supported
	switch (format) {
	case HAL_PIXEL_FORMAT_RGB_565:
	case HAL_PIXEL_FORMAT_RGBA_8888:
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_RGBA_4444:
	case HAL_PIXEL_FORMAT_RGBA_5551:
	case HAL_PIXEL_FORMAT_BGRA_8888:
		return true;
	}
	return false;
}
#endif

EGLBoolean FGLWindowSurface::bindDrawSurface(FGLContext* gl)
{
	FGLPlane buffer;
	
	buffer.version = sizeof(FGLPlane);
	buffer.width   = this->buffer->width;
	buffer.height  = this->buffer->height;
	buffer.stride  = this->buffer->stride;
	buffer.data    = (FGLubyte*)bits;
	buffer.format  = this->buffer->format;
	
	fglSetColorBuffer(gl, &buffer);
	fglSetDepthBuffer(gl, &depth);

#if 0
	gl->copybits.drawSurfaceBuffer = 0;
	if (gl->copybits.blitEngine != NULL) {
		if (supportedCopybitsDestinationFormat(buffer.format)) {
		buffer_handle_t handle = this->buffer->handle;
		if (handle != NULL) {
			gl->copybits.drawSurfaceBuffer = this->buffer;
		}
		}
	}
#endif

	return EGL_TRUE;
}

EGLBoolean FGLWindowSurface::bindReadSurface(FGLContext* gl)
{
	FGLPlane buffer;
	buffer.version = sizeof(FGLPlane);
	buffer.width   = this->buffer->width;
	buffer.height  = this->buffer->height;
	buffer.stride  = this->buffer->stride;
	buffer.data    = (FGLubyte*)bits; // FIXME: hopefully is is LOCKED!!!
	buffer.format  = this->buffer->format;
	fglSetReadBuffer(gl, &buffer);
	return EGL_TRUE;
}

EGLint FGLWindowSurface::getHorizontalResolution() const {
	return (nativeWindow->xdpi * EGL_DISPLAY_SCALING) * (1.0f / 25.4f);
}

EGLint FGLWindowSurface::getVerticalResolution() const {
	return (nativeWindow->ydpi * EGL_DISPLAY_SCALING) * (1.0f / 25.4f);
}

EGLint FGLWindowSurface::getRefreshRate() const {
	return (60 * EGL_DISPLAY_SCALING); // FIXME
}

EGLint FGLWindowSurface::getSwapBehavior() const
{
	/*
	* EGL_BUFFER_PRESERVED means that eglSwapBuffers() completely preserves
	* the content of the swapped buffer.
	*
	* EGL_BUFFER_DESTROYED means that the content of the buffer is lost.
	*
	* However when ANDROID_swap_retcangle is supported, EGL_BUFFER_DESTROYED
	* only applies to the area specified by eglSetSwapRectangleANDROID(), that
	* is, everything outside of this area is preserved.
	*
	* This implementation of EGL assumes the later case.
	*
	*/

	return EGL_BUFFER_DESTROYED;
}

// ----------------------------------------------------------------------------

struct FGLPixmapSurface : public FGLSurface
{
	FGLPixmapSurface(
		EGLDisplay dpy, EGLConfig config,
		int32_t depthFormat,
		egl_native_pixmap_t const * pixmap);

	virtual ~FGLPixmapSurface() { }

	virtual     bool        initCheck() const { return !depth.format || depth.data!=0; }
	virtual     EGLBoolean  bindDrawSurface(FGLContext* gl);
	virtual     EGLBoolean  bindReadSurface(FGLContext* gl);
	virtual     EGLint      getWidth() const    { return nativePixmap.width;  }
	virtual     EGLint      getHeight() const   { return nativePixmap.height; }
	private:
	egl_native_pixmap_t     nativePixmap;
};

FGLPixmapSurface::FGLPixmapSurface(EGLDisplay dpy,
	EGLConfig config,
	int32_t depthFormat,
	egl_native_pixmap_t const * pixmap)
	: FGLSurface(dpy, config, depthFormat), nativePixmap(*pixmap)
{
	if (depthFormat) {
		depth.width   = pixmap->width;
		depth.height  = pixmap->height;
		depth.stride  = depth.width; // use the width here
		depth.data    = (FGLubyte*)fimgAllocMemory(depth.stride*depth.height*4);
		if (depth.data == 0) {
			setError(EGL_BAD_ALLOC);
		}
	}
}

EGLBoolean FGLPixmapSurface::bindDrawSurface(FGLContext* gl)
{
	FGLPlane buffer;

	buffer.version = sizeof(FGLPlane);
	buffer.width   = nativePixmap.width;
	buffer.height  = nativePixmap.height;
	buffer.stride  = nativePixmap.stride;
	buffer.data    = nativePixmap.data;
	buffer.format  = nativePixmap.format;

	fglSetColorBuffer(gl, &buffer);
	fglSetDepthBuffer(gl, &depth);

	return EGL_TRUE;
}

EGLBoolean FGLPixmapSurface::bindReadSurface(FGLContext* gl)
{
	FGLPlane buffer;

	buffer.version = sizeof(FGLPlane);
	buffer.width   = nativePixmap.width;
	buffer.height  = nativePixmap.height;
	buffer.stride  = nativePixmap.stride;
	buffer.data    = nativePixmap.data;
	buffer.format  = nativePixmap.format;

	fglSetReadBuffer(gl, &buffer);

	return EGL_TRUE;
}

// ----------------------------------------------------------------------------

struct FGLPbufferSurface : public FGLSurface
{
	FGLPbufferSurface(
		EGLDisplay dpy, EGLConfig config, int32_t depthFormat,
		int32_t w, int32_t h, int32_t f);

	virtual ~FGLPbufferSurface();

	virtual     bool        initCheck() const   { return pbuffer.data != 0; }
	virtual     EGLBoolean  bindDrawSurface(FGLContext* gl);
	virtual     EGLBoolean  bindReadSurface(FGLContext* gl);
	virtual     EGLint      getWidth() const    { return pbuffer.width;  }
	virtual     EGLint      getHeight() const   { return pbuffer.height; }
	private:
	FGLPlane  pbuffer;
};

FGLPbufferSurface::FGLPbufferSurface(EGLDisplay dpy,
	EGLConfig config, int32_t depthFormat,
	int32_t w, int32_t h, int32_t f)
: FGLSurface(dpy, config, depthFormat)
{
	size_t size = w*h;
	switch (f) {
		case FGL_PIXEL_FORMAT_A_8:          size *= 1; break;
		case FGL_PIXEL_FORMAT_RGB_565:      size *= 2; break;
		case FGL_PIXEL_FORMAT_RGBA_8888:    size *= 4; break;
		case FGL_PIXEL_FORMAT_RGBX_8888:    size *= 4; break;
		default:
		LOGE("incompatible pixel format for pbuffer (format=%d)", f);
		pbuffer.data = 0;
		break;
	}
	pbuffer.version = sizeof(FGLPlane);
	pbuffer.width   = w;
	pbuffer.height  = h;
	pbuffer.stride  = w;
	pbuffer.data    = (FGLubyte*)fimgAllocMemory(size);
	pbuffer.format  = f;

	if (depthFormat) {
		depth.width   = pbuffer.width;
		depth.height  = pbuffer.height;
		depth.stride  = depth.width; // use the width here
		depth.data    = (FGLubyte*)fimgAllocMemory(depth.stride*depth.height*4);
		if (depth.data == 0) {
			setError(EGL_BAD_ALLOC);
		}
	}
}

FGLPbufferSurface::~FGLPbufferSurface() {
	fimgFreeMemory(pbuffer.data);
}

EGLBoolean FGLPbufferSurface::bindDrawSurface(FGLContext* gl)
{
	fglSetColorBuffer(gl, &pbuffer);
	fglSetDepthBuffer(gl, &depth);

	return EGL_TRUE;
}

EGLBoolean FGLPbufferSurface::bindReadSurface(FGLContext* gl)
{
	fglSetReadBuffer(gl, &pbuffer);

	return EGL_TRUE;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
				EGLNativeWindowType win,
				const EGLint *attrib_list)
{
#warning Unimplemented function
	return EGL_NO_SURFACE;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
				const EGLint *attrib_list)
{
#warning Unimplemented function
	return EGL_NO_SURFACE;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
				EGLNativePixmapType pixmap,
				const EGLint *attrib_list)
{
#warning Unimplemented function
	return EGL_NO_SURFACE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
			EGLint attribute, EGLint *value)
{
#warning Unimplemented function
	return EGL_FALSE;
}


EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLenum EGLAPIENTRY eglQueryAPI(void)
{
#warning Unimplemented function
	return EGL_NONE;
}


EGLAPI EGLBoolean EGLAPIENTRY eglWaitClient(void)
{
#warning Unimplemented function
	return EGL_FALSE;
}


EGLAPI EGLBoolean EGLAPIENTRY eglReleaseThread(void)
{
#warning Unimplemented function
	return EGL_FALSE;
}


EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferFromClientBuffer(
	EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
	EGLConfig config, const EGLint *attrib_list)
{
#warning Unimplemented function
	return EGL_FALSE;
}


EGLAPI EGLBoolean EGLAPIENTRY eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface,
			EGLint attribute, EGLint value)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config,
			EGLContext share_context,
			const EGLint *attrib_list)
{
#warning Unimplemented function
	return EGL_NO_CONTEXT;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
			EGLSurface read, EGLContext ctx)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void)
{
#warning Unimplemented function
	return EGL_NO_CONTEXT;
}

EGLAPI EGLSurface EGLAPIENTRY eglGetCurrentSurface(EGLint readdraw)
{
#warning Unimplemented function
	return EGL_NO_SURFACE;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetCurrentDisplay(void)
{
#warning Unimplemented function
	return EGL_NO_DISPLAY;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQueryContext(EGLDisplay dpy, EGLContext ctx,
			EGLint attribute, EGLint *value)
{
#warning Unimplemented function
	return EGL_FALSE;
}


EGLAPI EGLBoolean EGLAPIENTRY eglWaitGL(void)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglWaitNative(EGLint engine)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
#warning Unimplemented function
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglCopyBuffers(EGLDisplay dpy, EGLSurface surface,
			EGLNativePixmapType target)
{
#warning Unimplemented function
	return EGL_FALSE;
}


EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY
eglGetProcAddress(const char *procname)
{
#warning Unimplemented function
	return NULL;
}