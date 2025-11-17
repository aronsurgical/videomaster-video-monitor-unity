using System;
using System.Runtime.InteropServices;
using UnityEngine;
using static DeltacastAdapter;


public class UnityDeltacast : MonoBehaviour {

    public Vector2Int requestedResolution = new Vector2Int(1280, 720);

    public string cameraName = "";

    public bool readCameraName = true;

    public bool initialized = false;

    public enum StereoConfig { Mono, SBSLeftOnly, SBSRightOnly, InterlacedEven, InterlacedOdd, SBSFullWidthLeftOnly, SBSFullWidthRightOnly };
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

    private DeltacastAdapter deltacastAdapter;

    public Material outputMat;

    public bool showDebug = false;

    public int captureIndex = 0;
    public void connectDeltacast() {
        if(readCameraName) {
            if(cameraName != null && cameraName.StartsWith("DELTACAST")) {
                string[] splitDeltacastName = cameraName.Split('+');
                boardID = int.Parse(splitDeltacastName[1]);
                streamID = int.Parse(splitDeltacastName[2]);
                dvSdiAuto = Enum.Parse<VIDEOINPUT>(splitDeltacastName[3].ToUpper());
                int w = int.Parse(splitDeltacastName[4]);
                int h = int.Parse(splitDeltacastName[5]);
                requestedResolution = new Vector2Int(w, h);
                progressive = !splitDeltacastName[6].Equals("0");
                framerate = uint.Parse(splitDeltacastName[7]);
                
                cable_color_space = Enum.Parse<VHD_DV_CS>(splitDeltacastName[8].ToUpper());
                cable_sampling = Enum.Parse<VHD_DV_SAMPLING>(splitDeltacastName[9].ToUpper());
                video_standard = Enum.Parse<VHD_VIDEOSTANDARD>(splitDeltacastName[10]);
                clock_divisor = Enum.Parse<VHD_CLOCKDIVISOR>(splitDeltacastName[11].ToUpper());
                video_interface = Enum.Parse<VHD_INTERFACE>(splitDeltacastName[12].ToUpper());
                fieldMerge = !splitDeltacastName[13].Equals("0");
                buffer_packing = Enum.Parse<VHD_BUFFERPACKING>(splitDeltacastName[14].ToUpper());
                stereoConfig = Enum.Parse<StereoConfig>(splitDeltacastName[15]);
                captureIndex = int.Parse(splitDeltacastName[16]);
            }
        }
        if(cameraName != null && cameraName.ToUpper().StartsWith("DELTACAST")) {
            deltacastAdapter = new DeltacastAdapter();
            deltacastAdapter.boardID = this.boardID;
            deltacastAdapter.streamID = this.streamID;
            deltacastAdapter.stereoConfig = this.stereoConfig;
            deltacastAdapter.dvSdiAuto = this.dvSdiAuto;
            deltacastAdapter.cable_color_space = this.cable_color_space;
            deltacastAdapter.cable_sampling = this.cable_sampling;
            deltacastAdapter.progressive = this.progressive;
            deltacastAdapter.framerate = this.framerate;
            deltacastAdapter.video_standard = this.video_standard;
            deltacastAdapter.clock_divisor = this.clock_divisor;
            deltacastAdapter.video_interface = this.video_interface;
            deltacastAdapter.fieldMerge = this.fieldMerge;
            deltacastAdapter.buffer_packing = this.buffer_packing;
            deltacastAdapter.width = (uint)requestedResolution.x;
            deltacastAdapter.height = (uint)requestedResolution.y;
            deltacastAdapter.showDebug = this.showDebug;
            deltacastAdapter.captureIndex = this.captureIndex;




            deltacastAdapter.Init();


            outputMat.mainTexture = deltacastAdapter.tex;
            Debug.Log(deltacastAdapter.tex.width);
            initialized = deltacastAdapter.initialized;

        }
    }

    void Start() {
        connectDeltacast();

    }
    void Update() {
        if(cameraName != null && cameraName.StartsWith("DELTACAST") && deltacastAdapter != null && deltacastAdapter.initialized) {
            deltacastAdapter.Update();
        }
    }
    [ContextMenu("restartDeltacast")]
    public void restartDeltacast() {
        stopDeltacast();
        connectDeltacast();

    }

    [ContextMenu("stopDeltacast")]
    public void stopDeltacast() {
        if(cameraName != null && cameraName.StartsWith("DELTACAST") && deltacastAdapter != null) {
            deltacastAdapter.Stop();
        }

        initialized = false;

    }

    void OnApplicationQuit() {
        stopDeltacast();
    }
}
