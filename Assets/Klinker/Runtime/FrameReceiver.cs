using UnityEngine;
using UnityEngine.Rendering;
using System.Runtime.InteropServices;

namespace Klinker
{
    public class FrameReceiver : MonoBehaviour
    {
        #region Device settings

        [SerializeField] int _deviceSelection = 0;

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

        RenderTexture _receivedTexture;

        public Texture receivedTexture { get {
            return _targetTexture != null ? _targetTexture : _receivedTexture;
        } }

        static byte[] _nameBuffer = new byte[256];

        public string formatName { get {
            if (_plugin == System.IntPtr.Zero) return "-";
            var bstr = PluginEntry.GetReceiverFormatName(_plugin);
            if (bstr == System.IntPtr.Zero) return "-";
            return Marshal.PtrToStringBSTR(bstr);
        } }

        #endregion

        #region Private members

        System.IntPtr _plugin;
        Texture2D _sourceTexture;
        Material _blitMaterial;
        MaterialPropertyBlock _propertyBlock;

        #endregion

        #region MonoBehaviour implementation

        void Start()
        {
            _plugin = PluginEntry.CreateReceiver(_deviceSelection, 0);
            _blitMaterial = new Material(Shader.Find("Hidden/Klinker/Decoder"));
        }

        void OnDestroy()
        {
            PluginEntry.DestroyReceiver(_plugin);
            Util.Destroy(_sourceTexture);
            Util.Destroy(_receivedTexture);
            Util.Destroy(_blitMaterial);
        }

        void Update()
        {
            var width = PluginEntry.GetReceiverFrameWidth(_plugin);
            var height = PluginEntry.GetReceiverFrameHeight(_plugin);

            // Renew texture objects when the frame dimensions were changed.
            if (_sourceTexture != null &&
                (_sourceTexture.width != width / 2 ||
                 _sourceTexture.height != height))
            {
                Util.Destroy(_sourceTexture);
                Util.Destroy(_receivedTexture);
                _sourceTexture = null;
                _receivedTexture = null;
            }

            // Source texture lazy initialization
            if (_sourceTexture == null)
            {
                _sourceTexture = new Texture2D(width / 2, height);
                _sourceTexture.filterMode = FilterMode.Point;
            }

            // Request texture update via the command buffer.
            Util.IssueTextureUpdateEvent(
                PluginEntry.GetTextureUpdateCallback(),
                _sourceTexture,
                PluginEntry.GetReceiverID(_plugin)
            );

            // Receiver texture lazy initialization
            if (_targetTexture == null && _receivedTexture == null)
            {
                _receivedTexture = new RenderTexture(width, height, 0);
                _receivedTexture.wrapMode = TextureWrapMode.Clamp;
            }

            // Chroma upsampling
            var receiver = _targetTexture != null ? _targetTexture : _receivedTexture;
            Graphics.Blit(_sourceTexture, receiver, _blitMaterial, 0);
            receiver.IncrementUpdateCount();

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
        }

        #endregion
    }
}
