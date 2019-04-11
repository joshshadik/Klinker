#pragma once

#include "Common.h"
#include <atomic>
#include <mutex>
#include <queue>
#include <tuple>
#include <vector>

namespace klinker
{
    //
    // Frame receiver class
    //
    // Arrived frames will be stored in an internal queue that is only used to
    // avoid frame dropping. Frame rate matching should be done on the
    // application side.
    //
    class Receiver final : private IDeckLinkInputCallback
    {
    public:

        #pragma region Constructor/destructor

        ~Receiver()
        {
            // Internal objects should have been released.
            assert(input_ == nullptr);
            assert(displayMode_ == nullptr);
        }

        #pragma endregion

        #pragma region Accessor methods

        std::tuple<int, int> GetFrameDimensions() const
        {
            assert(displayMode_ != nullptr);
            return { displayMode_->GetWidth(), displayMode_->GetHeight() };
        }

        std::int64_t GetFrameDuration() const
        {
            BMDTimeValue duration;
            BMDTimeScale scale;
            ShouldOK(displayMode_->GetFrameRate(&duration, &scale));
            return flicksPerSecond * duration / scale;
        }

        bool IsProgressive() const
        {
            assert(displayMode_ != nullptr);
            return displayMode_->GetFieldDominance() == bmdProgressiveFrame;
        }

        std::size_t CalculateFrameDataSize() const
        {
            assert(displayMode_ != nullptr);

			std::size_t bpp = 0;

			switch (pixelFormat_)
			{
			case bmdFormat8BitYUV:
				bpp = 16;
				break;

			case bmdFormat8BitBGRA:
				bpp = 32;
				break;
			}
			
            return (bpp * displayMode_->GetWidth() * displayMode_->GetHeight()) / 8;
        }

        BSTR RetrieveFormatName() const
        {
            assert(displayMode_ != nullptr);
            std::lock_guard<std::mutex> lock(mutex_);
            BSTR name;
            ShouldOK(displayMode_->GetName(&name));
            return name;
        }

        int CountDroppedFrames() const
        {
            return dropCount_;
        }

        const std::string& GetErrorString() const
        {
            return error_;
        }

        #pragma endregion

        #pragma region Frame queue methods

        std::size_t CountQueuedFrames() const
        {
            return frameQueue_.size();
        }

        void DequeueFrame()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!frameQueue_.empty()) frameQueue_.pop();
        }

        const uint8_t* LockOldestFrameData()
        {
            mutex_.lock();

            if (!frameQueue_.empty())
            {
                return frameQueue_.front().image_.data();
            }
            else
            {
                mutex_.unlock(); // Unlock before fail return
                return nullptr;
            }
        }

        void UnlockOldestFrameData()
        {
            mutex_.unlock();
        }

        std::uint32_t GetOldestTimecode() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!frameQueue_.empty())
                return frameQueue_.front().timecode_;
            else
                return 0xffffffffU;
        }

        #pragma endregion

        #pragma region Public methods

        void Start(int deviceIndex, int formatIndex, int pixFormat)
        {
            assert(input_ == nullptr);
            assert(displayMode_ == nullptr);

            if (!InitializeInput(deviceIndex, formatIndex, (BMDPixelFormat)pixFormat)) return;

            ShouldOK(input_->StartStreams());
        }

        void Stop()
        {
            // Stop the input stream.
            if (input_ != nullptr)
            {
                input_->StopStreams();
                input_->SetCallback(nullptr);
                input_->DisableVideoInput();
            }

            // Release the internal objects.
            if (displayMode_ != nullptr)
            {
                displayMode_->Release();
                displayMode_ = nullptr;
            }

            if (input_ != nullptr)
            {
                input_->Release();
                input_ = nullptr;
            }
        }

        #pragma endregion

        #pragma region IUnknown implementation

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
        {
            if (iid == IID_IUnknown)
            {
                *ppv = this;
                return S_OK;
            }

            if (iid == IID_IDeckLinkInputCallback)
            {
                *ppv = (IDeckLinkInputCallback*)this;
                return S_OK;
            }

            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return refCount_.fetch_add(1);
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            auto val = refCount_.fetch_sub(1);
            if (val == 1) delete this;
            return val;
        }

        #pragma endregion

        #pragma region IDeckLinkInputCallback implementation

        HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
            BMDVideoInputFormatChangedEvents events,
            IDeckLinkDisplayMode* mode,
            BMDDetectedVideoInputFormatFlags flags
        ) override
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);

                // Update the display mode information.
                displayMode_->Release();
                displayMode_ = mode;
                mode->AddRef();

                // Flush the frame queue.
                while (!frameQueue_.empty()) frameQueue_.pop();
            }

            // Change the video input format as notified.
            input_->PauseStreams();

			input_->EnableVideoInput(
				displayMode_->GetDisplayMode(),
				pixelFormat_,
				bmdVideoInputEnableFormatDetection
			);


            input_->FlushStreams();
            input_->StartStreams();

            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
            IDeckLinkVideoInputFrame* videoFrame,
            IDeckLinkAudioInputPacket* audioPacket
        ) override
        {
            if (videoFrame == nullptr) return S_OK;

            if (frameQueue_.size() >= maxQueueLength_)
            {
                DebugLog("Overqueuing: Arrived frame was dropped.");
                dropCount_++;
                return S_OK;
            }

            // Calculate the data size.
            auto size = videoFrame->GetRowBytes() * videoFrame->GetHeight();
            assert(size == CalculateFrameDataSize());

            // Retrieve the data pointer.
            std::uint8_t* source;
            ShouldOK(videoFrame->GetBytes(reinterpret_cast<void**>(&source)));

            // Retrieve the timecode.
            auto timecode = GetFrameTimecode(videoFrame);

            // Allocate and push a new frame to the frame queue.
            std::lock_guard<std::mutex> lock(mutex_);
            frameQueue_.emplace(timecode, source, size);

            return S_OK;
        }

        #pragma endregion

    private:

        #pragma region Internal frame data structure

        struct FrameData
        {
            std::uint32_t timecode_;
            std::vector<uint8_t> image_;
            FrameData(std::uint32_t timecode, std::uint8_t* source, std::size_t size)
                : timecode_(timecode), image_(source, source + size) {}
        };

        #pragma endregion

        #pragma region Private members

        std::atomic<ULONG> refCount_ = 1;
        std::string error_;

        IDeckLinkInput* input_ = nullptr;
        IDeckLinkDisplayMode* displayMode_ = nullptr;

		BMDPixelFormat pixelFormat_ = BMDPixelFormat::bmdFormat8BitYUV;

        std::queue<FrameData> frameQueue_;
        mutable std::mutex mutex_;

        static const std::size_t maxQueueLength_ = 8;
        int dropCount_ = 0;

        static std::uint32_t GetFrameTimecode(IDeckLinkVideoInputFrame* frame)
        {
            IDeckLinkTimecode* timecode = nullptr;
            std::uint32_t bcdTime;

            if (frame->GetTimecode(bmdTimecodeRP188VITC1, &timecode) == S_OK)
                bcdTime = 0;
            else if (frame->GetTimecode(bmdTimecodeRP188VITC2, &timecode) == S_OK)
                bcdTime = 0x80U; // Even field flag
            else
                return 0xffffffffU;

            bcdTime |= timecode->GetBCD();

            // Drop frame flag
            if (timecode->GetFlags() & bmdTimecodeIsDropFrame) bcdTime |= 0x40;

            timecode->Release();
            return bcdTime;
        }

        bool InitializeInput(int deviceIndex, int formatIndex, BMDPixelFormat pixFormat)
        {
			this->pixelFormat_ = pixFormat;

            // Device iterator
            IDeckLinkIterator* iterator;
            auto res = CoCreateInstance(
                CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                IID_IDeckLinkIterator, reinterpret_cast<void**>(&iterator)
            );

            if (res != S_OK)
            {
                error_ = "DeckLink driver is not found.";
                return false;
            }

            // Iterate until reaching the specified index.
            IDeckLink* device = nullptr;
            for (auto i = 0; i <= deviceIndex; i++)
            {
                if (device != nullptr) device->Release();
                res = iterator->Next(&device);

                if (res != S_OK)
                {
                    error_ = "Invalid device index.";
                    iterator->Release();
                    return false;
                }
            }

            iterator->Release(); // The iterator is no longer needed.

            // Input interface of the specified device
            res = device->QueryInterface(
                IID_IDeckLinkInput,
                reinterpret_cast<void**>(&input_)
            );

            device->Release(); // The device object is no longer needed.

            if (res != S_OK)
            {
                error_ = "Device has no input.";
                return false;
            }

            // Display mode iterator
            IDeckLinkDisplayModeIterator* dmIterator;
            res = input_->GetDisplayModeIterator(&dmIterator);

            assert(res == S_OK);

            // Iterate until reaching the specified index.
            for (auto i = 0; i <= formatIndex; i++)
            {
                if (displayMode_ != nullptr) displayMode_->Release();
                res = dmIterator->Next(&displayMode_);

                if (res != S_OK)
                {
                    error_ = "Invalid format index.";
                    device->Release();
                    dmIterator->Release();
                    return false;
                }
            }

            dmIterator->Release(); // The iterator is no longer needed.

            // Set this object as a frame input callback.
            res = input_->SetCallback(this);
            assert(res == S_OK);

            // Enable the video input.
            res = input_->EnableVideoInput(
                displayMode_->GetDisplayMode(),
				pixelFormat_,
                bmdVideoInputEnableFormatDetection
            );

            if (res != S_OK)
            {
                error_ = "Can't open input device (possibly already used).";
                return false;
            }

            return true;
        }

        #pragma endregion
    };
}
