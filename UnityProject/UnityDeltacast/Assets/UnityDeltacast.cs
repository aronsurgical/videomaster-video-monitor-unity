using System;
using System.Runtime.InteropServices;
using UnityEngine;
using static UnityDeltacast;

public class UnityDeltacast : MonoBehaviour {

    public Vector2Int requestedResolution = new Vector2Int(1280, 720);

    public string cameraName = "";

    public bool initialized = false;

    public enum StereoConfig { Mono, LeftOnly, RightOnly, Stereo };

    public StereoConfig stereoConfig;

    private DeltacastAdapter deltacastAdapter;

    public Material outputMat;


    public void connectDeltacast() {
        if(cameraName != null && cameraName.StartsWith("DELTACAST")) {
            deltacastAdapter = new DeltacastAdapter();

            string[] splitDeltacastName = cameraName.Split('_');
            int boardID = int.Parse(splitDeltacastName[1]);
            int streamID = int.Parse(splitDeltacastName[2]);
            string stereoConfigString = splitDeltacastName[3];
            stereoConfig = Enum.Parse<StereoConfig>(stereoConfigString);

            if(stereoConfig.Equals(StereoConfig.Mono)) {
                deltacastAdapter.Init(boardID, streamID, requestedResolution.x, requestedResolution.y, false);
            }

            if(stereoConfig.Equals(StereoConfig.Stereo)) {
                deltacastAdapter.Init(boardID, streamID, requestedResolution.x, requestedResolution.y, true);
 
            }
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
