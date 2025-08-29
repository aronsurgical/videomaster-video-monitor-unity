using System;
using System.Runtime.InteropServices;
using UnityEngine;

public class UnityDeltacast : MonoBehaviour {
    // On Windows, Unity looks in the project root or Plugins folder for DLLs
    const string DLL_NAME = "unityDeltacast";

    [DllImport(DLL_NAME)]
    private static extern void InitLibrary();

    [DllImport(DLL_NAME)]
    private static extern int AddInts(int a, int b);

    [DllImport(DLL_NAME)]
    private static extern IntPtr GetMessage();

    [DllImport(DLL_NAME)] static extern void StartCapture(int deviceId, int streamId);
    [DllImport(DLL_NAME)] static extern void StopCapture();
    [DllImport(DLL_NAME)] static extern int GetFrame(IntPtr dst, int maxSize);

    Texture2D tex;
    byte[] buffer;
    GCHandle handle;
    IntPtr bufferPtr;

    public Material matToShow;

    void Start() {
        InitLibrary();
        //int result = AddInts(3, 4);
        //Debug.Log("3 + 4 = " + result);

        //string msg = Marshal.PtrToStringAnsi(GetMessage());

        int width = 1920, height = 1080; // adapt to detected signal
        tex = new Texture2D(width, height, TextureFormat.BGRA32, false);
        buffer = new byte[width * height * 4]; // RGBA target
        handle = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        bufferPtr = handle.AddrOfPinnedObject();

        StartCapture(0, 0); // board 0, stream RX0
        //StartCapture(0, 4); // board 0, stream RX4, this is HDMI
        matToShow.mainTexture = tex;
    }

    void Update() {
        int bytes = GetFrame(bufferPtr, buffer.Length);
        //Debug.Log(bytes);
        if(bytes > 0) {
            tex.LoadRawTextureData(buffer);
            tex.Apply();
        }
        //matToShow.mainTexture = tex;
        //Graphics.Blit(tex, (RenderTexture)null); // show on screen
        printCleanMsg();
    }

    void OnDestroy() {
        StopCapture();
        handle.Free();
    }

    public void printCleanMsg() {
        string rawMsg = Marshal.PtrToStringAnsi(GetMessage());
        if(rawMsg.Length > 0) {
            Debug.Log(rawMsg);
        }
    }
}
