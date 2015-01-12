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


// Generell description of a buffer used by
// the IMX context, e.g. for blitting
class CIMXBuffer {
public:
  CIMXBuffer() : m_iRefs(0) {}

  // Shared pointer interface
  virtual void Lock() = 0;
  virtual long Release() = 0;
  virtual bool IsValid() = 0;

public:
  uint32_t     iWidth;
  uint32_t     iHeight;
  int          pPhysAddr;
  uint8_t     *pVirtAddr;
  int          iFormat;

protected:
  long         m_iRefs;
};


class CDVDVideoCodecIMXIPUBuffers;

// Base class of IMXVPU and IMXIPU buffer
class CDVDVideoCodecIMXBuffer : public CIMXBuffer {
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIMXBuffer(CDVDVideoCodecIMXIPUBuffers *p, int idx);
#else
  CDVDVideoCodecIMXBuffer(CDVDVideoCodecIMXIPUBuffers *p);
#endif

  // Detaches the buffer from its parent
  void          Detach();

  void          SetPts(double pts);
  double        GetPts() const { return m_pts; }

  void          SetDts(double dts);
  double        GetDts() const { return m_dts; }

  void          SetDropped(bool d) { m_drop = d; }
  bool          Dropped() const { return m_drop; }

  // Shows the buffer. Only IPU buffers can be shown.
  virtual bool  Show() = 0;

  bool          bDoubled;

protected:
#ifdef TRACE_FRAMES
  int           m_idx;
#endif
  CDVDVideoCodecIMXIPUBuffers *m_parent;

private:
  double        m_pts;
  double        m_dts;
  bool          m_drop;
};


class CDVDVideoCodecIMXVPUBuffer : public CDVDVideoCodecIMXBuffer
{
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIMXVPUBuffer(CDVDVideoCodecIMXIPUBuffers *p, int idx);
#else
  CDVDVideoCodecIMXVPUBuffer(CDVDVideoCodecIMXIPUBuffers *p);
#endif

  // reference counting
  virtual void                Lock();
  virtual long                Release();
  virtual bool                IsValid();

  virtual bool                Show();

  bool                        Rendered() const;
  void                        Queue(VpuDecOutFrameInfo *frameInfo,
                                    CDVDVideoCodecIMXVPUBuffer *previous);
  VpuDecRetCode               ReleaseFramebuffer(VpuDecHandle *handle);
  CDVDVideoCodecIMXVPUBuffer *GetPreviousBuffer() const { return m_previousBuffer; }
  VpuFieldType                GetFieldType() const { return m_fieldType; }
  bool                        IsInterlaced() const { return m_fieldType != VPU_FIELD_NONE; }

private:
  // private because we are reference counted
  virtual                     ~CDVDVideoCodecIMXVPUBuffer();

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
  CDVDVideoCodecIMXIPUBuffer(CDVDVideoCodecIMXIPUBuffers *p, int idx);
#else
  CDVDVideoCodecIMXIPUBuffer(CDVDVideoCodecIMXIPUBuffers *p);
#endif

  // reference counting
  virtual void Lock();
  virtual long Release();
  virtual bool IsValid();

  // Returns whether the buffer is ready to be used
  bool         Rendered() const { return m_bFree; }
  void         GrabFrameBuffer() { m_bFree = false; }
  void         ReleaseFrameBuffer();

  bool         Show();

private:
  virtual      ~CDVDVideoCodecIMXIPUBuffer();

public:
  int          iPage;

private:
  bool         m_bFree;
};


// iMX context class that handles all iMX hardware
// related stuff
class CIMXContext
{
public:
  CIMXContext();
  ~CIMXContext();

  bool     Configure(int pages = 2);
  bool     Close();

  bool     Blank();
  bool     Unblank();
  bool     SetVSync(bool enable);

  bool     IsValid() const         { return m_fbPages > 0; }

  // Returns the number of available pages
  int      PageCount() const       { return m_fbPages; }

  // Populates a CIMXBuffer with attributes of a page
  bool     GetPageInfo(CIMXBuffer *info, int page);

  // Blitter configuration
  void     SetDeInterlacing(bool flag);
  void     SetDoubleRate(bool flag);
  bool     DoubleRate() const;
  void     SetInterpolatedFrame(bool flag);

  void     SetBlitRects(const CRect &srcRect, const CRect &dstRect);

  // Blits a buffer to a particular page.
  // source_p (previous buffer) is required for de-interlacing
  // modes LOW_MOTION and MED_MOTION.
  bool     Blit(int targetPage, CIMXBuffer *source_p,
                CIMXBuffer *source,
                bool topBottomFields = true);

  // Shows a page vsynced
  bool     ShowPage(int page);

  // Clears the pages or a single page with 'black'
  void     Clear(int page = -1);

private:
  int                          m_fbHandle;
  int                          m_fbPages;
  int                          m_fbWidth;
  int                          m_fbHeight;
  int                          m_fbLineLength;
  int                          m_fbPageSize;
  int                          m_fbPhysSize;
  int                          m_fbPhysAddr;
  uint8_t                     *m_fbVirtAddr;
  struct fb_var_screeninfo     m_fbVar;
  int                          m_ipuHandle;
  int                          m_currentFieldFmt;
  bool                         m_vsync;
  bool                         m_deInterlacing;
  CRect                        m_srcRect;
  CRect                        m_dstRect;
  CRectInt                     m_inputRect;
  CRectInt                     m_outputRect;
  CRectInt                    *m_pageCrops;
  CCriticalSection             m_rectLock;
};


extern CIMXContext g_IMXContext;


// Collection class that manages a pool of IPU buffers that are used for
// rendering. In future they can also serve rotation or color conversion
// buffers.
class CDVDVideoCodecIMXIPUBuffers
{
public:
  CDVDVideoCodecIMXIPUBuffers();
  ~CDVDVideoCodecIMXIPUBuffers();

  bool Init(int numBuffers);
  // Sets the mode to be used if deinterlacing is set to AUTO
  bool Reset();
  bool Close();

  CDVDVideoCodecIMXIPUBuffer *
  Process(CDVDVideoCodecIMXVPUBuffer *sourceBuffer);

  bool Show(CDVDVideoCodecIMXIPUBuffer*);

private:
  bool Blit(CDVDVideoCodecIMXIPUBuffer *target,
            CDVDVideoCodecIMXVPUBuffer *source);

private:
  CDVDVideoCodecIMXIPUBuffer  *m_displayBuffer;
  bool                         m_autoMode;
  int                          m_bufferNum;
  CDVDVideoCodecIMXIPUBuffer **m_buffers;
};


class CDVDVideoMixerIMX : private CThread
{
public:
  CDVDVideoMixerIMX(CDVDVideoCodecIMXIPUBuffers *proc);
  virtual ~CDVDVideoMixerIMX();

  void SetCapacity(int intput, int output);
  bool OutputFull();

  void SetDeInterlacingAutoMode(bool f) { m_deinterlacingAutoMode = f; }
  void Start();
  void Reset();
  void Dispose();
  bool IsActive();

  // This function blocks until an input slot is available.
  // It returns and output frame if available or NULL.
  CDVDVideoCodecIMXBuffer *Process(CDVDVideoCodecIMXVPUBuffer *input);

private:
  CDVDVideoCodecIMXVPUBuffer *GetNextInput();
  bool HasFreeInput();
  void WaitForFreeOutput();
  bool PushOutput(CDVDVideoCodecIMXBuffer *v);
  CDVDVideoCodecIMXIPUBuffer *ProcessFrame(CDVDVideoCodecIMXVPUBuffer *input);

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
  bool                            m_deinterlacingAutoMode;
};


class CDVDVideoCodecIMX : public CDVDVideoCodec
{
public:
  CDVDVideoCodecIMX();
  virtual ~CDVDVideoCodecIMX();

  // Methods from CDVDVideoCodec which require overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose();
  virtual int  Decode(BYTE *pData, int iSize, double dts, double pts);
  virtual void Reset();
  virtual bool ClearPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName() { return (const char*)m_pFormatName; }
  virtual unsigned GetAllowedReferences();

  static void Enter();
  static void Leave();

protected:
  bool VpuOpen();
  bool VpuAllocBuffers(VpuMemInfo *);
  bool VpuFreeBuffers();
  bool VpuAllocFrameBuffers();
  int  VpuFindBuffer(void *frameAddr);

  static const int             m_extraVpuBuffers;   // Number of additional buffers for VPU
  static const int             m_maxVpuDecodeLoops; // Maximum iterations in VPU decoding loop
  static CCriticalSection      m_codecBufferLock;   // Lock to protect buffers handled
                                                    // by both decoding and rendering threads
  CDVDStreamInfo               m_hints;             // Hints from demuxer at stream opening
  const char                  *m_pFormatName;       // Current decoder format name
  VpuDecOpenParam              m_decOpenParam;      // Parameters required to call VPU_DecOpen
  CDecMemInfo                  m_decMemInfo;        // VPU dedicated memory description
  VpuDecHandle                 m_vpuHandle;         // Handle for VPU library calls
  VpuDecInitInfo               m_initInfo;          // Initial info returned from VPU at decoding start
  bool                         m_dropState;         // Current drop state
  int                          m_vpuFrameBufferNum; // Total number of allocated frame buffers
  VpuFrameBuffer              *m_vpuFrameBuffers;   // Table of VPU frame buffers description
  CDVDVideoCodecIMXIPUBuffers  m_ipuFrameBuffers;   // Pool of buffers used for rendering
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
  double                       m_durationPts;       // Current duration of two subsequent frames
  bool                         m_frameReported;     // State whether the frame consumed event will be reported by libfslvpu
#ifdef DUMP_STREAM
  FILE                        *m_dump;
#endif
};
