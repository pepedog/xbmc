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
#include <imx-mm/vpu/vpu_wrapper.h>
#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "threads/CriticalSection.h"
#include "utils/BitstreamConverter.h"


//#define IMX_PROFILE
//#define TRACE_FRAMES

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
  int nVirtNum;
  void** virtMem;

  //phy mem info
  int nPhyNum;
  VpuMemDesc* phyMem;
};


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

  uint32_t      iWidth;
  uint32_t      iHeight;
  uint8_t      *pPhysAddr;
  uint8_t      *pVirtAddr;
  uint8_t       iFormat;

protected:
#ifdef TRACE_FRAMES
  int           m_idx;
#endif
  long          m_refs;

private:
  double        m_pts;
  double        m_dts;
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

private:
  // private because we are reference counted
  virtual                     ~CDVDVideoCodecIMXVPUBuffer();

private:
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
  bool                     Process(int fd, CDVDVideoCodecIMXVPUBuffer *buffer,
                                   VpuFieldType fieldType, int fieldFmt,
                                   bool lowMotion);
  void                     ReleaseFrameBuffer();

  bool                     Allocate(int fd, int width, int height, int nAlign);
  bool                     Free(int fd);

private:
  virtual                  ~CDVDVideoCodecIMXIPUBuffer();

private:
  bool                     m_bFree;
  int                      m_pPhyAddr;
  uint8_t                 *m_pVirtAddr;
  uint32_t                 m_iWidth;
  uint32_t                 m_iHeight;
  int                      m_nSize;
};


class CDVDVideoCodecIMX : public CDVDVideoCodec
{
  friend class CDVDVideoCodecIMXVPUBuffer;
  friend class CDVDVideoCodecIPUBuffer;

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
  CDVDVideoCodecIMXVPUBuffer **m_outputBuffers;     // Table of VPU output buffers
  CDVDVideoCodecIMXVPUBuffer  *m_lastBuffer;        // Keep track of previous VPU output buffer (needed by deinterlacing motion engin)
  VpuMemDesc                  *m_extraMem;          // Table of allocated extra Memory
  int                          m_frameCounter;      // Decoded frames counter
  bool                         m_usePTS;            // State whether pts out of decoding process should be used
  VpuDecOutFrameInfo           m_frameInfo;         // Store last VPU output frame info
  CBitstreamConverter         *m_converter;         // H264 annex B converter
  bool                         m_convert_bitstream; // State whether bitstream conversion is required
  int                          m_bytesToBeConsumed; // Remaining bytes in VPU
  double                       m_previousPts;       // Enable to keep pts when needed
  bool                         m_frameReported;     // State whether the frame consumed event will be reported by libfslvpu
  double                       m_dts;               // Current dts
};
