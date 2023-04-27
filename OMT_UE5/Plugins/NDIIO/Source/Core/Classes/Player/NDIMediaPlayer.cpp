/*
	All rights reserved. Copyright(c) 2018-2022, NewTek Inc.

	This file and it's use within a Product is bound by the terms of NDI SDK license that was provided
	as part of the NDI SDK. For more information, please review the license and the NDI SDK documentation.
*/


#include "NDIMediaPlayer.h"

#include <MediaIOCoreSamples.h>
#include <MediaIOCoreTextureSampleBase.h>
#include <MediaIOCoreAudioSampleBase.h>
#include <IMediaEventSink.h>
#include <IMediaTextureSampleConverter.h>
#include <Misc/EngineVersionComparison.h>


#define LOCTEXT_NAMESPACE "FNDIMediaPlayer"


// An NDI-derived media texture sample, representing a frame of video
class NDIMediaTextureSample : public FMediaIOCoreTextureSampleBase, public IMediaTextureSampleConverter
{
	using Super = FMediaIOCoreTextureSampleBase;

public:

	bool Initialize(const NDIlib_video_frame_v2_t& InVideoFrame, FTimespan InTime, UNDIMediaReceiver* InReceiver)
	{
		VideoFrame = InVideoFrame;
		Receiver = InReceiver;
		Time = InTime;

		if (InVideoFrame.FourCC == NDIlib_FourCC_video_type_UYVY)
			Data.assign(InVideoFrame.p_data, InVideoFrame.p_data + InVideoFrame.line_stride_in_bytes * InVideoFrame.yres);
		else if (InVideoFrame.FourCC == NDIlib_FourCC_video_type_UYVA)
			Data.assign(InVideoFrame.p_data, InVideoFrame.p_data + InVideoFrame.line_stride_in_bytes * InVideoFrame.yres +
			                                                       InVideoFrame.xres*InVideoFrame.yres);
		else
			return false;

		VideoFrame.p_data = Data.data();

		return true;
	}

	virtual EMediaTextureSampleFormat GetFormat() const override
	{
		return EMediaTextureSampleFormat::CharBGRA;
	}

	virtual bool IsOutputSrgb() const override
	{
		return false;
	}

	virtual const FMatrix& GetYUVToRGBMatrix() const override
	{
#if ENGINE_MAJOR_VERSION == 5
		return MediaShaders::YuvToRgbRec709Scaled;
#elif ENGINE_MAJOR_VERSION == 4
		return MediaShaders::YuvToRgbRec709Full;
#else
		#error "Unsupported engine major version"
#endif
	}

	virtual FLinearColor GetScaleRotation() const override
	{
		// Note that there appears to be a bug in the Unreal Engine where having
		// a transform other than identity does not work if the same media texture
		// sample is drawn more than once.
		return Super::GetScaleRotation();
	}

	virtual FIntPoint GetDim() const override
	{
		return FIntPoint(VideoFrame.xres, VideoFrame.yres);
	}

	virtual FIntPoint GetOutputDim() const override
	{
		return FIntPoint(VideoFrame.xres, VideoFrame.yres);
	}

	virtual uint32 GetStride() const override
	{
		return VideoFrame.line_stride_in_bytes;
	}

	virtual FMediaTimeStamp GetTime() const override
	{
		return Time;
	}

	virtual FTimespan GetDuration() const override
	{
		return ETimespan::TicksPerSecond * FFrameRate(VideoFrame.frame_rate_N, VideoFrame.frame_rate_D).AsInterval();
	}

	virtual IMediaTextureSampleConverter* GetMediaTextureSampleConverter() override
	{
		return this;
	}

	uint32 GetConverterInfoFlags() const
	{
		return ConverterInfoFlags_WillCreateOutputTexture;
	}

	virtual bool Convert(FTexture2DRHIRef & InDstTexture, const FConversionHints & Hints) override
	{
		FTexture2DRHIRef DstTexture(Receiver->DisplayFrame(VideoFrame));
		InDstTexture = DstTexture;

		return true;
	}

private:
	NDIlib_video_frame_v2_t VideoFrame;
	UNDIMediaReceiver* Receiver { nullptr };
	FMediaTimeStamp Time;
	std::vector<uint8_t> Data;
};

class NDIMediaTextureSamplePool : public TMediaObjectPool<NDIMediaTextureSample>
{};


// An NDI-derived media audio sample, representing a frame of audio
class NDIMediaAudioSample : public FMediaIOCoreAudioSampleBase
{
	using Super = FMediaIOCoreAudioSampleBase;

public:
};

class NDIMediaAudioSamplePool : public TMediaObjectPool<NDIMediaAudioSample>
{};



FNDIMediaPlayer::FNDIMediaPlayer(IMediaEventSink& InEventSink)
	: Super(InEventSink)
	, NDIPlayerState(EMediaState::Closed)
	, EventSink(InEventSink)
	, TextureSamplePool(new NDIMediaTextureSamplePool)
	, AudioSamplePool(new NDIMediaAudioSamplePool)
{}


FNDIMediaPlayer::~FNDIMediaPlayer()
{
	Close();

	delete TextureSamplePool;
	delete AudioSamplePool;
}


FGuid FNDIMediaPlayer::GetPlayerPluginGUID() const
{
	static FGuid PlayerPluginGUID(0x71b13c2b, 0x70874965, 0x8a0e23f7, 0x5be6698f);
	return PlayerPluginGUID;
}


bool FNDIMediaPlayer::Open(const FString& Url, const IMediaOptions* Options)
{
	if (!Super::Open(Url, Options))
	{
		return false;
	}

	MaxNumVideoFrameBuffer = Options->GetMediaOption(NDIMediaOption::MaxVideoFrameBuffer, (int64)8);
	MaxNumAudioFrameBuffer = Options->GetMediaOption(NDIMediaOption::MaxAudioFrameBuffer, (int64)8);
	MaxNumMetadataFrameBuffer = Options->GetMediaOption(NDIMediaOption::MaxAncillaryFrameBuffer, (int64)8);

	// Setup our different supported channels based on source settings
	SetupSampleChannels();

	// If the player is opened with an NDIMediaReceiver, use that. Otherwise create an internal one.
	bool bIsNDIMediaReceiver = Options->HasMediaOption(NDIMediaOption::IsNDIMediaReceiver);
	if (bIsNDIMediaReceiver)
	{
		Receiver = static_cast<UNDIMediaReceiver*>(const_cast<IMediaOptions*>(Options));
		bInternalReceiver = false;
	}
	else
	{
		Receiver = NewObject<UNDIMediaReceiver>();
		bInternalReceiver = true;
	}

	// Hook into the video and audio captures
	Receiver->OnNDIReceiverVideoCaptureEvent.Remove(VideoCaptureEventHandle);
	VideoCaptureEventHandle = Receiver->OnNDIReceiverVideoCaptureEvent.AddLambda([this](UNDIMediaReceiver* receiver, const NDIlib_video_frame_v2_t& video_frame)
	{
		this->DisplayFrame(video_frame);
	});
	Receiver->OnNDIReceiverAudioCaptureEvent.Remove(AudioCaptureEventHandle);
	AudioCaptureEventHandle = Receiver->OnNDIReceiverAudioCaptureEvent.AddLambda([this](UNDIMediaReceiver* receiver, const NDIlib_audio_frame_v2_t& audio_frame)
	{
		this->PlayAudio(audio_frame);
	});

	// Control the player's state based on the receiver connecting and disconnecting
	Receiver->OnNDIReceiverConnectedEvent.Remove(ConnectedEventHandle);
	ConnectedEventHandle = Receiver->OnNDIReceiverConnectedEvent.AddLambda([this](UNDIMediaReceiver* receiver)
	{
		this->NDIPlayerState = EMediaState::Playing;
	});
	Receiver->OnNDIReceiverDisconnectedEvent.Remove(DisconnectedEventHandle);
	DisconnectedEventHandle = Receiver->OnNDIReceiverDisconnectedEvent.AddLambda([this](UNDIMediaReceiver* receiver)
	{
		this->NDIPlayerState = EMediaState::Closed;
	});


	// Get ready to connect
	CurrentState = EMediaState::Preparing;
	NDIPlayerState = EMediaState::Preparing;
	EventSink.ReceiveMediaEvent(EMediaEvent::MediaConnecting);

	// Start up the receiver under the player's control.
	// Use the provided URL as the source if given, otherwise use the connection info set for the receiver
	FString Scheme;
	FString Location;
	if (Url.Split(TEXT("://"), &Scheme, &Location, ESearchCase::CaseSensitive))
	{
		FNDIConnectionInformation ConnectionInformation = Receiver->ConnectionSetting;
		ConnectionInformation.SourceName = Location;
		Receiver->Initialize(ConnectionInformation, UNDIMediaReceiver::EUsage::Controlled);
	}
	else
	{
		Receiver->Initialize(UNDIMediaReceiver::EUsage::Controlled);
	}

	return true;
}


void FNDIMediaPlayer::Close()
{
	NDIPlayerState = EMediaState::Closed;

	if (Receiver != nullptr)
	{
		// Disconnect from receiver events
		Receiver->OnNDIReceiverVideoCaptureEvent.Remove(VideoCaptureEventHandle);
		VideoCaptureEventHandle.Reset();
		Receiver->OnNDIReceiverAudioCaptureEvent.Remove(AudioCaptureEventHandle);
		AudioCaptureEventHandle.Reset();
		Receiver->OnNDIReceiverConnectedEvent.Remove(ConnectedEventHandle);
		ConnectedEventHandle.Reset();
		Receiver->OnNDIReceiverDisconnectedEvent.Remove(DisconnectedEventHandle);
		DisconnectedEventHandle.Reset();

		// Shut down the receiver
		Receiver->Shutdown();

		// If the player created the receiver, destroy the receiver
		if (bInternalReceiver)
			Receiver->ConditionalBeginDestroy();

		Receiver = nullptr;
		bInternalReceiver = false;
	}

	TextureSamplePool->Reset();
	AudioSamplePool->Reset();

	Super::Close();
}


void FNDIMediaPlayer::TickInput(FTimespan DeltaTime, FTimespan Timecode)
{
	// Update player state
	EMediaState NewState = NDIPlayerState;

	if (NewState != CurrentState)
	{
		CurrentState = NewState;
		if (CurrentState == EMediaState::Playing)
		{
			EventSink.ReceiveMediaEvent(EMediaEvent::TracksChanged);
			EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpened);
			EventSink.ReceiveMediaEvent(EMediaEvent::PlaybackResumed);
		}
		else if (NewState == EMediaState::Error)
		{
			EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpenFailed);
			Close();
		}
	}

	if (CurrentState != EMediaState::Playing)
	{
		return;
	}

	TickTimeManagement();
}


void FNDIMediaPlayer::TickFetch(FTimespan DeltaTime, FTimespan Timecode)
{
	Super::TickFetch(DeltaTime, Timecode);

	if ((CurrentState == EMediaState::Preparing) || (CurrentState == EMediaState::Playing))
	{
		if (Receiver != nullptr)
		{
			// Ask receiver to capture a new frame of video and audio.
			// Will call DisplayFrame() and PlayAudio() through capture event.
			Receiver->CaptureConnectedAudio();
			Receiver->CaptureConnectedVideo();
		}
	}

	if (CurrentState == EMediaState::Playing)
	{
		ProcessFrame();
		VerifyFrameDropCount();
	}
}


void FNDIMediaPlayer::ProcessFrame()
{
	if (CurrentState == EMediaState::Playing)
	{
		// No need to lock here. That info is only used for debug information.
		//AudioTrackFormat.NumChannels = 0;//NDIThreadAudioChannels;
		//AudioTrackFormat.SampleRate = 0;//NDIThreadAudioSampleRate;
	}
}


void FNDIMediaPlayer::DisplayFrame(const NDIlib_video_frame_v2_t& video_frame)
{
	auto TextureSample = TextureSamplePool->AcquireShared();

	if (TextureSample->Initialize(video_frame, FTimespan::FromSeconds(GetPlatformSeconds()), Receiver))
	{
		Samples->AddVideo(TextureSample);
	}
}


void FNDIMediaPlayer::PlayAudio(const NDIlib_audio_frame_v2_t& audio_frame)
{
	auto AudioSample = AudioSamplePool->AcquireShared();

	// UE wants 32bit signed interleaved audio data, so need to convert the NDI audio.
	// Fortunately the NDI library has a utility function to do that.

	// Get a buffer to convert to
	const int32 available_samples = audio_frame.no_samples * audio_frame.no_channels;
	void* SampleBuffer = AudioSample->RequestBuffer(available_samples);

	if (SampleBuffer != nullptr)
	{
		// Format to convert to
		NDIlib_audio_frame_interleaved_32s_t audio_frame_32s(
			audio_frame.sample_rate,
			audio_frame.no_channels,
			audio_frame.no_samples,
			audio_frame.timecode,
			20,
			static_cast<int32_t*>(SampleBuffer));

		// Convert received NDI audio
		NDIlib_util_audio_to_interleaved_32s_v2(&audio_frame, &audio_frame_32s);

		// Supply converted audio data
		if (AudioSample->SetProperties(available_samples
			, audio_frame_32s.no_channels
			, audio_frame_32s.sample_rate
			, FTimespan::FromSeconds(GetPlatformSeconds())
			, TOptional<FTimecode>()))
		{
			Samples->AddAudio(AudioSample);
		}
	}
}


void FNDIMediaPlayer::VerifyFrameDropCount()
{
}


bool FNDIMediaPlayer::IsHardwareReady() const
{
	return NDIPlayerState == EMediaState::Playing ? true : false;
}


void FNDIMediaPlayer::SetupSampleChannels()
{
	FMediaIOSamplingSettings VideoSettings = BaseSettings;
	VideoSettings.BufferSize = MaxNumVideoFrameBuffer;
	Samples->InitializeVideoBuffer(VideoSettings);

	FMediaIOSamplingSettings AudioSettings = BaseSettings;
	AudioSettings.BufferSize = MaxNumAudioFrameBuffer;
	Samples->InitializeAudioBuffer(AudioSettings);

	FMediaIOSamplingSettings MetadataSettings = BaseSettings;
	MetadataSettings.BufferSize = MaxNumMetadataFrameBuffer;
	Samples->InitializeMetadataBuffer(MetadataSettings);
}


//~ ITimedDataInput interface
#if WITH_EDITOR
const FSlateBrush* FNDIMediaPlayer::GetDisplayIcon() const
{
	return nullptr;
}
#endif


#undef LOCTEXT_NAMESPACE
