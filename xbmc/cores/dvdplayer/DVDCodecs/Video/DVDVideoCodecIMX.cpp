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

#include "DVDVideoCodecIMX.h"

#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/ipu.h>
#include "settings/MediaSettings.h"
#include "settings/VideoSettings.h"
#include "settings/AdvancedSettings.h"
#include "threads/SingleLock.h"
#include "threads/Atomics.h"
#include "utils/log.h"
#include "DVDClock.h"

#define IMX_VDI_MAX_WIDTH 968
#define FRAME_ALIGN 16
#define MEDIAINFO 1
#define _4CC(c1,c2,c3,c4) (((uint32_t)(c4)<<24)|((uint32_t)(c3)<<16)|((uint32_t)(c2)<<8)|(uint32_t)(c1))
#define Align(ptr,align)  (((unsigned int)ptr + (align) - 1)/(align)*(align))
#define Align2(ptr,align)  (((unsigned int)ptr)/(align)*(align))

// Experiments show that we need at least one more (+1) VPU buffer than the min value returned by the VPU
const int CDVDVideoCodecIMX::m_extraVpuBuffers = 6;
const int CDVDVideoCodecIMX::m_maxVpuDecodeLoops = 5;

bool CDVDVideoCodecIMX::VpuAllocBuffers(VpuMemInfo *pMemBlock)
{
  int i, size;
  void* ptr;
  VpuMemDesc vpuMem;
  VpuDecRetCode ret;

  for(i=0; i<pMemBlock->nSubBlockNum; i++)
  {
    size = pMemBlock->MemSubBlock[i].nAlignment + pMemBlock->MemSubBlock[i].nSize;
    if (pMemBlock->MemSubBlock[i].MemType == VPU_MEM_VIRT)
    { // Allocate standard virtual memory
      ptr = malloc(size);
      if(ptr == NULL)
      {
        CLog::Log(LOGERROR, "%s - Unable to malloc %d bytes.\n", __FUNCTION__, size);
        goto AllocFailure;
      }
      pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)Align(ptr, pMemBlock->MemSubBlock[i].nAlignment);

      m_decMemInfo.nVirtNum++;
      m_decMemInfo.virtMem = (void**)realloc(m_decMemInfo.virtMem, m_decMemInfo.nVirtNum*sizeof(void*));
      m_decMemInfo.virtMem[m_decMemInfo.nVirtNum-1] = ptr;
    }
    else
    { // Allocate contigous mem for DMA
      vpuMem.nSize = size;
      ret = VPU_DecGetMem(&vpuMem);
      if(ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - Unable alloc %d bytes of physical memory (%d).\n", __FUNCTION__, size, ret);
        goto AllocFailure;
      }
      pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)Align(vpuMem.nVirtAddr, pMemBlock->MemSubBlock[i].nAlignment);
      pMemBlock->MemSubBlock[i].pPhyAddr = (unsigned char*)Align(vpuMem.nPhyAddr, pMemBlock->MemSubBlock[i].nAlignment);

      m_decMemInfo.nPhyNum++;
      m_decMemInfo.phyMem = (VpuMemDesc*)realloc(m_decMemInfo.phyMem, m_decMemInfo.nPhyNum*sizeof(VpuMemDesc));
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nPhyAddr = vpuMem.nPhyAddr;
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nVirtAddr = vpuMem.nVirtAddr;
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nCpuAddr = vpuMem.nCpuAddr;
      m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nSize = size;
    }
  }

  return true;

AllocFailure:
  VpuFreeBuffers();
  return false;
}

int CDVDVideoCodecIMX::VpuFindBuffer(void *frameAddr)
{
  for (int i=0; i<m_vpuFrameBufferNum; i++)
  {
    if (m_vpuFrameBuffers[i].pbufY == frameAddr)
      return i;
  }
  return -1;
}

bool CDVDVideoCodecIMX::VpuFreeBuffers(void)
{
  VpuMemDesc vpuMem;
  VpuDecRetCode vpuRet;
  bool ret = true;

  if (m_decMemInfo.virtMem)
  {
    //free virtual mem
    for(int i=0; i<m_decMemInfo.nVirtNum; i++)
    {
      if (m_decMemInfo.virtMem[i])
        free((void*)m_decMemInfo.virtMem[i]);
    }
    free(m_decMemInfo.virtMem);
    m_decMemInfo.virtMem = NULL;
    m_decMemInfo.nVirtNum = 0;
  }

  if (m_decMemInfo.phyMem)
  {
    //free physical mem
    for(int i=0; i<m_decMemInfo.nPhyNum; i++)
    {
      vpuMem.nPhyAddr = m_decMemInfo.phyMem[i].nPhyAddr;
      vpuMem.nVirtAddr = m_decMemInfo.phyMem[i].nVirtAddr;
      vpuMem.nCpuAddr = m_decMemInfo.phyMem[i].nCpuAddr;
      vpuMem.nSize = m_decMemInfo.phyMem[i].nSize;
      vpuRet = VPU_DecFreeMem(&vpuMem);
      if(vpuRet != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - Errror while trying to free physical memory (%d).\n", __FUNCTION__, ret);
        ret = false;
      }
    }
    free(m_decMemInfo.phyMem);
    m_decMemInfo.phyMem = NULL;
    m_decMemInfo.nPhyNum = 0;
  }

  return ret;
}


bool CDVDVideoCodecIMX::VpuOpen(void)
{
  VpuDecRetCode  ret;
  VpuVersionInfo vpuVersion;
  VpuMemInfo     memInfo;
  VpuDecConfig config;
  int param;

  memset(&memInfo, 0, sizeof(VpuMemInfo));
  ret = VPU_DecLoad();
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU load failed with error code %d.\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  ret = VPU_DecGetVersionInfo(&vpuVersion);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU version cannot be read (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }
  else
  {
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "VPU Lib version : major.minor.rel=%d.%d.%d.\n", vpuVersion.nLibMajor, vpuVersion.nLibMinor, vpuVersion.nLibRelease);
  }

  ret = VPU_DecQueryMem(&memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
          CLog::Log(LOGERROR, "%s - iMX VPU query mem error (%d).\n", __FUNCTION__, ret);
          goto VpuOpenError;
  }
  VpuAllocBuffers(&memInfo);

  m_decOpenParam.nReorderEnable = 1;
#ifdef IMX_INPUT_FORMAT_I420
  m_decOpenParam.nChromaInterleave = 0;
#else
  m_decOpenParam.nChromaInterleave = 1;
#endif
  m_decOpenParam.nMapType = 0;
  m_decOpenParam.nTiled2LinearEnable = 0;
  m_decOpenParam.nEnableFileMode = 0;

  ret = VPU_DecOpen(&m_vpuHandle, &m_decOpenParam, &memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU open failed (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  config = VPU_DEC_CONF_SKIPMODE;
  param = VPU_DEC_SKIPNONE;
  ret = VPU_DecConfig(m_vpuHandle, config, &param);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU set skip mode failed  (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  config = VPU_DEC_CONF_BUFDELAY;
  param = 0;
  ret = VPU_DecConfig(m_vpuHandle, config, &param);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU set buffer delay failed  (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  config = VPU_DEC_CONF_INPUTTYPE;
  param = VPU_DEC_IN_NORMAL;
  ret = VPU_DecConfig(m_vpuHandle, config, &param);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU configure input type failed  (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  // Note that libvpufsl (file vpu_wrapper.c) associates VPU_DEC_CAP_FRAMESIZE
  // capability to the value of nDecFrameRptEnabled which is in fact directly
  // related to the ability to generate VPU_DEC_ONE_FRM_CONSUMED even if the
  // naming is misleading...
  ret = VPU_DecGetCapability(m_vpuHandle, VPU_DEC_CAP_FRAMESIZE, &param);
  m_frameReported = (param != 0);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU get framesize capability failed (%d).\n", __FUNCTION__, ret);
    m_frameReported = false;
  }

  return true;

VpuOpenError:
  Dispose();
  return false;
}

bool CDVDVideoCodecIMX::VpuAllocFrameBuffers(void)
{
  int totalSize = 0;
  int ySize     = 0;
  int uSize     = 0;
  int vSize     = 0;
  int mvSize    = 0;
  int yStride   = 0;
  int uvStride  = 0;

  VpuDecRetCode ret;
  VpuMemDesc vpuMem;
  unsigned char* ptr;
  unsigned char* ptrVirt;
  int nAlign;

  m_vpuFrameBufferNum = m_initInfo.nMinFrameBufferCount + m_extraVpuBuffers;
  m_vpuFrameBuffers = new VpuFrameBuffer[m_vpuFrameBufferNum];

  yStride = Align(m_initInfo.nPicWidth,FRAME_ALIGN);
  if(m_initInfo.nInterlace)
  {
    ySize = Align(m_initInfo.nPicWidth,FRAME_ALIGN)*Align(m_initInfo.nPicHeight,(2*FRAME_ALIGN));
  }
  else
  {
    ySize = Align(m_initInfo.nPicWidth,FRAME_ALIGN)*Align(m_initInfo.nPicHeight,FRAME_ALIGN);
  }

#ifdef IMX_INPUT_FORMAT_I420
  switch (m_initInfo.nMjpgSourceFormat)
  {
  case 0: // I420 (4:2:0)
    uvStride = yStride / 2;
    uSize = vSize = mvSize = ySize / 4;
    break;
  case 1: // Y42B (4:2:2 horizontal)
    uvStride = yStride / 2;
    uSize = vSize = mvSize = ySize / 2;
    break;
  case 3: // Y444 (4:4:4)
    uvStride = yStride;
    uSize = vSize = mvSize = ySize;
    break;
  default:
    CLog::Log(LOGERROR, "%s: invalid source format in init info\n",__FUNCTION__,ret);
    return false;
  }

#else
  // NV12
  uvStride = yStride;
  uSize    = ySize/2;
  mvSize   = uSize/2;
#endif

  nAlign = m_initInfo.nAddressAlignment;
  if(nAlign>1)
  {
    ySize = Align(ySize, nAlign);
    uSize = Align(uSize, nAlign);
    vSize = Align(vSize, nAlign);
    mvSize = Align(mvSize, nAlign);
  }

  m_outputBuffers = new CDVDVideoCodecIMXVPUBuffer*[m_vpuFrameBufferNum];

  for (int i=0 ; i < m_vpuFrameBufferNum; i++)
  {
    totalSize = ySize + uSize + vSize + mvSize + nAlign;

    vpuMem.nSize = totalSize;
    ret = VPU_DecGetMem(&vpuMem);
    if(ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s: vpu malloc frame buf failure: ret=%d \r\n",__FUNCTION__,ret);
      return false;
    }

    //record memory info for release
    m_decMemInfo.nPhyNum++;
    m_decMemInfo.phyMem = (VpuMemDesc*)realloc(m_decMemInfo.phyMem, m_decMemInfo.nPhyNum*sizeof(VpuMemDesc));
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nPhyAddr = vpuMem.nPhyAddr;
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nVirtAddr = vpuMem.nVirtAddr;
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nCpuAddr = vpuMem.nCpuAddr;
    m_decMemInfo.phyMem[m_decMemInfo.nPhyNum-1].nSize = vpuMem.nSize;

    //fill frameBuf
    ptr = (unsigned char*)vpuMem.nPhyAddr;
    ptrVirt = (unsigned char*)vpuMem.nVirtAddr;

    //align the base address
    if(nAlign>1)
    {
      ptr = (unsigned char*)Align(ptr,nAlign);
      ptrVirt = (unsigned char*)Align(ptrVirt,nAlign);
    }

    // fill stride info
    m_vpuFrameBuffers[i].nStrideY           = yStride;
    m_vpuFrameBuffers[i].nStrideC           = uvStride;

    // fill phy addr
    m_vpuFrameBuffers[i].pbufY              = ptr;
    m_vpuFrameBuffers[i].pbufCb             = ptr + ySize;
#ifdef IMX_INPUT_FORMAT_I420
    m_vpuFrameBuffers[i].pbufCr             = ptr + ySize + uSize;
#else
    m_vpuFrameBuffers[i].pbufCr             = 0;
#endif
    m_vpuFrameBuffers[i].pbufMvCol          = ptr + ySize + uSize + vSize;

    // fill virt addr
    m_vpuFrameBuffers[i].pbufVirtY          = ptrVirt;
    m_vpuFrameBuffers[i].pbufVirtCb         = ptrVirt + ySize;
#ifdef IMX_INPUT_FORMAT_I420
    m_vpuFrameBuffers[i].pbufVirtCr         = ptrVirt + ySize + uSize;
#else
    m_vpuFrameBuffers[i].pbufVirtCr         = 0;
#endif
    m_vpuFrameBuffers[i].pbufVirtMvCol      = ptrVirt + ySize + uSize + vSize;

    m_vpuFrameBuffers[i].pbufY_tilebot      = 0;
    m_vpuFrameBuffers[i].pbufCb_tilebot     = 0;
    m_vpuFrameBuffers[i].pbufVirtY_tilebot  = 0;
    m_vpuFrameBuffers[i].pbufVirtCb_tilebot = 0;

#ifdef TRACE_FRAMES
    m_outputBuffers[i] = new CDVDVideoCodecIMXVPUBuffer(i);
#else
    m_outputBuffers[i] = new CDVDVideoCodecIMXVPUBuffer();
#endif
  }

  CLog::Log(LOGNOTICE, "IMX: Initialize hardware deinterlacing\n");
  if (!m_deinterlacer.Init(m_initInfo.nPicWidth, m_initInfo.nPicHeight, GetAllowedReferences()+5, nAlign))
  {
    CLog::Log(LOGWARNING, "IMX: Failed to initialize IPU buffers: deinterlacing disabled\n");
  }

  m_deinterlacer.SetAutoMode(m_initInfo.nInterlace);

  return true;
}

CDVDVideoCodecIMX::CDVDVideoCodecIMX() : m_mixer(&m_deinterlacer)
{
  m_pFormatName = "iMX-xxx";
  m_vpuHandle = 0;
  m_vpuFrameBuffers = NULL;
  m_outputBuffers = NULL;
  m_lastBuffer = NULL;
  m_currentBuffer = NULL;
  m_extraMem = NULL;
  m_vpuFrameBufferNum = 0;
  m_dropState = false;
  m_convert_bitstream = false;
  m_frameCounter = 0;
  m_usePTS = true;
  if (getenv("IMX_NOPTS") != NULL)
  {
    m_usePTS = false;
  }
  m_converter = NULL;
  m_convert_bitstream = false;
  m_bytesToBeConsumed = 0;
  m_previousPts = DVD_NOPTS_VALUE;
  m_mixer.SetCapacity(3,3);
#ifdef DUMP_STREAM
  m_dump = NULL;
#endif
}

CDVDVideoCodecIMX::~CDVDVideoCodecIMX()
{
  Dispose();
}

bool CDVDVideoCodecIMX::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (hints.software)
  {
    CLog::Log(LOGNOTICE, "iMX VPU : software decoding requested.\n");
    return false;
  }

#ifdef DUMP_STREAM
  m_dump = fopen("stream.dump", "wb");
  if (m_dump != NULL)
  {
    fwrite(&hints.software, sizeof(hints.software), 1, m_dump);
    fwrite(&hints.codec, sizeof(hints.codec), 1, m_dump);
    fwrite(&hints.profile, sizeof(hints.profile), 1, m_dump);
    fwrite(&hints.codec_tag, sizeof(hints.codec_tag), 1, m_dump);
    fwrite(&hints.extrasize, sizeof(hints.extrasize), 1, m_dump);
    CLog::Log(LOGNOTICE, "Dump: HEADER: %d  %d  %d  %d  %d\n",
              hints.software, hints.codec, hints.profile,
              hints.codec_tag, hints.extrasize);
    if (hints.extrasize > 0)
      fwrite(hints.extradata, 1, hints.extrasize, m_dump);
  }
#endif

  m_hints = hints;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "Let's decode with iMX VPU\n");

#ifdef MEDIAINFO
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: fpsrate %d / fpsscale %d\n", m_hints.fpsrate, m_hints.fpsscale);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: CodecID %d \n", m_hints.codec);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: StreamType %d \n", m_hints.type);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Level %d \n", m_hints.level);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Profile %d \n", m_hints.profile);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: PTS_invalid %d \n", m_hints.ptsinvalid);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Tag %d \n", m_hints.codec_tag);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: %dx%d \n", m_hints.width,  m_hints.height);
  }
  { uint8_t *pb = (uint8_t*)&m_hints.codec_tag;
    if ((isalnum(pb[0]) && isalnum(pb[1]) && isalnum(pb[2]) && isalnum(pb[3])) && g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Tag fourcc %c%c%c%c\n", pb[0], pb[1], pb[2], pb[3]);
  }
  if (m_hints.extrasize)
  {
    char buf[4096];

    for (unsigned int i=0; i < m_hints.extrasize; i++)
      sprintf(buf+i*2, "%02x", ((uint8_t*)m_hints.extradata)[i]);

    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: extradata %d %s\n", m_hints.extrasize, buf);
  }
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: %d / %d \n", m_hints.width,  m_hints.height);
    CLog::Log(LOGDEBUG, "Decode: aspect %f - forced aspect %d\n", m_hints.aspect, m_hints.forced_aspect);
  }
#endif

  m_convert_bitstream = false;
  switch(m_hints.codec)
  {
  case CODEC_ID_MPEG1VIDEO:
    m_decOpenParam.CodecFormat = VPU_V_MPEG2;
    m_pFormatName = "iMX-mpeg1";
    break;
  case CODEC_ID_MPEG2VIDEO:
  case CODEC_ID_MPEG2VIDEO_XVMC:
    m_decOpenParam.CodecFormat = VPU_V_MPEG2;
    m_pFormatName = "iMX-mpeg2";
    break;
  case CODEC_ID_H263:
    m_decOpenParam.CodecFormat = VPU_V_H263;
    m_pFormatName = "iMX-h263";
    break;
  case CODEC_ID_H264:
  {
    // Test for VPU unsupported profiles to revert to sw decoding
    if ((m_hints.profile == 110) || //hi10p
        (m_hints.profile == 578))   //quite uncommon h264 profile
    {
      CLog::Log(LOGNOTICE, "i.MX6 VPU is not able to decode AVC profile %d", m_hints.profile);
      return false;
    }
    m_decOpenParam.CodecFormat = VPU_V_AVC;
    m_pFormatName = "iMX-h264";
    if (hints.extradata)
    {
      if ( *(char*)hints.extradata == 1 )
      {
        m_converter         = new CBitstreamConverter();
        m_convert_bitstream = m_converter->Open(hints.codec, (uint8_t *)hints.extradata, hints.extrasize, true);
      }
    }
    break;
  }
  case CODEC_ID_VC1:
    m_decOpenParam.CodecFormat = VPU_V_VC1_AP;
    m_pFormatName = "iMX-vc1";
    break;
  case CODEC_ID_CAVS:
  case CODEC_ID_AVS:
    m_decOpenParam.CodecFormat = VPU_V_AVS;
    m_pFormatName = "iMX-AVS";
    break;
  case CODEC_ID_RV10:
  case CODEC_ID_RV20:
  case CODEC_ID_RV30:
  case CODEC_ID_RV40:
    m_decOpenParam.CodecFormat = VPU_V_RV;
    m_pFormatName = "iMX-RV";
    break;
  case CODEC_ID_KMVC:
    m_decOpenParam.CodecFormat = VPU_V_AVC_MVC;
    m_pFormatName = "iMX-MVC";
    break;
  case CODEC_ID_VP8:
    m_decOpenParam.CodecFormat = VPU_V_VP8;
    m_pFormatName = "iMX-vp8";
    break;
  case CODEC_ID_MPEG4:
    switch(m_hints.codec_tag)
    {
    case _4CC('D','I','V','X'):
      m_decOpenParam.CodecFormat = VPU_V_XVID; // VPU_V_DIVX4
      m_pFormatName = "iMX-divx4";
      break;
    case _4CC('D','X','5','0'):
    case _4CC('D','I','V','5'):
      m_decOpenParam.CodecFormat = VPU_V_XVID; // VPU_V_DIVX56
      m_pFormatName = "iMX-divx5";
      break;
    case _4CC('X','V','I','D'):
    case _4CC('M','P','4','V'):
    case _4CC('P','M','P','4'):
    case _4CC('F','M','P','4'):
      m_decOpenParam.CodecFormat = VPU_V_XVID;
      m_pFormatName = "iMX-xvid";
      break;
    default:
      CLog::Log(LOGERROR, "iMX VPU : MPEG4 codec tag %d is not (yet) handled.\n", m_hints.codec_tag);
      return false;
    }
    break;
  default:
    CLog::Log(LOGERROR, "iMX VPU : codecid %d is not (yet) handled.\n", m_hints.codec);
    return false;
  }

  m_mixer.Start();

  return true;
}

void CDVDVideoCodecIMX::Dispose(void)
{
#ifdef DUMP_STREAM
  if (m_dump)
  {
    fclose(m_dump);
    m_dump = NULL;
  }
#endif
  VpuDecRetCode  ret;
  bool VPU_loaded = m_vpuHandle;

  // Dispose the mixer thread
  m_mixer.Dispose();

  // Release last buffer
  SAFE_RELEASE(m_lastBuffer);
  SAFE_RELEASE(m_currentBuffer);

  // Invalidate output buffers to prevent the renderer from mapping this memory
  for (int i=0; i<m_vpuFrameBufferNum; i++)
  {
    m_outputBuffers[i]->ReleaseFramebuffer(&m_vpuHandle);
    SAFE_RELEASE(m_outputBuffers[i]);
  }

  if (m_vpuHandle)
  {
    ret = VPU_DecFlushAll(m_vpuHandle);
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU flush failed with error code %d.\n", __FUNCTION__, ret);
    }
    ret = VPU_DecClose(m_vpuHandle);
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU close failed with error code %d.\n", __FUNCTION__, ret);
    }
    m_vpuHandle = 0;
  }

  m_frameCounter = 0;

  // Release memory
  if (m_outputBuffers != NULL)
  {
    delete m_outputBuffers;
    m_outputBuffers = NULL;
  }

  VpuFreeBuffers();
  m_vpuFrameBufferNum = 0;

  if (m_vpuFrameBuffers != NULL)
  {
    delete m_vpuFrameBuffers;
    m_vpuFrameBuffers = NULL;
  }

  if (VPU_loaded)
  {
    ret = VPU_DecUnLoad();
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU unload failed with error code %d.\n", __FUNCTION__, ret);
    }
  }

  if (m_converter)
  {
    m_converter->Close();
    SAFE_DELETE(m_converter);
  }

  return;
}

int CDVDVideoCodecIMX::Decode(BYTE *pData, int iSize, double dts, double pts)
{
  VpuDecFrameLengthInfo frameLengthInfo;
  VpuBufferNode inData;
  VpuDecRetCode ret;
  int decRet = 0;
  int retStatus = 0;
  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  int retries = 0;
  int idx;

#ifdef IMX_PROFILE
  static unsigned long long previous, current;
#endif
#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS)
  unsigned long long before_dec;
#endif

#ifdef DUMP_STREAM
  if (m_dump != NULL)
  {
    if (pData)
    {
      fwrite(&dts, sizeof(double), 1, m_dump);
      fwrite(&pts, sizeof(double), 1, m_dump);
      fwrite(&iSize, sizeof(int), 1, m_dump);
      fwrite(pData, 1, iSize, m_dump);
    }
  }
#endif

  SAFE_RELEASE(m_currentBuffer);

  if (!m_vpuHandle)
  {
    VpuOpen();
    if (!m_vpuHandle)
      return VC_ERROR;
  }

  for (int i=0; i < m_vpuFrameBufferNum; i++)
  {
    if (m_outputBuffers[i]->Rendered())
    {
      ret = m_outputBuffers[i]->ReleaseFramebuffer(&m_vpuHandle);
      if(ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s: vpu clear frame display failure: ret=%d \r\n",__FUNCTION__,ret);
      }
    }
  }

#ifdef IMX_PROFILE
  current = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s - delta time decode : %llu - demux size : %d  dts : %f - pts : %f\n", __FUNCTION__, current - previous, iSize, dts, pts);
  previous = current;
#endif

  if ((pData && iSize) ||
     (m_bytesToBeConsumed))
  {
    if ((m_convert_bitstream) && (iSize))
    {
      // convert demuxer packet from bitstream to bytestream (AnnexB)
      if (m_converter->Convert(demuxer_content, demuxer_bytes))
      {
        demuxer_content = m_converter->GetConvertBuffer();
        demuxer_bytes = m_converter->GetConvertSize();
      }
      else
        CLog::Log(LOGERROR,"%s - bitstream_convert error", __FUNCTION__);
    }

    inData.nSize = demuxer_bytes;
    inData.pPhyAddr = NULL;
    inData.pVirAddr = demuxer_content;
    if ((m_decOpenParam.CodecFormat == VPU_V_MPEG2) ||
        (m_decOpenParam.CodecFormat == VPU_V_VC1_AP)||
        (m_decOpenParam.CodecFormat == VPU_V_XVID))
    {
      inData.sCodecData.pData = (unsigned char *)m_hints.extradata;
      inData.sCodecData.nSize = m_hints.extrasize;
    }
    else
    {
      inData.sCodecData.pData = NULL;
      inData.sCodecData.nSize = 0;
    }

#ifdef IMX_PROFILE_BUFFERS
    static unsigned long long dec_time = 0;
#endif

    while (true) // Decode as long as the VPU consumes data
    {
#if defined(IMX_PROFILE) || defined(IMX_PROFILE_BUFFERS)
      before_dec = XbmcThreads::SystemClockMillis();
#endif
      if (m_frameReported)
        m_bytesToBeConsumed += inData.nSize;
      ret = VPU_DecDecodeBuf(m_vpuHandle, &inData, &decRet);
#ifdef IMX_PROFILE_BUFFERS
      dec_time += XbmcThreads::SystemClockMillis()-before_dec;
#endif
#ifdef IMX_PROFILE
      CLog::Log(LOGDEBUG, "%s - VPU dec 0x%x decode takes : %lld\n\n", __FUNCTION__, decRet,  XbmcThreads::SystemClockMillis() - before_dec);
#endif

      if (ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - VPU decode failed with error code %d.\n", __FUNCTION__, ret);
        goto out_error;
      }

      if (decRet & VPU_DEC_INIT_OK)
      // VPU decoding init OK : We can retrieve stream info
      {
        ret = VPU_DecGetInitialInfo(m_vpuHandle, &m_initInfo);
        if (ret == VPU_DEC_RET_SUCCESS)
        {
          if (g_advancedSettings.CanLogComponent(LOGVIDEO))
          {
            CLog::Log(LOGDEBUG, "%s - VPU Init Stream Info : %dx%d (interlaced : %d - Minframe : %d)"\
                      " - Align : %d bytes - crop : %d %d %d %d - Q16Ratio : %x\n", __FUNCTION__,
              m_initInfo.nPicWidth, m_initInfo.nPicHeight, m_initInfo.nInterlace, m_initInfo.nMinFrameBufferCount,
              m_initInfo.nAddressAlignment, m_initInfo.PicCropRect.nLeft, m_initInfo.PicCropRect.nTop,
              m_initInfo.PicCropRect.nRight, m_initInfo.PicCropRect.nBottom, m_initInfo.nQ16ShiftWidthDivHeightRatio);
          }
          if (VpuAllocFrameBuffers())
          {
            ret = VPU_DecRegisterFrameBuffer(m_vpuHandle, m_vpuFrameBuffers, m_vpuFrameBufferNum);
            if (ret != VPU_DEC_RET_SUCCESS)
            {
              CLog::Log(LOGERROR, "%s - VPU error while registering frame buffers (%d).\n", __FUNCTION__, ret);
              goto out_error;
            }
          }
          else
          {
            goto out_error;
          }
        }
        else
        {
          CLog::Log(LOGERROR, "%s - VPU get initial info failed (%d).\n", __FUNCTION__, ret);
          goto out_error;
        }
      } //VPU_DEC_INIT_OK

      if (decRet & VPU_DEC_ONE_FRM_CONSUMED)
      {
        ret = VPU_DecGetConsumedFrameInfo(m_vpuHandle, &frameLengthInfo);
        if (ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - VPU error retireving info about consummed frame (%d).\n", __FUNCTION__, ret);
        }
        m_bytesToBeConsumed -= (frameLengthInfo.nFrameLength + frameLengthInfo.nStuffLength);
        if (frameLengthInfo.pFrame)
        {
          idx = VpuFindBuffer(frameLengthInfo.pFrame->pbufY);
          if (m_bytesToBeConsumed < 50)
            m_bytesToBeConsumed = 0;
          if (idx != -1)
          {
            if (m_previousPts != DVD_NOPTS_VALUE)
            {
              m_outputBuffers[idx]->SetPts(m_previousPts);
              m_previousPts = DVD_NOPTS_VALUE;
            }
            else
              m_outputBuffers[idx]->SetPts(pts);
          }
          else
            CLog::Log(LOGERROR, "%s - could not find frame buffer\n", __FUNCTION__);
        }
      } //VPU_DEC_ONE_FRM_CONSUMED

      if (decRet & VPU_DEC_OUTPUT_DIS)
      // Frame ready to be displayed
      {
        if (retStatus & VC_PICTURE)
            CLog::Log(LOGERROR, "%s - Second picture in the same decode call !\n", __FUNCTION__);

        ret = VPU_DecGetOutputFrame(m_vpuHandle, &m_frameInfo);
        if(ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - VPU Cannot get output frame(%d).\n", __FUNCTION__, ret);
          goto out_error;
        }

        // Some codecs (VC1?) lie about their frame size (mod 16). Adjust...
        m_frameInfo.pExtInfo->nFrmWidth  = (((m_frameInfo.pExtInfo->nFrmWidth) + 15) & ~15);
        m_frameInfo.pExtInfo->nFrmHeight = (((m_frameInfo.pExtInfo->nFrmHeight) + 15) & ~15);

        idx = VpuFindBuffer(m_frameInfo.pDisplayFrameBuf->pbufY);
        if (idx != -1)
        {
          CDVDVideoCodecIMXVPUBuffer *buffer = m_outputBuffers[idx];
          /* quick & dirty fix to get proper timestamping for VP8 codec */
          if (m_decOpenParam.CodecFormat == VPU_V_VP8)
            buffer->SetPts(pts);

          buffer->Lock();
          buffer->SetDts(dts);
          buffer->Queue(&m_frameInfo, m_lastBuffer);

#ifdef IMX_PROFILE_BUFFERS
          CLog::Log(LOGNOTICE, "+D  %f  %lld\n", buffer->GetPts(), dec_time);
          dec_time = 0;
#endif

#ifdef TRACE_FRAMES
          CLog::Log(LOGDEBUG, "+  %02d dts %f pts %f  (VPU)\n", idx, pDvdVideoPicture->dts, pDvdVideoPicture->pts);
#endif

          if (!m_usePTS)
          {
            buffer->SetPts(DVD_NOPTS_VALUE);
            buffer->SetDts(DVD_NOPTS_VALUE);
          }

          // Save last buffer
          SAFE_RELEASE(m_lastBuffer);
          m_lastBuffer = buffer;
          m_lastBuffer->Lock();

#ifdef IMX_PROFILE_BUFFERS
          static unsigned long long lastD = 0;
          unsigned long long current = XbmcThreads::SystemClockMillis(), tmp;
          CLog::Log(LOGNOTICE, "+V  %f  %lld\n", buffer->GetPts(), current-lastD);
          lastD = current;
#endif

          m_currentBuffer = buffer;
          m_currentBuffer->ipu = &m_deinterlacer;
          /*
          if (m_dropState)
          {
            m_currentBuffer = m_mixer.Process(NULL);
            if (!m_currentBuffer)
              m_currentBuffer = buffer;
            else
              buffer->Release();
          }
          else
            m_currentBuffer = m_mixer.Process(buffer);
          */

          if (m_currentBuffer)
            retStatus |= VC_PICTURE;
        }
      } //VPU_DEC_OUTPUT_DIS

      // According to libfslvpuwrap: If this flag is set then the frame should
      // be dropped. It is just returned to gather decoder information but not
      // for display.
      if (decRet & VPU_DEC_OUTPUT_MOSAIC_DIS)
      {
        ret = VPU_DecGetOutputFrame(m_vpuHandle, &m_frameInfo);
        if(ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - VPU Cannot get output frame(%d).\n", __FUNCTION__, ret);
          goto out_error;
        }

        // Display frame
        ret = VPU_DecOutFrameDisplayed(m_vpuHandle, m_frameInfo.pDisplayFrameBuf);
        if(ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s: VPU Clear frame display failure(%d)\n",__FUNCTION__,ret);
          goto out_error;
        }
      } //VPU_DEC_OUTPUT_MOSAIC_DIS

      if (decRet & VPU_DEC_OUTPUT_REPEAT)
      {
        if (g_advancedSettings.CanLogComponent(LOGVIDEO))
          CLog::Log(LOGDEBUG, "%s - Frame repeat.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_OUTPUT_DROPPED)
      {
        if (g_advancedSettings.CanLogComponent(LOGVIDEO))
          CLog::Log(LOGDEBUG, "%s - Frame dropped.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_NO_ENOUGH_BUF)
      {
          CLog::Log(LOGERROR, "%s - No frame buffer available.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_SKIP)
      {
        if (g_advancedSettings.CanLogComponent(LOGVIDEO))
          CLog::Log(LOGDEBUG, "%s - Frame skipped.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_FLUSH)
      {
        CLog::Log(LOGNOTICE, "%s - VPU requires a flush.\n", __FUNCTION__);
        Reset();
        retStatus = VC_FLUSHED;
      }
      if (decRet & VPU_DEC_OUTPUT_EOS)
      {
        CLog::Log(LOGNOTICE, "%s - EOS encountered.\n", __FUNCTION__);
      }
      if ((decRet & VPU_DEC_NO_ENOUGH_INBUF) ||
          (decRet & VPU_DEC_OUTPUT_DIS))
      {
        // We are done with VPU decoder that time
        break;
      }

      retries++;
      if (retries >= m_maxVpuDecodeLoops)
      {
        CLog::Log(LOGERROR, "%s - Leaving VPU decoding loop after %d iterations\n", __FUNCTION__, m_maxVpuDecodeLoops);
        break;
      }

      if (!(decRet & VPU_DEC_INPUT_USED))
      {
        CLog::Log(LOGERROR, "%s - input not used : addr %p  size :%d!\n", __FUNCTION__, inData.pVirAddr, inData.nSize);
      }

      // Let's process again as VPU_DEC_NO_ENOUGH_INBUF was not set
      // and we don't have an image ready if we reach that point
      inData.pVirAddr = NULL;
      inData.nSize = 0;
    } // Decode loop
  } //(pData && iSize)

  // Reply to flush call only if double rate is active
  if ((m_dropState || m_mixer.DoubleRate()) && !retStatus)
  {
    m_currentBuffer = m_mixer.Process(NULL);
    if (m_currentBuffer)
    {
      retStatus |= VC_PICTURE;
    }
  }

  if (retStatus == 0)
  {
    retStatus |= VC_BUFFER;
  }

  if (m_bytesToBeConsumed > 0)
  {
    // Remember the current pts because the data which has just
    // been sent to the VPU has not yet been consumed.
    // This pts is related to the frame that will be consumed
    // at next call...
    m_previousPts = pts;
  }

#ifdef IMX_PROFILE
  CLog::Log(LOGDEBUG, "%s - returns %x - duration %lld\n", __FUNCTION__, retStatus, XbmcThreads::SystemClockMillis() - previous);
#endif
  return retStatus;

out_error:
  return VC_ERROR;
}

void CDVDVideoCodecIMX::Reset()
{
  int ret;

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s - called\n", __FUNCTION__);

  // Restart mixer
  m_mixer.Dispose();
  m_mixer.Start();

  // Release last buffer
  SAFE_RELEASE(m_lastBuffer);
  SAFE_RELEASE(m_currentBuffer);

  // Invalidate all buffers
  for(int i=0; i < m_vpuFrameBufferNum; i++)
    m_outputBuffers[i]->ReleaseFramebuffer(&m_vpuHandle);

  m_frameCounter = 0;
  m_bytesToBeConsumed = 0;
  m_previousPts = DVD_NOPTS_VALUE;

  // Flush VPU
  ret = VPU_DecFlushAll(m_vpuHandle);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU flush failed with error code %d.\n", __FUNCTION__, ret);
  }

}

unsigned CDVDVideoCodecIMX::GetAllowedReferences()
{
  return 3;
}

bool CDVDVideoCodecIMX::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
  if (pDvdVideoPicture)
  {
    SAFE_RELEASE(pDvdVideoPicture->IMXBuffer);
  }

  return true;
}

bool CDVDVideoCodecIMX::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
#ifdef IMX_PROFILE
  static unsigned int previous = 0;
  unsigned int current;

  current = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s  tm:%03d\n", __FUNCTION__, current - previous);
  previous = current;
#endif

  m_frameCounter++;

  pDvdVideoPicture->iFlags = DVP_FLAG_ALLOCATED;
  if (m_dropState)
    pDvdVideoPicture->iFlags |= DVP_FLAG_DROPPED;
  else
    pDvdVideoPicture->iFlags &= ~DVP_FLAG_DROPPED;

  pDvdVideoPicture->format = RENDER_FMT_IMXMAP;
  pDvdVideoPicture->iWidth = m_frameInfo.pExtInfo->FrmCropRect.nRight - m_frameInfo.pExtInfo->FrmCropRect.nLeft;
  pDvdVideoPicture->iHeight = m_frameInfo.pExtInfo->FrmCropRect.nBottom - m_frameInfo.pExtInfo->FrmCropRect.nTop;

  pDvdVideoPicture->iDisplayWidth = ((pDvdVideoPicture->iWidth * m_frameInfo.pExtInfo->nQ16ShiftWidthDivHeightRatio) + 32767) >> 16;
  pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;

  // Current buffer is locked already -> hot potato
  pDvdVideoPicture->pts = m_currentBuffer->GetPts();
  pDvdVideoPicture->dts = m_currentBuffer->GetDts();

  pDvdVideoPicture->IMXBuffer = m_currentBuffer;
  m_currentBuffer = NULL;

  return true;
}

void CDVDVideoCodecIMX::SetDropState(bool bDrop)
{

  // We are fast enough to continue to really decode every frames
  // and avoid artefacts...
  // (Of course these frames won't be rendered but only decoded)

  if (m_dropState != bDrop)
  {
    m_dropState = bDrop;
#ifdef TRACE_FRAMES
    CLog::Log(LOGDEBUG, "%s : %d\n", __FUNCTION__, bDrop);
#endif
  }
}

/*******************************************/
#ifdef TRACE_FRAMES
CDVDVideoCodecIMXBuffer::CDVDVideoCodecIMXBuffer(int idx)
  : ipu(NULL)
  , m_idx(idx)
  , m_refs(1)
#else
CDVDVideoCodecIMXBuffer::CDVDVideoCodecIMXBuffer()
  : ipu(NULL)
  , m_refs(1)
#endif
  , m_pts(DVD_NOPTS_VALUE)
  , m_dts(DVD_NOPTS_VALUE)
{
}

void CDVDVideoCodecIMXBuffer::SetPts(double pts)
{
  m_pts = pts;
}

void CDVDVideoCodecIMXBuffer::SetDts(double dts)
{
  m_dts = dts;
}

bool CDVDVideoCodecIMXBuffer::Blit(const CRectInt *crop)
{
  if (!ipu) return false;
  return ipu->BlitFB(this, crop);
}

bool CDVDVideoCodecIMXBuffer::Show()
{
  if (!ipu) return false;
  return ipu->SwapFB();
}

#ifdef TRACE_FRAMES
CDVDVideoCodecIMXVPUBuffer::CDVDVideoCodecIMXVPUBuffer(int idx)
  : CDVDVideoCodecIMXBuffer(idx)
#else
CDVDVideoCodecIMXVPUBuffer::CDVDVideoCodecIMXVPUBuffer()
  : CDVDVideoCodecIMXBuffer()
#endif
  , m_frameBuffer(NULL)
  , m_rendered(false)
  , m_previousBuffer(NULL)
{
}

void CDVDVideoCodecIMXVPUBuffer::Lock()
{
#ifdef TRACE_FRAMES
  long count = AtomicIncrement(&m_refs);
  CLog::Log(LOGDEBUG, "R+ %02d  -  ref : %d  (VPU)\n", m_idx, count);
#else
  AtomicIncrement(&m_refs);
#endif
}

long CDVDVideoCodecIMXVPUBuffer::Release()
{
  long count = AtomicDecrement(&m_refs);
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG, "R- %02d  -  ref : %d  (VPU)\n", m_idx, count);
#endif
  if (count == 2)
  {
    // Only referenced by the codec and its next frame, release the previous
    SAFE_RELEASE(m_previousBuffer);
  }
  else if (count == 1)
  {
    // If count drops to 1 then the only reference is being held by the codec
    // that it can be released in the next Decode call.
    if(m_frameBuffer != NULL)
    {
      m_rendered = true;
      SAFE_RELEASE(m_previousBuffer);
#ifdef TRACE_FRAMES
      CLog::Log(LOGDEBUG, "R  %02d  (VPU)\n", m_idx);
#endif
    }
  }
  else if (count == 0)
  {
    delete this;
  }

  return count;
}

bool CDVDVideoCodecIMXVPUBuffer::IsValid()
{
  return m_frameBuffer != NULL;
}

bool CDVDVideoCodecIMXVPUBuffer::Rendered() const
{
  return m_rendered;
}

void CDVDVideoCodecIMXVPUBuffer::Queue(VpuDecOutFrameInfo *frameInfo,
                                       CDVDVideoCodecIMXVPUBuffer *previous)
{
  // No lock necessary because at this time there is definitely no
  // thread still holding a reference
  m_frameBuffer = frameInfo->pDisplayFrameBuf;
  m_rendered = false;
  m_previousBuffer = previous;
  if (m_previousBuffer)
    m_previousBuffer->Lock();

  iWidth    = frameInfo->pExtInfo->nFrmWidth;
  iHeight   = frameInfo->pExtInfo->nFrmHeight;
  pVirtAddr = m_frameBuffer->pbufVirtY;
  pPhysAddr = m_frameBuffer->pbufY;
  fieldType = frameInfo->eFieldType;

  // We decode to I420
#ifdef IMX_INPUT_FORMAT_I420
  iFormat = 0;
#else
  iFormat = 1;
#endif
}

VpuDecRetCode CDVDVideoCodecIMXVPUBuffer::ReleaseFramebuffer(VpuDecHandle *handle)
{
  // Again no lock required because this is only issued after the last
  // external reference was released
  VpuDecRetCode ret = VPU_DEC_RET_FAILURE;

  if((m_frameBuffer != NULL) && *handle)
  {
    ret = VPU_DecOutFrameDisplayed(*handle, m_frameBuffer);
    if(ret != VPU_DEC_RET_SUCCESS)
      CLog::Log(LOGERROR, "%s: vpu clear frame display failure: ret=%d \r\n",__FUNCTION__,ret);
  }
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG, "-  %02d  (VPU)\n", m_idx);
#endif
  m_rendered = false;
  m_frameBuffer = NULL;
  SetPts(DVD_NOPTS_VALUE);
  SAFE_RELEASE(m_previousBuffer);

  return ret;
}

CDVDVideoCodecIMXVPUBuffer *CDVDVideoCodecIMXVPUBuffer::GetPreviousBuffer() const
{
  return m_previousBuffer;
}

CDVDVideoCodecIMXVPUBuffer::~CDVDVideoCodecIMXVPUBuffer()
{
  assert(m_refs == 0);
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG, "~  %02d  (VPU)\n", m_idx);
#endif
}

#ifdef TRACE_FRAMES
CDVDVideoCodecIMXIPUBuffer::CDVDVideoCodecIMXIPUBuffer(int idx)
  : CDVDVideoCodecIMXBuffer(idx)
#else
CDVDVideoCodecIMXIPUBuffer::CDVDVideoCodecIMXIPUBuffer()
  : CDVDVideoCodecIMXBuffer()
#endif
  , m_bFree(true)
  , m_pPhyAddr(0)
  , m_pVirtAddr(NULL)
  , m_nSize(0)
{
}

CDVDVideoCodecIMXIPUBuffer::~CDVDVideoCodecIMXIPUBuffer()
{
  assert(m_refs == 0);
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG, "~  %02d  (IPU)\n", m_idx);
#endif
}

void CDVDVideoCodecIMXIPUBuffer::Lock()
{
#ifdef TRACE_FRAMES
  long count = AtomicIncrement(&m_refs);
  CLog::Log(LOGDEBUG, "R+ %02d  -  ref : %d  (IPU)\n", m_idx, count);
#else
  AtomicIncrement(&m_refs);
#endif

}

long CDVDVideoCodecIMXIPUBuffer::Release()
{
  long count = AtomicDecrement(&m_refs);
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG, "R- %02d  -  ref : %d  (IPU)\n", m_idx, count);
#endif
  if (count == 1)
  {
    ReleaseFrameBuffer();
  }
  else if (count == 0)
  {
    delete this;
  }

  return count;
}

bool CDVDVideoCodecIMXIPUBuffer::IsValid()
{
  return /*m_source && m_source->IsValid() && */m_pPhyAddr;
}

bool CDVDVideoCodecIMXIPUBuffer::Process(int fd, CDVDVideoCodecIMXVPUBuffer *buffer,
                                         int fieldFmt, bool lowMotion)
{
  CDVDVideoCodecIMXVPUBuffer *previousBuffer;
  struct ipu_task task;
  memset(&task, 0, sizeof(task));

  if (lowMotion)
    previousBuffer = buffer->GetPreviousBuffer();
  else
    previousBuffer = NULL;

  m_bFree            = true;
  iWidth             = buffer->iWidth;
  iHeight            = buffer->iHeight;
  fieldType          = VPU_FIELD_NONE;

  // Input is the VPU decoded frame
  task.input.width   = iWidth;
  task.input.height  = iHeight;
  task.input.crop.h  = iHeight;

#ifdef IMX_INPUT_FORMAT_I420
  task.input.format  = IPU_PIX_FMT_YUV420P;
#else
  task.input.format  = IPU_PIX_FMT_NV12;
#endif

  // Output is our IPU buffer
  task.output.width  = iWidth;
  task.output.height = iHeight;
  task.output.crop.h = iHeight;
#ifdef IMX_OUTPUT_FORMAT_I420
  task.output.format = IPU_PIX_FMT_YUV420P;
  iFormat            = 0;
#else
  task.output.format = IPU_PIX_FMT_NV12;
  iFormat            = 1;
#endif
#ifdef IMX_OUTPUT_FORMAT_RGB565
  task.output.format = IPU_PIX_FMT_RGB565;
  iFormat            = 2;
#endif
#ifdef IMX_OUTPUT_FORMAT_RGB32
  task.output.format = IPU_PIX_FMT_RGB32;
  iFormat            = 3;
#endif
  task.output.paddr  = (int)pPhysAddr;

  VpuFieldType fieldType;
  task.input.deinterlace.enable = 1;

  // Fill current and next buffer address
  if (lowMotion && previousBuffer && previousBuffer->IsValid())
  {
    task.input.paddr_n            = (int)buffer->pPhysAddr;
    task.input.paddr              = (int)previousBuffer->pPhysAddr;
    task.input.deinterlace.motion = LOW_MOTION;
    fieldType                     = previousBuffer->GetFieldType();
  }
  else
  {
    task.input.paddr              = (int)buffer->pPhysAddr;
    task.input.deinterlace.motion = HIGH_MOTION;
    fieldType                     = buffer->GetFieldType();
  }

  task.input.deinterlace.field_fmt = fieldFmt;

  switch (fieldType)
  {
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

#ifdef IMX_PROFILE
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif

  /* We do the VDI buffer splitting ourselves since the kernel
   * driver (IPU) is either buggy or it is a feature that it does
   * not split widths aligned to 16 pixels to get maximum burst on
   * DMA. Since we know that the input and output dimensions are the
   * same we can implement an easier algorithm that takes care of
   * that.
   */
  unsigned int nRequiredStripes = (iWidth+IMX_VDI_MAX_WIDTH-1) / IMX_VDI_MAX_WIDTH;
  unsigned int iProcWidth = iWidth;
  unsigned int iStripeOffset = 0;

  while (iStripeOffset < iWidth)
  {
    unsigned int iStripeWidth = Align(iWidth/nRequiredStripes, FRAME_ALIGN);
    if (iStripeWidth > iProcWidth)
      iStripeWidth = iProcWidth;

    task.input.crop.pos.x  = iStripeOffset;
    task.input.crop.w      = iStripeWidth;
    task.output.crop.pos.x = task.input.crop.pos.x;
    task.output.crop.w     = task.input.crop.w;

    if (ioctl(fd, IPU_QUEUE_TASK, &task) < 0)
    {
      CLog::Log(LOGERROR, "IPU task failed: %s\n", strerror(errno));
      return false;
    }

    iStripeOffset += iStripeWidth;
    iProcWidth -= iStripeWidth;
  }

#ifdef IMX_PROFILE
  CLog::Log(LOGDEBUG, "DEINT: tm:%d\n", XbmcThreads::SystemClockMillis() - time);
#endif

  m_bFree = false;
  buffer->Lock();

  // Copy PTS
  SetPts(buffer->GetPts());
  SetDts(buffer->GetDts());

  buffer->Release();

  return true;
}

void CDVDVideoCodecIMXIPUBuffer::ReleaseFrameBuffer()
{
#ifdef TRACE_FRAMES
  CLog::Log(LOGDEBUG, "-  %02d  (IPU)\n", m_idx);
#endif
  m_bFree = true;
}

bool CDVDVideoCodecIMXIPUBuffer::Allocate(int fd, int width, int height, int nAlign)
{
  m_iWidth = Align(width,FRAME_ALIGN);
  m_iHeight = Align(height,(2*FRAME_ALIGN));
#if defined(IMX_OUTPUT_FORMAT_NV12) || defined(IMX_OUTPUT_FORMAT_I420)
  // I420 == 12 bpp
  m_nSize = m_iWidth*m_iHeight*12/8;
#endif
#ifdef IMX_OUTPUT_FORMAT_RGB565
  // RGB565 = 16 bpp
  m_nSize = m_iWidth*m_iHeight*16/8;
#endif
#ifdef IMX_OUTPUT_FORMAT_RGB32
  // RGB32 = 32 bpp
  m_nSize = m_iWidth*m_iHeight*32/8;
#endif

  m_nSize += FRAME_ALIGN;
  m_pPhyAddr = m_nSize;

  pPhysAddr = pVirtAddr = NULL;

  int r = ioctl(fd, IPU_ALLOC, &m_pPhyAddr);
  if (r < 0)
  {
    m_pPhyAddr = 0;
    CLog::Log(LOGERROR, "ioctl IPU_ALLOC fail: disable deinterlacing: %s\n", strerror(errno));
    return false;
  }

  CLog::Log(LOGNOTICE, "IPU: alloc %d bytes for frame of %dx%d at 0x%x\n",
            m_nSize, m_iWidth, m_iHeight, m_pPhyAddr);

  m_pVirtAddr = (uint8_t*)mmap(0, m_nSize, PROT_READ, MAP_SHARED,
                               fd, m_pPhyAddr);
  if (!m_pVirtAddr)
  {
    CLog::Log(LOGERROR, "IPU mmap failed: disable deinterlacing: %s\n", strerror(errno));
    return false;
  }

  pPhysAddr = (uint8_t*)Align(m_pPhyAddr, FRAME_ALIGN);
  pVirtAddr = (uint8_t*)Align(m_pVirtAddr, FRAME_ALIGN);

  return true;
}

bool CDVDVideoCodecIMXIPUBuffer::Free(int fd)
{
  bool ret = true;

  // Unmap virtual memory
  if (m_pVirtAddr != NULL)
  {
    if(munmap(m_pVirtAddr, m_nSize))
    {
      CLog::Log(LOGERROR, "IPU unmap failed: %s\n", strerror(errno));
      ret = false;
    }

    m_pVirtAddr = NULL;
  }

  // Free IPU memory
  if (m_pPhyAddr)
  {
    if (ioctl(fd, IPU_FREE, &m_pPhyAddr))
    {
      CLog::Log(LOGERROR, "IPU free buffer 0x%x failed: %s\n",
                m_pPhyAddr, strerror(errno));
      ret = false;
    }

    m_pPhyAddr = 0;
  }

  pPhysAddr = pVirtAddr = NULL;
  m_bFree = true;

  return ret;
}

RenderFB1::RenderFB1()
: m_fbHandle(0)
, m_fbPages(0)
, m_fbCurrentPage(0)
, m_fbPhysAddr(0)
, m_fbVirtAddr(NULL)
{
}

RenderFB1::~RenderFB1()
{
  Close();
}

bool RenderFB1::Init()
{
  m_lastCrop = CRectInt(0,0,0,0);

  int fb0 = open("/dev/fb0", O_RDWR, 0);

  if (fb0 < 0)
  {
    CLog::Log(LOGWARNING, "Failed to open /dev/fb0\n");
    return false;
  }

  struct fb_var_screeninfo fbVar;
  if (ioctl(fb0, FBIOGET_VSCREENINFO, &fbVar) < 0)
  {
    CLog::Log(LOGWARNING, "Failed to read primary screen resolution\n");
    close(fb0);
    return false;
  }

  if (m_fbHandle)
  {
    // Check for updated screen resolution
    if ((m_fbWidth != fbVar.xres) || (m_fbHeight != fbVar.yres))
      Close();
    else
    {
      close(fb0);
      Unblank();
      return true;
    }
  }

  close(fb0);

  memcpy(&m_fbVar, &fbVar, sizeof(fbVar));

  const char *deviceName = "/dev/fb1";
  // Open Framebuffer and gets its address
  m_fbHandle = open(deviceName, O_RDWR | O_NONBLOCK, 0);
  if (m_fbHandle < 0)
  {
    CLog::Log(LOGWARNING, "Failed to open framebuffer: %s\n", deviceName);
    return false;
  }

  m_fbWidth = m_fbVar.xres;
  m_fbHeight = m_fbVar.yres;

  if (ioctl(m_fbHandle, FBIOGET_VSCREENINFO, &m_fbVar) < 0)
  {
    CLog::Log(LOGWARNING, "Failed to query variable screen info at %s\n", deviceName);
    return false;
  }

  // We want two fb pages
  m_fbPages = 2;

  m_fbVar.xoffset = 0;
  m_fbVar.yoffset = 0;
  m_fbVar.bits_per_pixel = 16;
  m_fbVar.nonstd = _4CC('U', 'Y', 'V', 'Y');
  m_fbVar.activate = FB_ACTIVATE_NOW;
  m_fbVar.xres = m_fbWidth;
  m_fbVar.yres = m_fbHeight;
  m_fbVar.yres_virtual = m_fbVar.yres * m_fbPages;
  m_fbVar.xres_virtual = m_fbVar.xres;

  if (ioctl(m_fbHandle, FBIOPUT_VSCREENINFO, &m_fbVar) < 0)
  {
    CLog::Log(LOGWARNING, "Failed to setup %s\n", deviceName);
    close(m_fbHandle);
    m_fbHandle = 0;
    m_fbPages = 0;
    return false;
  }

  struct fb_fix_screeninfo fb_fix;
  if (ioctl(m_fbHandle, FBIOGET_FSCREENINFO, &fb_fix) < 0)
  {
    CLog::Log(LOGWARNING, "Failed to query fixed screen info at %s\n", deviceName);
    close(m_fbHandle);
    m_fbHandle = 0;
    m_fbPages = 0;
    return false;
  }

  // Final setup
  m_fbPhysSize = fb_fix.smem_len;
  m_fbPageSize = fb_fix.line_length * m_fbVar.yres_virtual / m_fbPages;
  m_fbPhysAddr = fb_fix.smem_start;
  m_fbVirtAddr = (char*)mmap(0, m_fbPhysSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_fbHandle, 0);
  m_fbNeedSwap = false;

  printf("smem_len = %u\n", fb_fix.smem_len);

  Clear();
  Unblank();

  CLog::Log(LOGNOTICE, "Initialized fb output with %d pages\n", m_fbPages);

  return true;
}

bool RenderFB1::Close()
{
  if (m_fbVirtAddr)
  {
    Clear();
    munmap(m_fbVirtAddr, m_fbPhysSize);
    m_fbVirtAddr = NULL;
  }

  if (m_fbHandle)
  {
    Blank();
    close(m_fbHandle);
    m_fbPages = 0;
    m_fbCurrentPage = 0;
    m_fbHandle = 0;
    m_fbPhysAddr = 0;
  }

  CLog::Log(LOGNOTICE, "Closed fb output\n", m_fbPages);

  return true;
}

bool RenderFB1::Blank()
{
  if (!m_fbHandle) return false;
  return ioctl(m_fbHandle, FBIOBLANK, 1) == 0;
}

bool RenderFB1::Unblank()
{
  if (!m_fbHandle) return false;
  return ioctl(m_fbHandle, FBIOBLANK, FB_BLANK_UNBLANK) == 0;
}

void RenderFB1::SetViewport(const CRectInt &r)
{
  if (m_lastCrop != r)
    Clear();

  m_lastCrop = r;
}

bool RenderFB1::Swap()
{
  if (!m_fbHandle) return false;
  if (m_fbPages < 2) return true;

  int ret;
  int nextPage = m_fbCurrentPage + 1;
  if (nextPage >= m_fbPages)
    nextPage = 0;

  if (nextPage != m_fbCurrentPage)
  {
    m_fbVar.activate = FB_ACTIVATE_VBL;
    m_fbVar.yoffset = m_fbVar.yres*m_fbCurrentPage;
    if ( (ret = ioctl(m_fbHandle, FBIOPAN_DISPLAY, &m_fbVar)) < 0 )
      CLog::Log(LOGWARNING, "Panning failed: %s\n", strerror(errno));

    // Wait for sync
    if (ioctl(m_fbHandle, FBIO_WAITFORVSYNC, 0) < 0)
      CLog::Log(LOGWARNING, "Vsync failed: %s\n", strerror(errno));
  }

  m_fbCurrentPage = nextPage;
  m_fbNeedSwap = false;
  return true;
}

void RenderFB1::Clear()
{
  if (!m_fbVirtAddr) return;

  char *tmp_buf = m_fbVirtAddr;
  int pixels = m_fbPageSize*m_fbPages/2;
  for (int i = 0; i < pixels; ++i, tmp_buf += 2)
  {
    tmp_buf[0] = 128;
    tmp_buf[1] = 16;
  }
}

RenderFB1 CDVDVideoCodecIMXIPUBuffers::m_fb;

CDVDVideoCodecIMXIPUBuffers::CDVDVideoCodecIMXIPUBuffers()
  : m_ipuHandle(0)
  , m_bufferNum(0)
  , m_buffers(NULL)
  , m_currentFieldFmt(0)
{
}

CDVDVideoCodecIMXIPUBuffers::~CDVDVideoCodecIMXIPUBuffers()
{
  Close();
}

bool CDVDVideoCodecIMXIPUBuffers::Init(int width, int height, int numBuffers, int nAlign)
{
  if (numBuffers<=0)
  {
    CLog::Log(LOGERROR, "IPU Init: invalid number of buffers: %d\n", numBuffers);
    return false;
  }

  m_fb.Init();
  m_fb.Clear();

  m_ipuHandle = open("/dev/mxc_ipu", O_RDWR, 0);
  if (m_ipuHandle<=0)
  {
    CLog::Log(LOGWARNING, "Failed to initialize IPU: deinterlacing disabled: %s\n",
              strerror(errno));
    m_ipuHandle = 0;
    return false;
  }

  m_bufferNum = numBuffers;
  m_buffers = new CDVDVideoCodecIMXIPUBuffer*[m_bufferNum];
  m_currentFieldFmt = 0;

  for (int i=0; i < m_bufferNum; i++)
  {
#ifdef TRACE_FRAMES
    m_buffers[i] = new CDVDVideoCodecIMXIPUBuffer(i);
#else
    m_buffers[i] = new CDVDVideoCodecIMXIPUBuffer;
#endif
    if (!m_buffers[i]->Allocate(m_ipuHandle, width, height, nAlign))
    {
      Close();
      return false;
    }
  }

  return true;
}

inline void CDVDVideoCodecIMXIPUBuffers::SetDoubleRate(bool flag)
{
  if (flag)
    m_currentFieldFmt |= IPU_DEINTERLACE_RATE_EN;
  else
    m_currentFieldFmt &= ~IPU_DEINTERLACE_RATE_EN;

  m_currentFieldFmt &= ~IPU_DEINTERLACE_RATE_FRAME1;
}

inline bool CDVDVideoCodecIMXIPUBuffers::DoubleRate() const
{
  return m_currentFieldFmt & IPU_DEINTERLACE_RATE_EN;
}

inline void CDVDVideoCodecIMXIPUBuffers::SetInterpolatedFrame(bool flag)
{
  if (flag)
    m_currentFieldFmt &= ~IPU_DEINTERLACE_RATE_FRAME1;
  else
    m_currentFieldFmt |= IPU_DEINTERLACE_RATE_FRAME1;
}

bool CDVDVideoCodecIMXIPUBuffers::Reset()
{
  for (int i=0; i < m_bufferNum; i++)
    m_buffers[i]->ReleaseFrameBuffer();
  m_currentFieldFmt = 0;
}

bool CDVDVideoCodecIMXIPUBuffers::Close()
{
  bool ret = true;

  if (m_ipuHandle)
  {
    for (int i=0; i < m_bufferNum; i++)
    {
      if (m_buffers[i] == NULL ) continue;
      if (!m_buffers[i]->Free(m_ipuHandle))
        ret = false;
    }

    // Close IPU device
    if (close(m_ipuHandle))
    {
      CLog::Log(LOGERROR, "IPU failed to close interface: %s\n", strerror(errno));
      ret = false;
    }

    m_ipuHandle = 0;
  }

  // Blank fb and clear it
  m_fb.Blank();
  m_fb.Clear();

  if (m_buffers)
  {
    for (int i=0; i < m_bufferNum; i++)
      SAFE_RELEASE(m_buffers[i]);

    delete m_buffers;
    m_buffers = NULL;
  }

  m_bufferNum = 0;

  return true;
}

CDVDVideoCodecIMXIPUBuffer *
CDVDVideoCodecIMXIPUBuffers::Process(CDVDVideoCodecIMXVPUBuffer *sourceBuffer,
                                     bool lowMotion)
{
  CDVDVideoCodecIMXIPUBuffer *target = NULL;
  bool ret = true;

  if (!m_bufferNum)
    return NULL;

  for (int i=0; i < m_bufferNum; i++ )
  {
    if (!m_buffers[i]->Rendered()) continue;

    // IPU process:
    // SRC: Current VPU physical buffer address + last VPU buffer address
    // DST: IPU buffer[i]
    ret = m_buffers[i]->Process(m_ipuHandle, sourceBuffer, m_currentFieldFmt,
                                lowMotion);
    if (ret)
    {
#ifdef TRACE_FRAMES
      CLog::Log(LOGDEBUG, "+  %02d  (IPU)\n", i);
#endif
      target = m_buffers[i];
    }
    break;
  }

  // Buffers are there but there is no free one, this is an error!
  // Rendering will continue with unprocessed frames ...
  if (ret && target==NULL)
  {
    CLog::Log(LOGERROR, "Deinterlacing: did not find free buffer, forward unprocessed frame\n");
  }

  return target;
}

bool CDVDVideoCodecIMXIPUBuffers::BlitFB(CDVDVideoCodecIMXBuffer *b,
                                         const CRectInt *crop)
{
  if (!m_fb.IsValid())
  {
    CLog::Log(LOGERROR, "fbout: not initialized\n");
    return false;
  }

  // Only VPU buffers are passed. The interface should be changed in
  // future to incorporate that. Currently the GPU path is still active
  // and can be switched on easily although it is much slower.
  CDVDVideoCodecIMXVPUBuffer *buf = static_cast<CDVDVideoCodecIMXVPUBuffer*>(b);

  //if (m_fb.NeedSwap())
  //  CLog::Log(LOGWARNING, "fbout: same page rendered again\n");

  int ret;
  struct ipu_task task;
  memset(&task, 0, sizeof(task));

  task.input.width = buf->iWidth;
  task.input.height = buf->iHeight;
  task.input.crop.pos.x = 0;
  task.input.crop.pos.y = 0;
  task.input.crop.w = buf->iWidth;
  task.input.crop.h = buf->iHeight;

  switch (buf->iFormat)
  {
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
    CLog::Log(LOGERROR, "fbout: unknown input format: %d\n", buf->iFormat);
    return false;
  }

  task.input.paddr = (int)buf->pPhysAddr;

  EDEINTERLACEMODE mDeintMode = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode;
  //EINTERLACEMETHOD mInt       = CMediaSettings::Get().GetCurrentVideoSettings().m_InterlaceMethod;

  bool deinterlacing = ((mDeintMode == VS_DEINTERLACEMODE_AUTO) && AutoMode()) ||
                        (mDeintMode == VS_DEINTERLACEMODE_FORCE);

  // Setup deinterlacing if enabled
  if (deinterlacing)
  {
    VpuFieldType fieldType;

    task.input.deinterlace.enable = 1;
    task.input.deinterlace.motion = HIGH_MOTION;
    fieldType = buf->GetFieldType();

    switch (fieldType)
    {
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
  }

  task.output.width = m_fb.Width();
  task.output.height = m_fb.Height();
  task.output.format = m_fb.Format();
  task.output.paddr = m_fb.GetPagePhysAddr();

  // Setup viewport cropping
  CRectInt cropRect;
  if (crop != NULL)
  {
    cropRect.x1 = Align(crop->x1,8);
    cropRect.y1 = Align(crop->y1,8);
    cropRect.x2 = Align2(crop->x2,8);
    cropRect.y2 = Align2(crop->y2,8);
  }
  else
  {
    cropRect.x1 = 0;
    cropRect.y1 = 0;
    cropRect.x2 = m_fb.Width();
    cropRect.y2 = m_fb.Height();
  }

  task.output.crop.pos.x = cropRect.x1;
  task.output.crop.pos.y = cropRect.y1;
  task.output.crop.w = cropRect.Width();
  task.output.crop.h = cropRect.Height();

  m_fb.SetViewport(cropRect);

  ret = IPU_CHECK_ERR_INPUT_CROP;
  while ( ret != IPU_CHECK_OK && ret > IPU_CHECK_ERR_MIN ) {
    ret = ioctl(m_ipuHandle, IPU_CHECK_TASK, &task);
    switch (ret)
    {
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
        CLog::Log(LOGWARNING, "fbout: unhandled IPU check error: %d\n", ret);
        return false;
    }
  }

  if ( (ret = ioctl(m_ipuHandle, IPU_QUEUE_TASK, &task)) < 0 )
  {
    CLog::Log(LOGERROR, "IPU task failed: %s\n", strerror(errno));
    return false;
  }

  m_fb.Finish();

  return true;
}

bool CDVDVideoCodecIMXIPUBuffers::SwapFB()
{
  return m_fb.Swap();
}

CDVDVideoMixerIMX::CDVDVideoMixerIMX(CDVDVideoCodecIMXIPUBuffers *proc)
  : CThread("IMX6 Mixer")
  , m_beginInput(0), m_endInput(0), m_bufferedInput(0)
  , m_beginOutput(0), m_endOutput(0), m_bufferedOutput(0)
  , m_proc(proc)
{
}

CDVDVideoMixerIMX::~CDVDVideoMixerIMX()
{
  Dispose();
}

void CDVDVideoMixerIMX::SetCapacity(int nInput, int nOutput)
{
	Reset();
	m_input.resize(nInput);
	m_output.resize(nOutput);
}

void CDVDVideoMixerIMX::Start()
{
  Create();
}

void CDVDVideoMixerIMX::Reset()
{
  CSingleLock lk(m_monitor);

  // Release all still referenced buffers
  for (size_t i = 0; i < m_input.size(); ++i)
  {
    SAFE_RELEASE(m_input[i]);
    m_input[i] = NULL;
  }

  for (size_t i = 0; i < m_output.size(); ++i)
  {
    SAFE_RELEASE(m_output[i]);
    m_output[i] = NULL;
  }

  // Reset ring buffer
  m_beginInput = m_endInput = m_bufferedInput = 0;
  m_beginOutput = m_endOutput = m_bufferedOutput = 0;

  m_inputNotFull.notifyAll();
  m_outputNotFull.notifyAll();
}

void CDVDVideoMixerIMX::Dispose()
{
  StopThread();
  Reset();
}

bool CDVDVideoMixerIMX::IsActive() {
  return IsRunning();
}

CDVDVideoCodecIMXBuffer *CDVDVideoMixerIMX::Process(CDVDVideoCodecIMXVPUBuffer *buffer)
{
  CSingleLock lk(m_monitor);
  CDVDVideoCodecIMXBuffer *r;

  if (m_bStop)
  {
    m_inputNotEmpty.notifyAll();
    m_outputNotFull.notifyAll();
    SAFE_RELEASE(buffer);
    return NULL;
  }

  if (m_bufferedOutput)
  {
    // Pop the output
    r = m_output[m_beginOutput];
    m_output[m_beginOutput] = NULL;
    m_beginOutput = (m_beginOutput+1) % m_output.size();
    --m_bufferedOutput;
    m_outputNotFull.notifyAll();
  }
  else
    r = NULL;

  // Flush call?
  if (!buffer)
    return r;

  // If the input queue is full, wait for a free slot
  while ((m_bufferedInput == m_input.size()) && !m_bStop)
    m_inputNotFull.wait(lk);

  if (m_bStop)
  {
    m_inputNotEmpty.notifyAll();
    m_outputNotFull.notifyAll();
    buffer->Release();
    return r;
  }

  // Store the value
  m_input[m_endInput] = buffer;
  m_endInput = (m_endInput+1) % m_input.size();
  ++m_bufferedInput;
  m_inputNotEmpty.notifyAll();

  //CLog::Log(LOGNOTICE, "Pushed input frame %x\n", (int)buffer);

  return r;
}

void CDVDVideoMixerIMX::OnStartup()
{
  CLog::Log(LOGNOTICE, "CDVDVideoMixerIMX::OnStartup: Mixer Thread created");
}

void CDVDVideoMixerIMX::OnExit()
{
  CLog::Log(LOGNOTICE, "CDVDVideoMixerIMX::OnExit: Mixer Thread terminated");
}

void CDVDVideoMixerIMX::StopThread(bool bWait /*= true*/)
{
  CThread::StopThread(false);
  m_inputNotFull.notifyAll();
  m_inputNotEmpty.notifyAll();
  m_outputNotFull.notifyAll();
  if (bWait)
    CThread::StopThread(true);
}

void CDVDVideoMixerIMX::Process()
{
  while (!m_bStop)
  {
    // Blocking until an input is available
    CDVDVideoCodecIMXVPUBuffer *inputBuffer = GetNextInput();
    if (inputBuffer)
    {
      EDEINTERLACEMODE mDeintMode = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode;
      //EINTERLACEMETHOD mInt       = CMediaSettings::Get().GetCurrentVideoSettings().m_InterlaceMethod;

      if ((mDeintMode == VS_DEINTERLACEMODE_OFF)
       || ((mDeintMode == VS_DEINTERLACEMODE_AUTO) && !m_proc->AutoMode()))
      {
        // Forward input buffer. Blocking until a free output slot is
        // available. The buffer is reference counted in PushOutput.
        PushOutput(inputBuffer);
      }
      else
      {
        CDVDVideoCodecIMXBuffer *outputBuffer;

        /*
        bool isSD = (inputBuffer->iWidth < 1024) && (inputBuffer->iHeight < 1024);
        bool hasPreviousBuffer = (inputBuffer->GetPreviousBuffer() != NULL) &&
                                 inputBuffer->GetPreviousBuffer()->IsValid();
        */

        // Disable lowMotion for now due to worse deinterlacing quality
        bool lowMotion = false;
        //bool lowMotion = isSD && hasPreviousBuffer;

        // Currently double rate introduces oscillating scanlines (either wrong
        // buffer setup or some other kernel issue) so we disable it completely
        // for the time being
        bool doubleRate = false;
        //bool doubleRate = isSD && hasPreviousBuffer;

        m_proc->SetDoubleRate(doubleRate);

        // In case of double rate, process the buffer again to create
        // an interpolated frame out of previous and current
        if (m_proc->DoubleRate())
        {
          double pts;
          if ((inputBuffer->GetPreviousBuffer()->GetPts() != DVD_NOPTS_VALUE)
           && (inputBuffer->GetPts() != DVD_NOPTS_VALUE))
            pts = (inputBuffer->GetPts()+inputBuffer->GetPreviousBuffer()->GetPts())*0.5;
          else
            pts = DVD_NOPTS_VALUE;

          // Wait for free slot
          WaitForFreeOutput();

          m_proc->SetInterpolatedFrame(true);
          outputBuffer = ProcessFrame(inputBuffer, lowMotion);

          // Queue the output if any
          if (outputBuffer)
          {
            // Update pts with interpolated value
            outputBuffer->SetPts(pts);
            outputBuffer->SetDts(DVD_NOPTS_VALUE);

            // Blocking until a free output slot is available. The buffer is
            // reference counted in PushOutput
            PushOutput(outputBuffer);
          }
        }

        // Wait for free slot
        WaitForFreeOutput();

        m_proc->SetInterpolatedFrame(false);
        outputBuffer = ProcessFrame(inputBuffer, lowMotion);

        // Queue the output if any
        if (outputBuffer)
        {
          // Blocking until a free output slot is available. The buffer is
          // reference counted in PushOutput
          PushOutput(outputBuffer);
        }
      }

      SAFE_RELEASE(inputBuffer);
    }
  }
}

CDVDVideoCodecIMXVPUBuffer *CDVDVideoMixerIMX::GetNextInput() {
  CSingleLock lk(m_monitor);
  while (!m_bufferedInput && !m_bStop)
    m_inputNotEmpty.wait(lk);

  if (m_bStop)
    return NULL;

  CDVDVideoCodecIMXVPUBuffer *v = m_input[m_beginInput];
  m_input[m_beginInput] = NULL;
  m_beginInput = (m_beginInput+1) % m_input.size();
  --m_bufferedInput;
  m_inputNotFull.notifyAll();

  //CLog::Log(LOGNOTICE, "Popped input frame %x\n", (int)v);

  return v;
}

void CDVDVideoMixerIMX::WaitForFreeOutput() {
  CSingleLock lk(m_monitor);

  // Output queue is full, wait for a free slot
  while (m_bufferedOutput == m_output.size() && !m_bStop)
    m_outputNotFull.wait(lk);
}

bool CDVDVideoMixerIMX::PushOutput(CDVDVideoCodecIMXBuffer *v) {
  CSingleLock lk(m_monitor);

  v->Lock();

  // If closed return false
  if (m_bStop)
  {
    v->Release();
    return false;
  }

  // Store the value
  m_output[m_endOutput] = v;
  m_endOutput = (m_endOutput+1) % m_output.size();
  ++m_bufferedOutput;

  return true;
}

CDVDVideoCodecIMXBuffer *CDVDVideoMixerIMX::ProcessFrame(CDVDVideoCodecIMXVPUBuffer *inputBuffer,
                                                         bool lowMotion)
{
  CDVDVideoCodecIMXBuffer *outputBuffer;

#ifdef IMX_PROFILE_BUFFERS
  unsigned long long current = XbmcThreads::SystemClockMillis();
#endif
//#define DUMMY_DEINTERLACER
#ifdef DUMMY_DEINTERLACER
  Sleep(35);
  outputBuffer = inputBuffer;
#else
  outputBuffer = m_proc->Process(inputBuffer, lowMotion);
#endif
#ifdef IMX_PROFILE_BUFFERS
  CLog::Log(LOGNOTICE, "+P  %f  %lld\n", outputBuffer->GetPts(),
            XbmcThreads::SystemClockMillis()-current);
#endif

  return outputBuffer;
}
