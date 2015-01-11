#pragma once
/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
#include <queue>
#include <vector>
#include <imx-mm/vpu/vpu_wrapper.h>
#include <linux/mxcfb.h>
#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "threads/CriticalSection.h"
#include "threads/Condition.h"
#include "threads/Event.h"
#include "threads/Thread.h"
#include "utils/BitstreamConverter.h"
#include "guilib/Geometry.h"


// The decoding format of the VPU buffer. Comment this to decode
// as NV12. The VPU works faster with NV12 in combination with
// deinterlacing.
// Progressive content seems to be handled faster with I420 whereas
// interlaced content is processed faster with NV12 as output format.
#define IMX_INPUT_FORMAT_I420

// This enables logging of times for Decode, Render->Render,
// Deinterlace. It helps to profile several stages of
// processing with respect to changed kernels or other configurations.
// Since we utilize VPU, IPU and GPU at the same time different kernel
// priorities to those subsystems can result in a very different user
// experience. With that setting enabled we can build some statistics,
// as numbers are always better than "feelings"
//#define IMX_PROFILE_BUFFERS

//#define IMX_PROFILE
//#define TRACE_FRAMES

// If uncommented a file "stream.dump" will be created in the current
// directory whenever a new stream is started. This is only for debugging
// and performance tests. This define must never be active in distributions.
//#define DUMP_STREAM

class CDecMemInfo
{
public:
  CDecMemInfo()
    : nVirtNum(0)
    , virtMem(NULL)
    , nPhyNum(0)
    , phyMem(NULL)
  {}

  //virtual mem info
  int         nVirtNum;
  void      **virtMem;

  //phy mem info
  int         nPhyNum;
  VpuMemDesc *phyMem;
};

class CDVDVideoCodecIMXIPUBuffers;

// Base class of IMXVPU and IMXIPU buffer
class CDVDVideoCodecIMXBuffer {
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIMXBuffer(int idx);
#else
  CDVDVideoCodecIMXBuffer();
#endif

  // reference counting
  virtual void  Lock() = 0;
  virtual long  Release() = 0;
  virtual bool  IsValid() = 0;

  void          SetPts(double pts);
  double        GetPts(void) const { return m_pts; }

  void          SetDts(double dts);
  double        GetDts(void) const { return m_dts; }

  void          SetDropped(bool d) { m_drop = d; }
  bool          Dropped() const { return m_drop; }

  // Shows the buffer. Only IPU buffers can be shown.
  virtual bool  Show() {}

  uint32_t      iWidth;
  uint32_t      iHeight;
  int           pPhysAddr;
  uint8_t      *pVirtAddr;
  int           iFormat;

protected:
#ifdef TRACE_FRAMES
  int           m_idx;
#endif
  long          m_refs;

private:
  double        m_pts;
  double        m_dts;
  bool          m_drop;
};


class CDVDVideoCodecIMXVPUBuffer : public CDVDVideoCodecIMXBuffer
{
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIMXVPUBuffer(int idx);
#else
  CDVDVideoCodecIMXVPUBuffer();
#endif

  // reference counting
  virtual void                Lock();
  virtual long                Release();
  virtual bool                IsValid();

  bool                        Rendered() const;
  void                        Queue(VpuDecOutFrameInfo *frameInfo,
                                    CDVDVideoCodecIMXVPUBuffer *previous);
  VpuDecRetCode               ReleaseFramebuffer(VpuDecHandle *handle);
  CDVDVideoCodecIMXVPUBuffer *GetPreviousBuffer() const;
  VpuFieldType                GetFieldType() const { return m_fieldType; }

private:
  // private because we are reference counted
  virtual                    ~CDVDVideoCodecIMXVPUBuffer();

private:
  VpuFieldType                m_fieldType;
  VpuFrameBuffer             *m_frameBuffer;
  bool                        m_rendered;
  CDVDVideoCodecIMXVPUBuffer *m_previousBuffer; // Holds a the reference counted
                                                // previous buffer
};


// Shared buffer that holds an IPU allocated memory block and serves as target
// for IPU operations such as deinterlacing, rotation or color conversion.
class CDVDVideoCodecIMXIPUBuffer : public CDVDVideoCodecIMXBuffer
{
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIMXIPUBuffer(int idx);
#else
  CDVDVideoCodecIMXIPUBuffer();
#endif

  // reference counting
  virtual void             Lock();
  virtual long             Release();
  virtual bool             IsValid();

  // Returns whether the buffer is ready to be used
  bool                     Rendered() const { return m_bFree; }
  void                     GrabFrameBuffer() { m_bFree = false; }
  void                     ReleaseFrameBuffer();

  void                     Set(CDVDVideoCodecIMXIPUBuffers *ipu,
                               int physAddr, uint8_t *virtAddr,
                               int w, int h, int format, int page);
  bool                     Show();

private:
  virtual                  ~CDVDVideoCodecIMXIPUBuffer();

public:
  CRectInt                     cropRect;
  int                          iPage;

private:
  CDVDVideoCodecIMXIPUBuffers *m_pIPU;
  bool                         m_bFree;
};


class RenderFB1
{
public:
  RenderFB1();
  ~RenderFB1();

  bool     Init(int pages = 2);
  bool     Close();

  bool     Blank();
  bool     Unblank();

  bool     IsValid() const         { return m_fbPages > 0; }
  int      Width() const           { return m_fbWidth; }
  int      Height() const          { return m_fbHeight; }
  int      Format() const          { return m_fbVar.nonstd; }
  int      PhysAddr(int p) const   { return m_fbPhysAddr + p*m_fbPageSize; }
  uint8_t *VirtAddr(int p) const   { return m_fbVirtAddr + p*m_fbPageSize; }

  // Shows a page vsynced
  bool     ShowPage(int page);

  // Clears the pages or a single page with 'black'
  void     Clear(int page = -1);

private:
  int                          m_fbHandle;
  int                          m_fbPages;
  int                          m_fbWidth;
  int                          m_fbHeight;
  int                          m_fbPageSize;
  int                          m_fbPhysSize;
  int                          m_fbPhysAddr;
  uint8_t                     *m_fbVirtAddr;
  struct fb_var_screeninfo     m_fbVar;
};


// Collection class that manages a pool of IPU buffers that are used for
// rendering. In future they can also serve rotation or color conversion
// buffers.
class CDVDVideoCodecIMXIPUBuffers
{
public:
  CDVDVideoCodecIMXIPUBuffers();
  ~CDVDVideoCodecIMXIPUBuffers();

  bool Init(int width, int height, int numBuffers, int nAlign);
  // Sets the mode to be used if deinterlacing is set to AUTO
  void SetAutoMode(bool mode) { m_autoMode = mode; }
  bool AutoMode() const { return m_autoMode; }
  void SetDoubleRate(bool flag);
  bool DoubleRate() const;
  void SetInterpolatedFrame(bool flag);
  bool Reset();
  bool Close();

  CDVDVideoCodecIMXIPUBuffer *
  Process(CDVDVideoCodecIMXVPUBuffer *sourceBuffer);

  bool ShowPage(int page);

private:
  bool Blit(CDVDVideoCodecIMXIPUBuffer *target,
            CDVDVideoCodecIMXVPUBuffer *source);

private:
  static RenderFB1             m_fb;
  int                          m_ipuHandle;
  bool                         m_autoMode;
  int                          m_bufferNum;
  CDVDVideoCodecIMXIPUBuffer **m_buffers;
  int                          m_currentFieldFmt;
};


class CDVDVideoMixerIMX : private CThread
{
public:
  CDVDVideoMixerIMX(CDVDVideoCodecIMXIPUBuffers *proc);
  virtual ~CDVDVideoMixerIMX();

  void SetCapacity(int intput, int output);
  bool DoubleRate() const { return m_proc->DoubleRate(); }

  void Start();
  void Reset();
  void Dispose();
  bool IsActive();

  // This function blocks until an input slot is available.
  // It returns if an output is available.
  CDVDVideoCodecIMXBuffer *Process(CDVDVideoCodecIMXVPUBuffer *input);

private:
  CDVDVideoCodecIMXVPUBuffer *GetNextInput();
  void WaitForFreeOutput();
  bool PushOutput(CDVDVideoCodecIMXBuffer *v);
  CDVDVideoCodecIMXBuffer *ProcessFrame(CDVDVideoCodecIMXVPUBuffer *input);

  virtual void OnStartup();
  virtual void OnExit();
  virtual void StopThread(bool bWait = true);
  virtual void Process();

private:
  typedef std::vector<CDVDVideoCodecIMXVPUBuffer*> InputBuffers;
  typedef std::vector<CDVDVideoCodecIMXBuffer*> OutputBuffers;

  CDVDVideoCodecIMXIPUBuffers    *m_proc;
  InputBuffers                    m_input;
  volatile int                    m_beginInput, m_endInput;
  volatile size_t                 m_bufferedInput;
  XbmcThreads::ConditionVariable  m_inputNotEmpty;
  XbmcThreads::ConditionVariable  m_inputNotFull;

  OutputBuffers                   m_output;
  volatile int                    m_beginOutput, m_endOutput;
  volatile size_t                 m_bufferedOutput;
  XbmcThreads::ConditionVariable  m_outputNotFull;

  mutable CCriticalSection        m_monitor;
  CDVDVideoCodecIMXBuffer        *m_lastFrame;    // Last input frame
};


class CDVDVideoCodecIMX : public CDVDVideoCodec
{
public:
  CDVDVideoCodecIMX();
  virtual ~CDVDVideoCodecIMX();

  // Methods from CDVDVideoCodec which require overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose(void);
  virtual int  Decode(BYTE *pData, int iSize, double dts, double pts);
  virtual void Reset(void);
  virtual bool ClearPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName(void) { return (const char*)m_pFormatName; }
  virtual unsigned GetAllowedReferences();

protected:
  bool VpuOpen();
  bool VpuAllocBuffers(VpuMemInfo *);
  bool VpuFreeBuffers();
  bool VpuAllocFrameBuffers();
  int  VpuFindBuffer(void *frameAddr);

  static const int             m_extraVpuBuffers;   // Number of additional buffers for VPU
  static const int             m_maxVpuDecodeLoops; // Maximum iterations in VPU decoding loop

  CDVDStreamInfo               m_hints;             // Hints from demuxer at stream opening
  const char                  *m_pFormatName;       // Current decoder format name
  VpuDecOpenParam              m_decOpenParam;      // Parameters required to call VPU_DecOpen
  CDecMemInfo                  m_decMemInfo;        // VPU dedicated memory description
  VpuDecHandle                 m_vpuHandle;         // Handle for VPU library calls
  VpuDecInitInfo               m_initInfo;          // Initial info returned from VPU at decoding start
  bool                         m_dropState;         // Current drop state
  int                          m_vpuFrameBufferNum; // Total number of allocated frame buffers
  VpuFrameBuffer              *m_vpuFrameBuffers;   // Table of VPU frame buffers description
  CDVDVideoCodecIMXIPUBuffers  m_deinterlacer;      // Pool of buffers used for deinterlacing
  CDVDVideoMixerIMX            m_mixer;
  CDVDVideoCodecIMXVPUBuffer **m_outputBuffers;     // Table of VPU output buffers
  CDVDVideoCodecIMXVPUBuffer  *m_lastBuffer;        // Keep track of previous VPU output buffer (needed by deinterlacing motion engin)
  CDVDVideoCodecIMXBuffer     *m_currentBuffer;
  VpuMemDesc                  *m_extraMem;          // Table of allocated extra Memory
  int                          m_frameCounter;      // Decoded frames counter
  bool                         m_usePTS;            // State whether pts out of decoding process should be used
  VpuDecOutFrameInfo           m_frameInfo;         // Store last VPU output frame info
  CBitstreamConverter         *m_converter;         // H264 annex B converter
  bool                         m_convert_bitstream; // State whether bitstream conversion is required
  int                          m_bytesToBeConsumed; // Remaining bytes in VPU
  double                       m_previousPts;       // Enable to keep pts when needed
  bool                         m_frameReported;     // State whether the frame consumed event will be reported by libfslvpu
#ifdef DUMP_STREAM
  FILE                        *m_dump;
#endif
};
