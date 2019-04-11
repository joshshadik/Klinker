// Klinker - Blackmagic DeckLink plugin for Unity
// https://github.com/keijiro/Klinker

using UnityEngine;
using UnityEngine.Rendering;

namespace Klinker
{
    // Frame receiver class
    // Retrieves input video frames from a DeckLink device and update a render
    // texture with them.
    [AddComponentMenu("Klinker/Frame Receiver")]
    public sealed class FrameReceiver : MonoBehaviour
    {
        #region Editable attribute

        [SerializeField] int _deviceSelection = 0;
        [SerializeField, Range(1, 6)] int _queueLength = 3;

        #endregion

        #region Target settings

        [SerializeField] RenderTexture _targetTexture;

        public RenderTexture targetTexture {
            get { return _targetTexture; }
            set { _targetTexture = value; }
        }

        [SerializeField] Renderer _targetRenderer;

        public Renderer targetRenderer {
            get { return _targetRenderer; }
            set { _targetRenderer = value; }
        }

        [SerializeField] string _targetMaterialProperty;

        public string targetMaterialProperty {
            get { return _targetMaterialProperty; }
            set { _targetMaterialProperty = value; }
        }

        #endregion

        #region Runtime properties

        public RenderTexture _receivedTexture;

        public Texture receivedTexture { get {
            return _targetTexture != null ? _targetTexture : _receivedTexture;
        } }

        public long timecodeFlicks { get { return _plugin.Timecode; } }

        public double timecodeSeconds { get {
            return (double)timecodeFlicks / Util.FlicksPerSecond;
        } }

        public int timecodeFrames { get {
            return (int)(_plugin.Timecode / _plugin.FrameDuration);
        } }

        static byte[] _nameBuffer = new byte[256];

        public string formatName { get { return _plugin?.FormatName ?? "-"; } }

        #endregion

        #region Input queue control

        long _frameTime;
        bool _prerolled;
        int _fieldCount;

        bool UpdateQueue()
        {
            // At least it should have one frame in the queue.
            if (_plugin.QueuedFrameCount == 0) return false;

            // Prerolling
            if (!_prerolled)
            {
                if (_plugin.QueuedFrameCount < 1 + _queueLength)
                    return false;
                else
                    _prerolled = true;
            }

            // Overqueuing detection and recovery
            var maxQueue = _queueLength + Mathf.Min(3, _queueLength);
            while (_plugin.QueuedFrameCount > maxQueue)
            {
                _plugin.DequeueFrame();
                _dropDetector.Warn();
            }

            // Advane the frame time. Use master clock when available.
            _frameTime += FrameSender.master?.frameDuration ?? Util.DeltaTimeInFlicks;

            // Move to the even field.
            _fieldCount = 1;

            // Dequeue input frames that are in the previous frame duration.
            var duration = _plugin.FrameDuration;
            while (_frameTime >= duration)
            {
                // If there is no frame to dequeue, restart from prerolling.
                if (_plugin.QueuedFrameCount < 2)
                {
                    _prerolled = false;
                    _dropDetector.Warn();
                    break;
                }

                // Dequeuing
                _plugin.DequeueFrame();
                _frameTime -= duration;

                // Move back to the odd field.
                _fieldCount = 0;
            }

            return true;
        }

        #endregion

        #region Private members

        ReceiverPlugin _plugin;
        Material _upsampler;
        public Texture2D _sourceTexture;
        MaterialPropertyBlock _propertyBlock;
        DropDetector _dropDetector;

        #endregion

        #region MonoBehaviour implementation

        void Start()
        {
            _plugin = new ReceiverPlugin(_deviceSelection, 0);
            _upsampler = new Material(Shader.Find("Hidden/Klinker/Upsampler"));
            _dropDetector = new DropDetector(gameObject.name);
        }

        void OnDestroy()
        {
            _plugin?.Dispose();
            Util.Destroy(_sourceTexture);
            Util.Destroy(_receivedTexture);
            Util.Destroy(_upsampler);
        }

        void Update()
        {
            if (_plugin == null) return;

            // Update input queue; Break if it's not ready.
            if (!UpdateQueue()) return;

            // Renew texture objects when the frame dimensions were changed.
            var dimensions = _plugin.FrameDimensions;
            if (_sourceTexture != null &&
                (_sourceTexture.width != dimensions.x ||
                 _sourceTexture.height != dimensions.y))
            {
                Util.Destroy(_sourceTexture);
                Util.Destroy(_receivedTexture);
                _sourceTexture = null;
                _receivedTexture = null;
            }

            // Source texture lazy initialization
            if (_sourceTexture == null)
            {
                _sourceTexture = new Texture2D(
                    dimensions.x, dimensions.y,
                    TextureFormat.BGRA32, false
                );
                _sourceTexture.filterMode = FilterMode.Point;
            }

            // Request texture update via the command buffer.
            Util.IssueTextureUpdateEvent(
                _plugin.TextureUpdateCallback, _sourceTexture, _plugin.ID
            );

            // Receiver texture lazy initialization
            if (_targetTexture == null && _receivedTexture == null)
            {
                _receivedTexture = new RenderTexture(dimensions.x, dimensions.y, 0, RenderTextureFormat.BGRA32);
                _receivedTexture.wrapMode = TextureWrapMode.Clamp;
            }

            // Chroma upsampling
            var receiver = _targetTexture != null ? _targetTexture : _receivedTexture;
            var pass = _plugin.IsProgressive ? 0 : 1 + _fieldCount;
            Graphics.Blit(_sourceTexture, receiver);

#if UNITY_2018_3_OR_NEWER
                receiver.IncrementUpdateCount();
#else
                receiver.imageContentsHash = new Hash128((uint)(Time.time * 1234.0f),(uint) Mathf.Abs(receiver.GetInstanceID()), (uint)Time.time, (uint)(Time.time * 230.0f));
#endif

            // Renderer override
            if (_targetRenderer != null)
            {
                // Material property block lazy initialization
                if (_propertyBlock == null) _propertyBlock = new MaterialPropertyBlock();

                // Read-modify-write
                _targetRenderer.GetPropertyBlock(_propertyBlock);
                _propertyBlock.SetTexture(_targetMaterialProperty, receiver);
                _targetRenderer.SetPropertyBlock(_propertyBlock);
            }

            _dropDetector.Update(_plugin.DropCount);
        }

        #endregion
    }
}
