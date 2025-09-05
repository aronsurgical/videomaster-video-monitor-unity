using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using static UnityDeltacast;

public class DeltacastAdapter {
    // On Windows, Unity looks in the project root or Plugins folder for DLLs
    const string DLL_NAME = "unityDeltacast";

    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern void InitLibrary();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern int AddInts(int a, int b);
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern int GetWidth();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern int GetHeight();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr GetMessage();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern void StartCapture(int device_id, int rx_stream_id, int buffer_depth, char inputType, uint requested_width, uint requested_height, uint progressive, uint framerate, VHD_DV_CS cable_color_space, VHD_DV_SAMPLING cable_sampling, VHD_VIDEOSTANDARD video_standard, VHD_CLOCKDIVISOR clock_divisor, VHD_INTERFACE video_interface, uint fieldMerge, VHD_BUFFERPACKING buffer_packing);
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern void StartCaptureStereo(int deviceId, int streamId, int buffer_depth);
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern void StopCapture();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern int GetFrame(IntPtr dst, int maxSize);

    public Texture2D tex;

    private byte[] buffer;

    private GCHandle handle;

    private IntPtr bufferPtr;


    public bool initialized = false;


    private int curW;

    private int curH;

    public uint width;
    public uint height;
    public int boardID;
    public int streamID;
    public StereoConfig stereoConfig;
    public VHD_DV_CS cable_color_space;
    public VHD_DV_SAMPLING cable_sampling;
    public bool progressive;
    public uint framerate;
    public VHD_VIDEOSTANDARD video_standard;
    public VHD_CLOCKDIVISOR clock_divisor;
    public VHD_INTERFACE video_interface;
    public bool fieldMerge;
    public VHD_BUFFERPACKING buffer_packing;
    public VIDEOINPUT dvSdiAuto;



    public void Init() {
        if(initialized) {
            Stop();
        }
        InitLibrary();
        //int result = AddInts(3, 4);
        //Debug.Log("3 + 4 = " + result);

        //string msg = Marshal.PtrToStringAnsi(GetMessage());

        curW = Math.Max(1, (int)width);
        curH = Math.Max(1, (int)height);
        if(!stereoConfig.Equals(StereoConfig.STEREO)) {
            tex = new Texture2D(curW, curH, TextureFormat.BGRA32, false);
            buffer = new byte[curW * curH * 4]; // BGRA target
        }
        else {
            tex = new Texture2D(curW * 2, curH, TextureFormat.BGRA32, false);
            buffer = new byte[curW * curH * 4 * 2]; // BGRA target
        }

        try {
            handle = GCHandle.Alloc(buffer, GCHandleType.Pinned);
            bufferPtr = handle.AddrOfPinnedObject();
        }
        catch {
            // make sure to release anything partially allocated
            if(handle.IsAllocated) {
                handle.Free();
            }
            throw;
        }



        
        if(!stereoConfig.Equals(StereoConfig.STEREO)) {
            char dvSdiAutoAsChar;
            if(dvSdiAuto.Equals(VIDEOINPUT.SDI)) {
                dvSdiAutoAsChar = 'S';
            }
            else if (dvSdiAuto.Equals(VIDEOINPUT.DV)) {
                dvSdiAutoAsChar = 'D';
            }
            else {
                dvSdiAutoAsChar = 'A';
            }
            uint progressiveuint = progressive ? (uint)1 : (uint)0;
            uint fieldMergeuint = fieldMerge ? (uint)1 : (uint)0;
            StartCapture(boardID, streamID, 2, dvSdiAutoAsChar, width, height, progressiveuint, framerate, cable_color_space, cable_sampling, video_standard,clock_divisor , video_interface, fieldMergeuint, buffer_packing); // board 0, stream RX0, bufferdepth
        }
        else {
            StartCaptureStereo(boardID, streamID, 2); // board 0, stream RX0, bufferdepth
        }


        initialized = true;
    }

    public int Update() {
        if(!initialized) {
            return 0;
        }
        // If native resolution changed, rebuild texture & buffer
        int w = GetWidth();
        int h = GetHeight();
        if(w > 0 && h > 0 && (w != curW || h != curH)) {
            RecreateResources(w, h);
        }

        int bytes = GetFrame(bufferPtr, buffer.Length);
        //Debug.Log(bytes);
        if(bytes > 0) {
            tex.LoadRawTextureData(bufferPtr, bytes);
            tex.Apply(false);
        }



        PrintCleanMsg();
        return bytes;
    }

    void RecreateResources(int w, int h) {
        // stop using old pinned memory during resize
        if(handle.IsAllocated) {
            handle.Free();
        }

        curW = w;
        curH = h;

        // reallocate managed buffer & pin again
        buffer = new byte[curW * curH * 4];
        handle = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        bufferPtr = handle.AddrOfPinnedObject();

        if(tex == null) {
            tex = new Texture2D(curW, curH, TextureFormat.BGRA32, false);
        }
        else {
            tex.Reinitialize(curW, curH, TextureFormat.BGRA32, false);
            tex.Apply(false, false);

        }
    }

    public void Stop() {
        try {
            if(tex != null) { UnityEngine.Object.Destroy(tex); tex = null; }
            StopCapture();
        }
        finally {
            if(handle.IsAllocated) {
                handle.Free();
                bufferPtr = IntPtr.Zero;   // invalidate the pointer
                buffer = null;           // let GC reclaim the array
            }
            initialized = false;
        }
    }

    public void PrintCleanMsg() {
        IntPtr p = GetMessage();
        if(p == IntPtr.Zero) {
            return;
        }

        string rawMsg = Marshal.PtrToStringAnsi(p);
        if(!string.IsNullOrEmpty(rawMsg)) {
            Debug.Log(rawMsg);
        }
    }

    public Vector2Int getResolution() {
        return new Vector2Int(GetWidth(), GetHeight());
    }


    public enum VHD_VIDEOSTANDARD : int {
        VHD_VIDEOSTD_S274M_1080p_25Hz = 0,  /*! SMPTE 274M - HD 1080p @ 25Hz standard */
        VHD_VIDEOSTD_S274M_1080p_30Hz,      /*! SMPTE 274M - HD 1080p @ 30Hz standard (default) */
        VHD_VIDEOSTD_S274M_1080i_50Hz,      /*! SMPTE 274M - HD 1080i @ 50Hz standard */
        VHD_VIDEOSTD_S274M_1080i_60Hz,      /*! SMPTE 274M - HD 1080i @ 60Hz standard */
        VHD_VIDEOSTD_S296M_720p_50Hz,       /*! SMPTE 296M - HD 720p @ 50Hz standard */
        VHD_VIDEOSTD_S296M_720p_60Hz,       /*! SMPTE 296M - HD 720p @ 60Hz standard */
        VHD_VIDEOSTD_S259M_PAL,             /*! SMPTE 259M - SD PAL standard */
        VHD_VIDEOSTD_S259M_NTSC_487,        /*! SMPTE 259M - SD NTSC standard */
        VHD_VIDEOSTD_S274M_1080p_24Hz,      /*! SMPTE 274M - HD 1080p @ 24Hz standard */
        VHD_VIDEOSTD_S274M_1080p_60Hz,      /*! SMPTE 274M - 3G 1080p @ 60Hz standard */
        VHD_VIDEOSTD_S274M_1080p_50Hz,      /*! SMPTE 274M - 3G 1080p @ 50Hz standard */
        VHD_VIDEOSTD_S274M_1080psf_24Hz,    /*! SMPTE 274M - HD 1080psf @ 24Hz standard */
        VHD_VIDEOSTD_S274M_1080psf_25Hz,    /*! SMPTE 274M - HD 1080psf @ 25Hz standard */
        VHD_VIDEOSTD_S274M_1080psf_30Hz,    /*! SMPTE 274M - HD 1080psf @ 30Hz standard */
        VHD_VIDEOSTD_S296M_720p_24Hz,       /*! SMPTE 296M - HD 720p @ 24Hz standard */
        VHD_VIDEOSTD_S296M_720p_25Hz,       /*! SMPTE 296M - HD 720p @ 25Hz standard */
        VHD_VIDEOSTD_S296M_720p_30Hz,       /*! SMPTE 296M - HD 720p @ 30Hz standard */
        VHD_VIDEOSTD_S2048M_2048p_24Hz,     /*! SMPTE 2048M - HD 2048p @ 24 Hz standard */
        VHD_VIDEOSTD_S2048M_2048p_25Hz,     /*! SMPTE 2048M - HD 2048p @ 25 Hz standard */
        VHD_VIDEOSTD_S2048M_2048p_30Hz,     /*! SMPTE 2048M - HD 2048p @ 30 Hz standard */
        VHD_VIDEOSTD_S2048M_2048psf_24Hz,   /*! SMPTE 2048M - HD 2048psf @ 24 Hz standard */
        VHD_VIDEOSTD_S2048M_2048psf_25Hz,   /*! SMPTE 2048M - HD 2048psf @ 25 Hz standard */
        VHD_VIDEOSTD_S2048M_2048psf_30Hz,   /*! SMPTE 2048M - HD 2048psf @ 30 Hz standard */
        VHD_VIDEOSTD_S2048M_2048p_60Hz,     /*! SMPTE 2048M - 3G 2048p @ 60Hz standard */
        VHD_VIDEOSTD_S2048M_2048p_50Hz,     /*! SMPTE 2048M - 3G 2048p @ 50Hz standard */
        VHD_VIDEOSTD_S2048M_2048p_48Hz,     /*! SMPTE 2048M - 3G 2048p @ 50Hz standard */
        VHD_VIDEOSTD_3840x2160p_24Hz,       /*! 3840x2160 - 4x HD 1080p @ 24Hz merged */
        VHD_VIDEOSTD_3840x2160p_25Hz,       /*! 3840x2160 - 4x HD 1080p @ 25Hz merged */
        VHD_VIDEOSTD_3840x2160p_30Hz,       /*! 3840x2160 - 4x HD 1080p @ 30Hz merged */
        VHD_VIDEOSTD_3840x2160p_50Hz,       /*! 3840x2160 - 4x 3G 1080p @ 50Hz merged */
        VHD_VIDEOSTD_3840x2160p_60Hz,       /*! 3840x2160 - 4x 3G 1080p @ 60Hz merged */
        VHD_VIDEOSTD_4096x2160p_24Hz,       /*! 4096x2160 - 4x HD 2048p @ 24Hz merged */
        VHD_VIDEOSTD_4096x2160p_25Hz,       /*! 4096x2160 - 4x HD 2048p @ 25Hz merged */
        VHD_VIDEOSTD_4096x2160p_30Hz,       /*! 4096x2160 - 4x HD 2048p @ 30Hz merged */
        VHD_VIDEOSTD_4096x2160p_48Hz,       /*! 4096x2160 - 4x 3G 2048p @ 48Hz merged */
        VHD_VIDEOSTD_4096x2160p_50Hz,       /*! 4096x2160 - 4x 3G 2048p @ 50Hz merged */
        VHD_VIDEOSTD_4096x2160p_60Hz,       /*! 4096x2160 - 4x 3G 2048p @ 60Hz merged */
        VHD_VIDEOSTD_S259M_NTSC_480,        /*! SMPTE 259M - SD NTSC standard - 480 active lines */
        VHD_VIDEOSTD_7680x4320p_24Hz,       /*! 7680x4320 - 4x 6G 3840x2160 @ 24Hz merged */
        VHD_VIDEOSTD_7680x4320p_25Hz,       /*! 7680x4320 - 4x 6G 3840x2160 @ 25Hz merged */
        VHD_VIDEOSTD_7680x4320p_30Hz,       /*! 7680x4320 - 4x 6G 3840x2160 @ 30Hz merged */
        VHD_VIDEOSTD_7680x4320p_50Hz,       /*! 7680x4320 - 4x 12G 3840x2160 @ 50Hz merged */
        VHD_VIDEOSTD_7680x4320p_60Hz,       /*! 7680x4320 - 4x 12G 3840x2160 @ 60Hz merged */
        VHD_VIDEOSTD_3840x2160psf_24Hz,     /*! 3840x2160 - 4x HD 1080psf @ 24Hz merged */
        VHD_VIDEOSTD_3840x2160psf_25Hz,     /*! 3840x2160 - 4x HD 1080psf @ 25Hz merged */
        VHD_VIDEOSTD_3840x2160psf_30Hz,     /*! 3840x2160 - 4x HD 1080psf @ 30Hz merged */
        VHD_VIDEOSTD_4096x2160psf_24Hz,     /*! 4096x2160 - 4x HD 2048psf @ 24Hz merged */
        VHD_VIDEOSTD_4096x2160psf_25Hz,     /*! 4096x2160 - 4x HD 2048psf @ 25Hz merged */
        VHD_VIDEOSTD_4096x2160psf_30Hz,     /*! 4096x2160 - 4x HD 2048psf @ 30Hz merged */
        VHD_VIDEOSTD_8192x4320p_24Hz,       /*! 8192x4320 - 4x 6G 2048psf @ 24Hz merged */
        VHD_VIDEOSTD_8192x4320p_25Hz,       /*! 8192x4320 - 4x 6G 2048psf @ 25Hz merged */
        VHD_VIDEOSTD_8192x4320p_30Hz,       /*! 8192x4320 - 4x 6G 2048psf @ 30Hz merged */
        VHD_VIDEOSTD_8192x4320p_48Hz,       /*! 8192x4320 - 4x 12G 2048psf @ 48Hz merged */
        VHD_VIDEOSTD_8192x4320p_50Hz,       /*! 8192x4320 - 4x 12G 2048psf @ 50Hz merged */
        VHD_VIDEOSTD_8192x4320p_60Hz,       /*! 8192x4320 - 4x 12G 2048psf @ 60Hz merged */
        NB_VHD_VIDEOSTANDARDS
    }

    public enum VHD_CLOCKDIVISOR : int {
        VHD_CLOCKDIV_1 = 0,           /*!_VHD_CLOCKDIVISOR::VHD_CLOCKDIV_1
                                    Clock rate divisor to produce a frame rate of 24,25,50,30 or
                                    60 fps (default)                                             */
        VHD_CLOCKDIV_1001,            /*!_VHD_CLOCKDIVISOR::VHD_CLOCKDIV_1001
                                    Clock rate divisor to produce a frame rate of 23.98, 29.97 or
                                    59.94 fps                                                     */
        NB_VHD_CLOCKDIVISORS
    }
    public enum VHD_INTERFACE : int {
        VHD_INTERFACE_DEPRECATED,                    /*! */
        VHD_INTERFACE_SD_259,                        /*! SD SMPTE 259 interface*/
        VHD_INTERFACE_HD_292_1,                      /*! HD SMPTE 291-1 interface*/
        VHD_INTERFACE_HD_DUAL,                       /*! HD Dual Link SMPTE 372 interface*/
        VHD_INTERFACE_3G_A_425_1,                    /*! 3G-A SMPTE 425-1 interface*/
        VHD_INTERFACE_4XHD_QUADRANT,                 /*! 4xHD interface (4K image is split into 4 quadrants for transport)*/
        VHD_INTERFACE_4X3G_A_QUADRANT,               /*! 4x3G-A interface (4K image is split into 4 quadrants for transport)*/
        VHD_INTERFACE_SD_DUAL,                       /*! SD Dual Link (application of SMPTE 372 to SD) */
        VHD_INTERFACE_3G_A_DUAL,                     /*! 3G-A Dual interface (application of SMPTE 372 to 3GA)*/
        VHD_INTERFACE_3G_B_DL_425_1,                 /*! 3G-B SMPTE 425-1 interface for mapping of SMPTE ST 372 dual-link*/
        VHD_INTERFACE_4X3G_B_DL_QUADRANT,            /*! 4x3G-B SMPTE 425-1 interface for mapping of SMPTE ST 372 dual-link (4K image is split into 4 quadrants for transport)*/
        VHD_INTERFACE_2X3G_B_DS_425_3,               /*! 2x3G-B SMPTE 425-3 interface  (4K image is split into 4 images with the 2-sample interleave division rule for transport)*/
        VHD_INTERFACE_4X3G_A_425_5,                  /*! 4x3G-A SMPTE 425-5 interface  (4K image is split into 4 images with the 2-sample interleave division rule for transport)*/
        VHD_INTERFACE_4X3G_B_DL_425_5,               /*! 4x3G-B SMPTE 425-5 interface  (4K image is split into 4 images with the 2-sample interleave division rule for transport)*/
        VHD_INTERFACE_3G_B_DL_425_1_DUAL,            /*! 3G-B SMPTE 425-1 interface for mapping of SMPTE ST 372 dual-link, dual interface*/
        VHD_INTERFACE_2X3G_B_DS_425_3_DUAL,          /*! 2x3G-B SMPTE 425-3 interface  (4K image is split into 4 images with the 2-sample interleave division rule for transport), dual interface*/
        VHD_INTERFACE_4XHD_QUADRANT_DUAL,            /*! 4xHD interface (4K image is split into 4 quadrants for transport), dual interface*/
        VHD_INTERFACE_4X3G_A_QUADRANT_DUAL,          /*! 4x3G-A interface (4K image is split into 4 quadrants for transport), dual interface*/
        VHD_INTERFACE_4X3G_A_425_5_DUAL,             /*! 4x3G-A SMPTE 425-5 interface  (4K image is split into 4 images with the 2-sample interleave division rule for transport), dual interface*/
        VHD_INTERFACE_4X3G_B_DL_QUADRANT_DUAL,       /*! 4x3G-B SMPTE 425-1 interface for mapping of SMPTE ST 372 dual-link (4K image is split into 4 quadrants for transport), dual interface*/
        VHD_INTERFACE_4X3G_B_DL_425_5_DUAL,          /*! 4x3G-B SMPTE 425-5 interface  (4K image is split into 4 images with the 2-sample interleave division rule for transport), dual interface*/
        VHD_INTERFACE_TICO_3G_A_425_1,               /*! 4K image transports with TICO over 3G-A SMPTE 425-1 interface*/
        VHD_INTERFACE_TICO_HD_292_1,                 /*! 4K image transports with TICO over HD SMPTE 291-1 interface*/
        VHD_INTERFACE_6G_2081_10,                    /*! 6G over SMPTE 2081-10 interface*/
        VHD_INTERFACE_12G_2082_10,                   /*! 12G over SMPTE 2082-10 interface*/
        VHD_INTERFACE_3G_B_DS_425_1,                 /*! 3G-B dual stream SMPTE 425-1 interface*/
        VHD_INTERFACE_4X6G_2081_10_QUADRANT,         /*! 4x6G over SMPTE 2081-10 interface (8K image is split into 4 quadrants for transport)*/
        VHD_INTERFACE_4X12G_2082_10_QUADRANT,        /*! 4x12G over SMPTE 2082-10 interface (8K image is split into 4 quadrants for transport)*/
        VHD_INTERFACE_6G_2081_10_DUAL,               /*! 6G over SMPTE 2081-10, dual interface */
        VHD_INTERFACE_12G_2082_10_DUAL,              /*! 12G over SMPTE 2082-10, dual interface */
        VHD_INTERFACE_4X6G_2081_12,                  /*! 4x6G over SMPTE 2081-12 */
        VHD_INTERFACE_4X12G_2082_12,                 /*! 4x12G over SMPTE 2082-12*/
        NB_VHD_INTERFACE
    }

    public enum VHD_BUFFERPACKING : int {
        VHD_BUFPACK_VIDEO_YUV422_8 = 0,              /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUV422_8
                                                   4:2:2 8-bit YUV video packing (default, detailed <link YUV422_8 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_YUVK4224_8,                /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUVK4224_8
                                                   4:2:2:4 8-bit YUVK video packing (detailed <link YUVK4224_8 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_YUV422_10,                 /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUV422_10
                                                   4:2:2 10-bit YUV video packing (detailed <link YUV422_10 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_YUVK4224_10,               /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUVK4224_10
                                                   4:2:2:4 10-bit YUVK video packing (detailed <link YUVK4224_10 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_YUV4444_8,                 /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUV4444_8
                                                   4:4:4:4 8-bit YUV video packing (K forced to blank, detailed <link YUV4444_8/ YUVK4444_8 Video Packings, here>) */
        VHD_BUFPACK_VIDEO_YUVK4444_8,                /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUVK4444_8
                                                   4:4:4:4 8-bit YUVK video packing (detailed <link YUV4444_8/ YUVK4444_8 Video Packings, here>) */
        VHD_BUFPACK_VIDEO_YUV444_10,                 /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUV444_10
                                                   4:4:4 10-bit YUV video packing (detailed <link YUV444_10 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_YUVK4444_10,               /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUVK4444_10
                                                   4:4:4:4 10-bit YUVK video packing (detailed <link YUVK4444_10 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_RGB_32,                    /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_RGB_32
                                                   4:4:4 8-bit RGB video packing (detailed <link RGB_32 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_RGBA_32,                   /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_RGBA_32
                                                   4:4:4:4 8-bit RGBA video packing (detailed <link RGBA_32 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_RGB_24,                    /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_RGB_24
                                                   4:4:4 8-bit RGB video packing (24 bits) (detailed <link RGB_24 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YVU420_8,           /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YVU420_8
                                                   4:2:0 8-bit YUV planar video packing (YV12)(detailed <link YV12 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YUV420_8,           /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YUV420_8
                                                   4:2:0 8-bit YUV planar video packing (I420)(detailed <link I420 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YVU420_10_MSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YVU420_10_MSB_PAD
                                                   4:2:0 10-bit MSB PAD YVU planar video packing (detailed <link YVU420_10_MSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YVU420_10_LSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YVU420_10_LSB_PAD
                                                   4:2:0 10-bit LSB PAD YVU planar video packing (detailed <link YVU420_10_LSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YUV420_10_MSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YUV420_10_MSB_PAD
                                                   4:2:0 10-bit MSB PAD YUV planar video packing (detailed <link YUV420_10_MSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YUV420_10_LSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YUV420_10_LSB_PAD
                                                   4:2:0 10-bit LSB PAD YUV planar video packing (detailed <link YUV420_10_LSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_RGB_64,                    /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_RGB_64
                                                   4:4:4:4 16-bit RGB video packing (detailed <link RGB_64 Video Packing, here>)  */
        VHD_BUFPACK_VIDEO_YUV422_16,                 /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUV422_16
                                                   4:2:2 16-bit YUV video packing (detailed <link YUV422_16 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_YUV444_8,                  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUV444_8
                                                   4:4:4 8-bit YUV video packing (detailed <link YUV444_8 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_ICTCP_422_8,              /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_ICTCP_422_8
                                                   4:2:2 8-bit ICtCp video packing (detailed <link ICTCP422_8 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_ICTCP_422_10,             /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_ICTCP_422_10
                                                   4:2:2 10-bit ICtCp video packing (detailed <link ICTCP422_10 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YUV422_10_LSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YUV422_10_LSB_PAD
                                                   4:2:2 10-bit LSB PAD YUV planar video packing (detailed <link YUV422_10_LSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YUV422_10_MSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YUV422_10_MSB_PAD
                                                   4:2:2 10-bit MSB PAD YUV planar video packing (detailed <link YUV422_10_MSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YVU422_10_LSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YVU422_10_LSB_PAD
                                                   4:2:2 10-bit LSB PAD YVU planar video packing (detailed <link YVU422_10_LSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YVU422_10_MSB_PAD,  /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YVU422_10_MSB_PAD
                                                   4:2:2 10-bit MSB PAD YVU planar video packing (detailed <link YVU422_10_MSB_PAD, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YUV422_8,           /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YUV422_8
                                                   4:2:2 8-bit YUV planar video packing (detailed <link PLANAR_YUV422_8, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_YVU422_8,           /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_YVU422_8
                                                   4:2:2 8-bit YVU planar video packing (detailed <link PLANAR_YVU422_8, here>) */
        VHD_BUFPACK_VIDEO_YUV422_10_NOPAD_BIGEND,    /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_YUV422_10_NOPAD_BIGEND
                                                   4:2:2 10-bit NOPAD BIGEND YUV video packing (detailed <link YUV422_10_NOPAD_BIGEND Video Packing, here>) */
        VHD_BUFPACK_VIDEO_PALETTE_RGBA_8,            /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PALETTE_RGBA_8
                                                   8-bit palette RGBA video packing (detailed <link RGBA_8_PALETTE Video Packing, here>) */
        VHD_BUFPACK_VIDEO_PLANAR_NV12,               /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_PLANAR_NV12
                                                  4:2:0 8-bit semi-planar format video packing (detailed <link VHD_BUFPACK_VIDEO_PLANAR_NV12 Video Packing, here>) */
        VHD_BUFPACK_VIDEO_RGB444_10_LSB_PAD,         /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_RGB444_10_LSB_PAD
                                                   4:4:4 10-bit RGB video packing (detailed <link RGB444_10_LSB_PAD Video Packing, here>) */
        VHD_BUFPACK_VIDEO_RGBA4444_10_LSB_PAD,       /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_RGBA4444_10_LSB_PAD
                                                   4:4:4:4 10-bit RGBA video packing (detailed <link RGBA4444_10_LSB_PAD Video Packing, here>) */
        VHD_BUFPACK_VIDEO_RGBA4444_16,               /*!_VHD_BUFFERPACKING::VHD_BUFPACK_VIDEO_RGBA4444_16
                                                   4:4:4:4 16-bit RGBA video packing (detailed <link RGBA4444_16 Video Packing, here>) */
        NB_VHD_BUFPACKINGS
    }

    public enum VHD_DV_CS : int {
        VHD_DV_CS_RGB_FULL = 0,            /*! RGB full color space */
        VHD_DV_CS_RGB_LIMITED,           /*! RGB limited color space */
        VHD_DV_CS_YUV601,                /*! YUV 601 (SD) color space */
        VHD_DV_CS_YUV709,                /*! YUV 709 (HD) color space */
        VHD_DV_CS_XVYCC_601,             /*! CVYCC 601 (SD) color space */
        VHD_DV_CS_XVYCC_709,             /*! CVYCC 709 (HD) color space */
        VHD_DV_CS_YUV_601_FULL,          /*! YUV 601 (SD) full color space */
        VHD_DV_CS_YUV_709_FULL,          /*! YUV 709 (HD) full color space */
        VHD_DV_CS_SYCC_601,              /*! SYCC 601 color space */
        VHD_DV_CS_ADOBE_YCC_601,         /*! Adobe YCC 601 color space */
        VHD_DV_CS_ADOBE_RGB,             /*! Adobe RGB color space */
        VHD_DV_CS_BT2020_YCCBCCRC,       /*! ITU-R BT.2020 Y'cC'bcC'rc color space*/
        VHD_DV_CS_BT2020_RGB_LIMITED,    /*! ITU-R BT.2020 RGB LIMITED color space*/
        VHD_DV_CS_BT2020_RGB_FULL,       /*! ITU-R BT.2020 RGB FULL color space*/
        VHD_DV_CS_BT2020_YCBCR,          /*! ITU-R BT.2020 Y'C'bC'r color space*/
        VHD_DV_CS_DCI_P3_RGB_D65,        /*! DCI-P3 RGB(D65) color space*/
        VHD_DV_CS_DCI_P3_RGB_THEATER,    /*! DCI-P3 RGB (theater) color space*/
        NB_VHD_DV_CS
    }
    public enum VHD_DV_SAMPLING : int {
        VHD_DV_SAMPLING_4_2_0_8BITS,              /*! 4-2-0 8-bit sampling */
        VHD_DV_SAMPLING_4_2_0_10BITS,             /*! 4-2-0 10-bit sampling */
        VHD_DV_SAMPLING_4_2_0_12BITS,             /*! 4-2-0 12-bit sampling */
        VHD_DV_SAMPLING_4_2_0_16BITS,             /*! 4-2-0 16-bit sampling */
        VHD_DV_SAMPLING_4_2_2_8BITS,              /*! 4-2-2 8-bit sampling */
        VHD_DV_SAMPLING_4_2_2_10BITS,             /*! 4-2-2 10-bit sampling */
        VHD_DV_SAMPLING_4_2_2_12BITS,             /*! 4-2-2 12-bit sampling */
        VHD_DV_SAMPLING_4_2_2_16BITS,             /*! 4-2-2 16-bit sampling */
        VHD_DV_SAMPLING_4_4_4_6BITS,              /*! 4-4-4 6-bit sampling */
        VHD_DV_SAMPLING_4_4_4_8BITS,              /*! 4-4-4 8-bit sampling */
        VHD_DV_SAMPLING_4_4_4_10BITS,             /*! 4-4-4 10-bit sampling */
        VHD_DV_SAMPLING_4_4_4_12BITS,             /*! 4-4-4 12-bit sampling */
        VHD_DV_SAMPLING_4_4_4_16BITS,             /*! 4-4-4 16-bit sampling */
        NB_VHD_DV_SAMPLING,
    }

    public enum VIDEOINPUT : int {
        SDI,
        DV,
        AUTO
    }

}
