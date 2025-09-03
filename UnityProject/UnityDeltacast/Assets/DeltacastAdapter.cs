using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

public class DeltacastAdapter {
    // On Windows, Unity looks in the project root or Plugins folder for DLLs
    const string DLL_NAME = "unityDeltacast";

    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern void InitLibrary();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern int AddInts(int a, int b);
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern int GetWidth();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern int GetHeight();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr GetMessage();
    [DllImport(DLL_NAME, CallingConvention = CallingConvention.Cdecl)] private static extern void StartCapture(int deviceId, int streamId, int buffer_depth);
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



    public void Init(int board, int stream, int width, int height, bool stereo=false) {
        if(initialized) {
            Stop();
        }
        InitLibrary();
        //int result = AddInts(3, 4);
        //Debug.Log("3 + 4 = " + result);

        //string msg = Marshal.PtrToStringAnsi(GetMessage());

        curW = Math.Max(1, width);
        curH = Math.Max(1, height);
        if(!stereo) {
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



        
        if(!stereo) {
            StartCapture(board, stream, 2); // board 0, stream RX0, bufferdepth
        }
        else {
            StartCaptureStereo(board, stream, 2); // board 0, stream RX0, bufferdepth
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
}
