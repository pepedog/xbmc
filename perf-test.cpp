//#define LINUX
//#define EGL_API_FB
#define GL_GLEXT_PROTOTYPES

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/ipu.h>
#include <linux/mxcfb.h>
#include <linux/kd.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <cores/dvdplayer/DVDCodecs/Video/DVDVideoCodecIMX.h>
#include <cores/dvdplayer/DVDCodecs/DVDCodecs.h>
#include <cores/dvdplayer/DVDClock.h>
#include <xbmc/threads/Thread.h>
#include <xbmc/settings/MediaSettings.h>
#include <xbmc/utils/log.h>

#include <iostream>
#include <iomanip>
#include <vector>

using namespace std;


bool appExit = false;
bool doubleRate = false;


void signal_handler(int signum) {
	appExit = true;
}


EGLDisplay display;
EGLContext context;
EGLSurface surface;

#define _4CC(c1,c2,c3,c4) (((uint32_t)(c4)<<24)|((uint32_t)(c3)<<16)|((uint32_t)(c2)<<8)|(uint32_t)(c1))
#define PAGE_SIZE (1 << 12)

static unsigned int alignToPageSize(unsigned int v)
{
	return (v + (PAGE_SIZE-1)) & ~(PAGE_SIZE-1);
}

#define CheckError(func) do { EGLint result = eglGetError(); if(result != EGL_SUCCESS) { printf("EGL error in %s: %x\n", func, result); return 1;} /*else printf("%s OK\n", func);*/ } while (0)

EGLint const attribute_list[] = {
		EGL_RED_SIZE,        8,
		EGL_GREEN_SIZE,      8,
		EGL_BLUE_SIZE,       8,
		EGL_ALPHA_SIZE,      0,
		EGL_DEPTH_SIZE,      0,
		EGL_STENCIL_SIZE,    0,
		EGL_SAMPLE_BUFFERS,  0,
		EGL_SAMPLES,         0,
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
};

EGLint ctxattr[] = {
	EGL_CONTEXT_CLIENT_VERSION, 2,
	EGL_NONE
};

struct fb_var_screeninfo screeninfo;

int initFB() {
	int fd;

	fd = open("/dev/fb0",O_RDWR);
	if (fd < 0)
	{
		printf("Could not open fb0\n");
		return 1;
	}

	if ( ioctl(fd, FBIOGET_VSCREENINFO, &screeninfo) != 0 ) {
		printf("Query screeninfo failed\n");
		return 1;
	}

	// Unblank the fbs
	if (ioctl(fd, FBIOBLANK, 0) < 0)
	{
		printf("Error while unblanking\n");
		return 1;
	}

	close(fd);
	return 0;
}


int initEGL() {
	EGLNativeDisplayType native_display;
	EGLConfig config;
	NativeWindowType native_window;
	EGLint num_config;

	/* get an EGL display connection */
	native_display = fbGetDisplayByIndex(0);
	CheckError("fbGetDisplayByIndex");

	/* create a native window */
	native_window = fbCreateWindow(native_display, 0, 0, screeninfo.xres, screeninfo.yres);
	//native_window = open("/dev/fb0", O_RDWR);
	CheckError("fbCreateWindow");

	display = eglGetDisplay(native_display);
	CheckError("eglGetDisplay");

	/* initialize the EGL display connection */
	eglInitialize(display, NULL, NULL);
	CheckError("eglInitialize");

	/* get an appropriate EGL frame buffer configuration */
	eglChooseConfig(display, attribute_list, NULL, 0, &num_config);
	CheckError("eglChooseConfig");

	if ( num_config == 0 ) {
		printf("No appropriate configs\n");
		return 1;
	}

	eglChooseConfig(display, attribute_list, &config, 1, &num_config);
	CheckError("eglChooseConfig");

	/* create an EGL window surface */
	surface = eglCreateWindowSurface(display, config, native_window, NULL);
	CheckError("eglCreateWindowSurface");

	/* create an EGL rendering context */
	context = eglCreateContext(display, config, NULL, ctxattr);
	CheckError("eglCreateContext");

	/* connect the context to the surface */
	eglMakeCurrent(display, surface, surface, context);
	CheckError("eglMakeCurrent");

	return 0;
}


int destroyEGL() {
	eglDestroyContext(display, context);
	CheckError("eglDestroyContext");

	eglDestroySurface(display, surface);
	CheckError("eglDestroySurface");

	eglTerminate(display);
	CheckError("eglTerminate");

	return 0;
}


GLuint loadShader(const char *shader_source, GLenum type) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &shader_source, NULL);
	glCompileShader(shader);
	return shader;
}


const char vertex_src [] =
"                                        \
   attribute vec4 position;              \
   attribute vec2 tc;                    \
   varying vec2 frag_tc;                 \
                                         \
   void main()                           \
   {                                     \
      frag_tc = tc;                      \
      gl_Position = position;            \
   }                                     \
";


const char fragment_src [] =
"                                                      \
   uniform sampler2D tex;                              \
   varying vec2 frag_tc;                               \
                                                       \
   void  main()                                        \
   {                                                   \
      gl_FragColor = texture2D(tex, frag_tc);          \
   }                                                   \
";

GLint position_loc, texture_loc;
GLfloat vertices[] = {
	-1.0,  1.0,  0.0,
	 1.0,  1.0,  0.0,
	-1.0, -1.0,  0.0,
	 1.0, -1.0,  0.0
};

GLfloat texCoords[] = {
	0.0, 0.0,
	1.0, 0.0,
	0.0, 1.0,
	1.0, 1.0
};

class Queue {
	public:
		Queue() {}
		~Queue() {
			for ( size_t i = 0; i < m_buffer.size(); ++i )
				SAFE_RELEASE(m_buffer[i].IMXBuffer);
		}

		void SetCapacity(int input) {
			m_buffer.resize(input);
			for ( size_t i = 0; i < m_buffer.size(); ++i )
				m_buffer[i].IMXBuffer = NULL;

			// Reset ring buffer
			m_beginInput = m_endInput = m_bufferedInput = 0;
			m_bClosed = false;
		}

		// Returns false if Queue is closed
		bool Push(const DVDVideoPicture &p) {
			CSingleLock lk(m_monitor);

			// If the input queue is full, wait for a free slot
			while ( (m_bufferedInput == m_buffer.size()) && !m_bClosed )
				m_inputNotFull.wait(lk);

			if ( m_bClosed ) {
				m_inputNotEmpty.notifyAll();
				return false;
			}

			// Store the value
			m_buffer[m_endInput] = p;
			// Lock the buffer
			p.IMXBuffer->Lock();
			m_endInput = (m_endInput+1) % m_buffer.size();
			++m_bufferedInput;
			m_inputNotEmpty.notifyAll();

			return true;
		}

		// Returns false if Queue is closed otherwise blocks
		// until output is available
		bool Pop(DVDVideoPicture &p) {
			CSingleLock lk(m_monitor);

			while ( m_bufferedInput == 0 && !m_bClosed ) {
				m_inputNotEmpty.wait(lk);
			}

			if ( m_bClosed )
				return false;

			p = m_buffer[m_beginInput];
			m_buffer[m_beginInput].IMXBuffer = NULL;
			m_beginInput = (m_beginInput+1) % m_buffer.size();
			--m_bufferedInput;
			m_inputNotFull.notifyAll();

			return true;
		}

		void Close() {
			if ( m_bClosed ) return;
			m_bClosed = true;
			m_inputNotFull.notifyAll();
			m_inputNotEmpty.notifyAll();
		}

	private:
		typedef vector<DVDVideoPicture> PictureBuffer;

		PictureBuffer                   m_buffer;
		volatile int                    m_beginInput, m_endInput;
		volatile size_t                 m_bufferedInput;
		XbmcThreads::ConditionVariable  m_inputNotEmpty;
		XbmcThreads::ConditionVariable  m_inputNotFull;
		bool                            m_bClosed;

		mutable CCriticalSection        m_monitor;
};


class BufferIterator {
	public:
		BufferIterator(CDVDVideoCodec *codec, const char *fn) {
			_codec = codec;
			_fp = fopen(fn, "rb");
			if ( _fp != NULL ) {
				CDVDStreamInfo hints;
				CDVDCodecOptions options;
				bool ok = true;
				ok = (fread(&hints.software, sizeof(hints.software), 1, _fp) == 1) && ok;
				ok = (fread(&hints.codec, sizeof(hints.codec), 1, _fp) == 1) && ok;
				ok = (fread(&hints.profile, sizeof(hints.profile), 1, _fp) == 1) && ok;
				ok = (fread(&hints.codec_tag, sizeof(hints.codec_tag), 1, _fp) == 1) && ok;
				ok = (fread(&hints.extrasize, sizeof(hints.extrasize), 1, _fp) == 1) && ok;
				if ( !ok ) {
					fclose(_fp);
					_fp = NULL;
					cerr << "Invalid header" << endl;
				}
				else {
					cerr << "Reading extradata with " << hints.extrasize << " bytes" << endl;
					hints.extradata = hints.extrasize > 0?malloc(hints.extrasize):NULL;
					if ( (hints.extradata != NULL) && (fread(hints.extradata, 1, hints.extrasize, _fp) != hints.extrasize) ) {
						fclose(_fp);
						_fp = NULL;
						cerr << "Could not read extradata" << endl;
					}
					else if ( !_codec->Open(hints, options) ) {
						cerr << "Could not open codec" << endl;
						fclose(_fp);
						_fp = NULL;
					}
				}
			}

			memset(&_pic, 0, sizeof(DVDVideoPicture));
			_state = VC_BUFFER;
		}

		~BufferIterator() {
			if ( _fp != NULL )
				fclose(_fp);
		}

		DVDVideoPicture *next() {
			while ( true ) {
				if ( _state & VC_BUFFER ) {
					double dts, pts;
					int size;

					if ( _fp == NULL ) {
						cerr << "Invalid file handle" << endl;
						return NULL;
					}

					bool ok = true;
					ok = (fread(&dts, sizeof(dts), 1, _fp) == 1) && ok;
					ok = (fread(&pts, sizeof(pts), 1, _fp) == 1) && ok;
					ok = (fread(&size, sizeof(size), 1, _fp) == 1) && ok;

					if ( !ok || (size <= 0) ) {
						if ( !feof(_fp) )
							cerr << "Wrong chunk header" << endl;
						return NULL;
					}

					if ( _buf.size() < size ) _buf.reserve(size);
					_buf.resize(size);

					if ( fread(&_buf[0], sizeof(BYTE), (int)_buf.size(), _fp) != (int)_buf.size() ) {
						cerr << "Failed to read chunk" << endl;
						return NULL;
					}

					// Call decode
					_state = _codec->Decode(&_buf[0], (int)_buf.size(), dts, pts);
				}
				else if ( !_state )
					_state = _codec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);

				if ( _state & VC_ERROR )
					return NULL;

				if ( _state & VC_PICTURE ) {
					_state &= ~VC_PICTURE;
					_codec->ClearPicture(&_pic);
					if ( _codec->GetPicture(&_pic) )
						return &_pic;
					else {
						CLog::Log(LOGWARNING, "Decoder::GetPicture failed: reset");
						_codec->Reset();
					}
				}
			}

			return NULL;
		}

	private:
		CDVDVideoCodec  *_codec;
		DVDVideoPicture  _pic;
		FILE            *_fp;
		vector<BYTE>     _buf;
		int              _state;
};


class OutputBase : private CThread {
	protected:
		OutputBase(Queue *q) : CThread("Output"), m_queue(q) {}

	public:
		void Start() {
			Create();
		}

		bool Wait() {
			return WaitForThreadExit(10000);
		}


	protected:
		virtual bool Init() = 0;

		virtual bool Run() {
			DVDVideoPicture p;

			while ( m_queue->Pop(p) ) {
				if ( !Ouput(p) )
					return false;
			}

			return true;
		}

		virtual void Done() = 0;

		// Return false to break the run loop
		virtual bool Ouput(DVDVideoPicture &) = 0;

	private:
		virtual void Process() {
			if ( !Init() ) return;
			Run();
			Done();
			m_queue->Close();
		}

	private:
		Queue *m_queue;
};


class Stats : public OutputBase {
	public:
		Stats(Queue *q) : OutputBase(q) {}

		virtual bool Init() {
			m_frameCount = 0;
			m_before = XbmcThreads::SystemClockMillis();
			return true;
		}

		virtual void Done() {
			m_after = XbmcThreads::SystemClockMillis();

			if ( m_frameCount > 0 ) {
				cerr << "Decoding of " << m_frameCount << " frames took " << (m_after-m_before) << " ms" << endl;
				cerr << "Average frame decoding time: " << ((m_after-m_before)/m_frameCount) << " ms" << endl;
			}
			else
				cerr << "No frames rendered" << endl;
		}

		virtual bool Ouput(DVDVideoPicture &p) {
			if ( p.IMXBuffer != NULL )
				p.IMXBuffer->Release();

			//cerr << m_frameCount << endl;
			++m_frameCount;
			return true;
		}

	private:
		int                m_frameCount;
		unsigned long long m_before, m_after;
};


class EGL : public Stats {
	public:
		EGL(Queue *q) : Stats(q) {}

		virtual bool Init() {
			Stats::Init();

			if ( initFB() ) {
				cerr << "FB init failed" << endl;
				return false;
			}

			if ( initEGL() ) {
				cerr << "EGL init failed" << endl;
				return false;
			}

			m_lastBuffer = NULL;
			GLuint vertexShader = loadShader(vertex_src, GL_VERTEX_SHADER);
			GLuint fragmentShader = loadShader(fragment_src, GL_FRAGMENT_SHADER);
			GLuint shaderProg = glCreateProgram();
			glAttachShader(shaderProg, vertexShader);
			glAttachShader(shaderProg, fragmentShader);

			glLinkProgram(shaderProg);
			glUseProgram(shaderProg);

			position_loc = glGetAttribLocation(shaderProg, "position");
			texture_loc  = glGetAttribLocation(shaderProg, "tc");

			glGenTextures(1, &m_textureID);

			glClearColor(0.0,0.0,0.0,1.0);
			glViewport(0, 0, screeninfo.xres, screeninfo.yres);
			glEnable(GL_TEXTURE_2D);

			return true;
		}

		virtual void Done() {
			Stats::Done();
			destroyEGL();
			SAFE_RELEASE(m_lastBuffer);
		}

		virtual bool Ouput(DVDVideoPicture &p) {
			SAFE_RELEASE(m_lastBuffer);
			m_lastBuffer = p.IMXBuffer;
			m_lastBuffer->Lock();

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_textureID);

			GLuint physical = ~0u;
			GLvoid *virt = (GLvoid*)p.IMXBuffer->pVirtAddr;
			GLenum format = GL_INVALID_ENUM;

			switch ( p.IMXBuffer->iFormat ) {
				case 0:
					format = GL_VIV_I420;
					break;
				case 1:
					format = GL_VIV_NV12;
					break;
				case 2:
					format = GL_RGB565;
					break;
				case 3:
					format = GL_RGBA;
					break;
				default:
					cerr << "Unsupported buffer format" << endl;
					break;
			}

			if ( format != GL_INVALID_ENUM ) {
				glTexDirectVIVMap(GL_TEXTURE_2D, p.IMXBuffer->iWidth,
				                  p.IMXBuffer->iHeight, format,
				                  &virt, &physical);

				glTexDirectInvalidateVIV(GL_TEXTURE_2D);
				glVertexAttribPointer(position_loc, 3, GL_FLOAT, false, 0, vertices);
				glVertexAttribPointer(texture_loc, 2, GL_FLOAT, false, 0, texCoords);
				glEnableVertexAttribArray(position_loc);
				glEnableVertexAttribArray(texture_loc);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}

			eglSwapBuffers(display, surface);

			return Stats::Ouput(p);
		}

	private:
		GLuint                   m_textureID;
		CDVDVideoCodecIMXBuffer *m_lastBuffer;
};



class FB : public Stats {
	public:
		FB(Queue *q) : Stats(q), _fd(-1), _ipu(-1), _lastBuffer(NULL) {}

		virtual bool Init() {
			Stats::Init();

			_numPages = 1;
			const char *fb = getenv("FB_MULTI_BUFFER");
			if ( fb != NULL ) {
				_numPages = atoi(fb);
				if ( _numPages < 1 || _numPages > 3 ) {
					cerr << "Invalid FB_MULTI_BUFFER value, falling back to default" << endl;
					_numPages = 1;
				}
			}

			_ipu = open("/dev/mxc_ipu", O_RDWR, 0);
			if ( _ipu < 0 ) {
				cerr << "Unable to open /dev/mxc_ipu" << endl;
				return false;
			}

			int screenWidth, screenHeight;
			if ( !GetResolution(0, screenWidth, screenHeight) ) {
				cerr << "Query for screen resolution failed" << endl;
				cleanup();
				return false;
			}

			const char *deviceName = "/dev/fb1";
			// Open Framebuffer and gets its address
			_fd = open(deviceName, O_RDWR | O_NONBLOCK, 0);
			if ( _fd < 0 ) {
				cerr << "Unable to open " << deviceName << endl;
				cleanup();
				return false;
			}

			if ( ioctl(_fd, FBIOGET_VSCREENINFO, &_vInitScreenInfo) < 0 ) {
				cerr << "Failed to get variable screen info" << endl;
				cleanup();
				return false;
			}

			memcpy(&_vScreenInfo, &_vInitScreenInfo, sizeof(_vScreenInfo));

			_vScreenInfo.xoffset = 0;
			_vScreenInfo.yoffset = 0;
			_vScreenInfo.bits_per_pixel = 16;
			_vScreenInfo.nonstd = v4l2_fourcc('U', 'Y', 'V', 'Y');
			//_vScreenInfo.nonstd = v4l2_fourcc('R', 'G', 'B', 'P');
			_vScreenInfo.activate = FB_ACTIVATE_NOW;
			_vScreenInfo.xres = screenWidth;
			_vScreenInfo.yres = screenHeight;
			_vScreenInfo.yres_virtual = _vScreenInfo.yres * _numPages;
			_vScreenInfo.xres_virtual = _vScreenInfo.xres;

			if ( ioctl(_fd, FBIOPUT_VSCREENINFO, &_vScreenInfo) < 0 ) {
				cerr << "Failed to set variable screen info" << endl;
				cleanup();
				return false;
			}

			if ( ioctl(_fd, FBIOGET_FSCREENINFO, &_fScreenInfo) < 0 ) {
				cerr << "Failed to get fixed screen info" << endl;
				cleanup();
				return false;
			}

			_fbSize = alignToPageSize(_fScreenInfo.line_length * _vScreenInfo.yres_virtual);

			if ( ioctl(_fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0 ) {
				cerr << "Unblanking failed" << endl;
				cleanup();
				return false;
			}

			_fbPageSize = _fbSize / _numPages;

			cerr << "Render page: " << _numPages << endl;
			cerr << "Visible resolution: " << _vScreenInfo.xres << "x"
			     << _vScreenInfo.yres
			     << "@" << _vScreenInfo.bits_per_pixel << endl;
			cerr << "Virtual resolution: " << _vScreenInfo.xres_virtual << "x"
			     << _vScreenInfo.yres_virtual
			     << "@" << _vScreenInfo.bits_per_pixel << endl;

			_currentPage = 0;

			_enableDeinterlacing = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode == VS_DEINTERLACEMODE_FORCE;

			// Switch deinterlacing always off since it is done in the renderer
			CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_OFF;

			return true;
		}

		virtual bool Ouput(DVDVideoPicture &p) {
			int ret;

			CDVDVideoCodecIMXBuffer *buf = p.IMXBuffer;

			SAFE_RELEASE(_lastBuffer);
			_lastBuffer = buf;
			_lastBuffer->Lock();

			struct ipu_task task;
			memset(&task, 0, sizeof(task));

			task.input.width = buf->iWidth;
			task.input.height = buf->iHeight;

			switch ( buf->iFormat ) {
				case 0:
					task.input.format = IPU_PIX_FMT_YUV420P;
					break;
				case 1:
					task.input.format = IPU_PIX_FMT_NV12;
					break;
				case 2:
					task.input.format = IPU_PIX_FMT_RGB565;
					break;
				case 3:
					task.input.format = IPU_PIX_FMT_RGB32;
					break;
				default:
					Stats::Ouput(p);
					cerr << "Unknown buffer format " << buf->iFormat << endl;
					return false;
			}

			task.input.paddr = (int)buf->pPhysAddr;

			// Setup deinterlacing if enabled
			if ( _enableDeinterlacing ) {
				task.input.deinterlace.enable = 1;
				task.input.deinterlace.motion = HIGH_MOTION;

				switch ( buf->GetFieldType() ) {
					case VPU_FIELD_TOP:
					case VPU_FIELD_TB:
						task.input.deinterlace.field_fmt |= IPU_DEINTERLACE_FIELD_TOP;
						break;
					case VPU_FIELD_BOTTOM:
					case VPU_FIELD_BT:
						task.input.deinterlace.field_fmt |= IPU_DEINTERLACE_FIELD_BOTTOM;
						break;
					default:
						break;
				}

				if ( doubleRate )
					task.input.deinterlace.field_fmt |= IPU_DEINTERLACE_RATE_EN;
			}

			task.output.width = _vScreenInfo.xres;
			task.output.height = _vScreenInfo.yres;
			//task.output.format  = v4l2_fourcc('R', 'G', 'B', 'P');
			task.output.format  = v4l2_fourcc('U', 'Y', 'V', 'Y');
			//task.output.paddr   = _fScreenInfo.smem_start+_currentPage*_fScreenInfo.line_length*_vScreenInfo.yres;
			//task.output.paddr   = _fScreenInfo.smem_start;
			task.output.paddr = _fScreenInfo.smem_start + _currentPage*_fbPageSize;

			// Setup test cropping for testing
			/*
			task.output.crop.pos.x = task.output.width / 4;
			task.output.crop.pos.y = task.output.height / 4;
			task.output.crop.w     = task.output.width / 2;
			task.output.crop.h     = task.output.height / 2;
			*/

			ret = IPU_CHECK_ERR_INPUT_CROP;
			while ( ret != IPU_CHECK_OK && ret > IPU_CHECK_ERR_MIN ) {
				ret = ioctl(_ipu, IPU_CHECK_TASK, &task);
				switch ( ret ) {
					case IPU_CHECK_OK:
						break;
					case IPU_CHECK_ERR_SPLIT_INPUTW_OVER:
						task.input.crop.w -= 8;
						break;
					case IPU_CHECK_ERR_SPLIT_INPUTH_OVER:
						task.input.crop.h -= 8;
						break;
					case IPU_CHECK_ERR_SPLIT_OUTPUTW_OVER:
						task.output.crop.w -= 8;
						break;
					case IPU_CHECK_ERR_SPLIT_OUTPUTH_OVER:
						task.output.crop.h -= 8;
						break;
					default:
						cerr << "Unknown IPU check error: " << ret << endl;
						Stats::Ouput(p);
						return false;
				}
			}

			if ( (ret = ioctl(_ipu, IPU_QUEUE_TASK, &task)) < 0 )
				cerr << "IPU task error: " << strerror(ret) << endl;

			SwapPages();

			if ( _enableDeinterlacing && doubleRate ) {
				// Lock buffer since it is release in Stats::Output
				buf->Lock();

				task.input.deinterlace.field_fmt |= IPU_DEINTERLACE_RATE_FRAME1;
				task.output.paddr = _fScreenInfo.smem_start + _currentPage*_fbPageSize;

				if ( (ret = ioctl(_ipu, IPU_QUEUE_TASK, &task)) < 0 )
					cerr << "IPU task error: " << strerror(ret) << endl;

				SwapPages();

				Stats::Ouput(p);
			}

			return Stats::Ouput(p);
		}

		virtual void Done() {
			cleanup();
			SAFE_RELEASE(_lastBuffer);
			Stats::Done();
		}

	private:
		bool GetResolution(int number, int &width, int &height) {
			int fd;
			struct fb_var_screeninfo fb_var;
			char deviceName[20] ;

			sprintf(deviceName, "/dev/fb%d", number);

			if ( (fd = open(deviceName, O_RDWR, 0)) < 0 ) {
				cerr << "Unable to open " << deviceName << endl;
				return false;
			}

			if ( ioctl(fd, FBIOGET_VSCREENINFO, &fb_var) < 0 ) {
				cerr << "Failed to set variable screen info" << endl;
				close(fd);
				return false;
			}

			close(fd);

			width = fb_var.xres;
			height = fb_var.yres;

			return true;
		}

		void SwapPages() {
			int ret;
			int nextPage = _currentPage + 1;
			if ( nextPage >= _numPages )
				nextPage = 0;

			if ( nextPage != _currentPage ) {
				_vScreenInfo.activate = FB_ACTIVATE_VBL;
				_vScreenInfo.yoffset = _vScreenInfo.yres*_currentPage;
				if ( (ret = ioctl(_fd, FBIOPAN_DISPLAY, &_vScreenInfo)) < 0 ) {
					cerr << "Panning failed: " << strerror(ret) << endl;
				}
			}

			_currentPage = nextPage;
		}

		void cleanup() {
			if ( _ipu >= 0 ) {
				close(_ipu);
				_ipu = -1;
			}

			if ( _fd >= 0 ) {
				ioctl(_fd, FBIOPUT_VSCREENINFO, &_vInitScreenInfo);
				ioctl(_fd, FBIOBLANK, 1);
				close(_fd);
				_fd = -1;
			}
		}


	private:
		int                       _fd;
		int                       _ipu;
		int                       _numPages;
		int                       _currentPage;
		unsigned int              _fbSize;
		int                       _fbPageSize;
		bool                      _enableDeinterlacing;
		struct fb_var_screeninfo  _vInitScreenInfo;
		struct fb_var_screeninfo  _vScreenInfo;
		struct fb_fix_screeninfo  _fScreenInfo;
		CDVDVideoCodecIMXBuffer  *_lastBuffer;
};


template <typename T>
void test(const char *filename) {
	CDVDVideoCodecIMX codec;
	Queue queue;
	BufferIterator it(&codec, filename);
	DVDVideoPicture *pic;
	T out(&queue);

	queue.SetCapacity(3);
	out.Start();

	while ( !appExit && ((pic = it.next()) != NULL) )
		if ( !queue.Push(*pic) ) break;

	queue.Close();
	out.Wait();
}

int main (int argc, char *argv[]) {
	if ( argc < 2 ) {
		printf("Need stream dump file\n");
		return 1;
	}

	bool progressiveTest = false;
	bool deinterlacedTest = false;
	bool outNull = false;
	bool outGL = false;
	bool outFB = false;

	signal(SIGINT, signal_handler);

	CLog::Init("./");
	CLog::SetLogLevel(LOG_LEVEL_DEBUG);

	for ( int i = 1; i < argc; ++i ) {
		const char *parg = argv[i-1];
		if ( !strcmp(parg, "--vscale") ) {
			double scale = atof(argv[i]);
			cerr << "Set quad size to " << scale << endl;
			for ( int v = 0; v < 4; ++v ) {
				vertices[v*3] *= scale;
				vertices[v*3+1] *= scale;
			}
		}
		if ( !strcmp(parg, "--tscale") ) {
			double scale = atof(argv[i]);
			cerr << "Set texture map area to " << scale << endl;
			for ( int t = 0; t < 4; ++t ) {
				texCoords[t*2] *= scale;
				texCoords[t*2+1] *= scale;
			}
		}
		if ( !strcmp(parg, "--vo") ) {
			if ( !strcmp(argv[i], "null") )
				outNull = true;
			else if ( !strcmp(argv[i], "gl") )
				outGL = true;
			else if ( !strcmp(argv[i], "fb") )
				outFB = true;
			else {
				cerr << "Unknown video output: " << argv[i] << endl;
				return 1;
			}
		}
		else if ( !strcmp(argv[i], "-p") )
			progressiveTest = true;
		else if ( !strcmp(argv[i], "-d") )
			deinterlacedTest = true;
		else if ( !strcmp(argv[i], "--doublerate") )
			doubleRate = true;
	}

	const char *filename = argv[1];

	if ( progressiveTest && outNull ) {
		cerr << "Set deinterlacing to OFF" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_OFF;
		test<Stats>(filename);
	}

	if ( deinterlacedTest && outNull ) {
		cerr << "Set deinterlacing to FORCE" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_FORCE;
		test<Stats>(filename);
	}

	if ( progressiveTest && outGL ) {
		cerr << "Set deinterlacing to OFF and render" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_OFF;
		test<EGL>(filename);
	}

	if ( deinterlacedTest && outGL ) {
		cerr << "Set deinterlacing to FORCE and render" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_FORCE;
		test<EGL>(filename);
	}

	if ( progressiveTest && outFB ) {
		cerr << "Set deinterlacing to OFF and render" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_OFF;
		test<FB>(filename);
	}

	if ( deinterlacedTest && outFB ) {
		cerr << "Set deinterlacing to FORCE and render" << endl;
		CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode = VS_DEINTERLACEMODE_FORCE;
		test<FB>(filename);
	}

	return 0;
}
