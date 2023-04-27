/*
	All rights reserved. Copyright(c) 2018-2022, NewTek Inc.

	This file and it's use within a Product is bound by the terms of NDI SDK license that was provided
	as part of the NDI SDK. For more information, please review the license and the NDI SDK documentation.
*/

#include <Services/NDIConnectionService.h>

#include <Misc/CoreDelegates.h>
#include <NDIIOPluginSettings.h>
#include <Objects/Media/NDIMediaSender.h>
#include <Framework/Application/SlateApplication.h>
#include <Misc/EngineVersionComparison.h>

#if WITH_EDITOR

#include <Editor.h>

#endif

/** Define Global Accessors */

FNDIConnectionServiceSendEvent FNDIConnectionService::EventOnSendAudioFrame;
FNDIConnectionServiceSendEvent FNDIConnectionService::EventOnSendVideoFrame;

/** ************************ **/

/**
	Constructs a new instance of this object
*/
FNDIConnectionService::FNDIConnectionService() {}

// Begin the service
bool FNDIConnectionService::Start()
{
	if (!bIsInitialized)
	{
		bIsInitialized = true;

		// Define some basic properties
		FNDIBroadcastConfiguration Configuration;
		FString BroadcastName = TEXT("Unreal Engine");
		EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transient | RF_MarkAsNative;

		bool bBeginBroadcastOnPlay = false;

		// Load the plugin settings for broadcasting the active viewport
		if (auto* CoreSettings = NewObject<UNDIIOPluginSettings>())
		{
			// Define the configuration properties
			Configuration.FrameRate = CoreSettings->BroadcastRate;
			Configuration.FrameSize = FIntPoint(FMath::Clamp(CoreSettings->PreferredFrameSize.X, 240, 3840),
												FMath::Clamp(CoreSettings->PreferredFrameSize.Y, 240, 3840));

			// Set the broadcast name
			BroadcastName = CoreSettings->ApplicationStreamName;

			bBeginBroadcastOnPlay = CoreSettings->bBeginBroadcastOnPlay;

			// clean-up the settings object
			CoreSettings->ConditionalBeginDestroy();
			CoreSettings = nullptr;
		}

		/** Construct the Active Viewport video texture */
		this->VideoTexture = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), UTextureRenderTarget2D::StaticClass(), TEXT("NDIViewportVideoTexture"), Flags);

		/** Construct the active viewport sender */
		this->ActiveViewportSender = NewObject<UNDIMediaSender>(GetTransientPackage(), UNDIMediaSender::StaticClass(),
																TEXT("NDIViewportSender"), Flags);

		VideoTexture->UpdateResource();

		// Update the active viewport sender, with the properties defined in the settings configuration
		this->ActiveViewportSender->ChangeSourceName(BroadcastName);
		this->ActiveViewportSender->ChangeVideoTexture(VideoTexture);
		this->ActiveViewportSender->ChangeBroadcastConfiguration(Configuration);

		// hook into the core for the end of frame handlers
		FCoreDelegates::OnEndFrame.AddRaw(this, &FNDIConnectionService::OnEndFrame);
		FCoreDelegates::OnEndFrameRT.AddRaw(this, &FNDIConnectionService::OnEndRenderFrame);

#if WITH_EDITOR

		FEditorDelegates::BeginPIE.AddLambda([this](const bool Success) {
			if (auto* CoreSettings = NewObject<UNDIIOPluginSettings>())
			{
				if (CoreSettings->bBeginBroadcastOnPlay == true)
					BeginBroadcastingActiveViewport();

				// clean-up the settings object
				CoreSettings->ConditionalBeginDestroy();
				CoreSettings = nullptr;
			}
			bIsInPIEMode = true;
		});
		FEditorDelegates::PrePIEEnded.AddLambda([this](const bool Success) { StopBroadcastingActiveViewport(); });

#endif

		if (bBeginBroadcastOnPlay)
			BeginBroadcastingActiveViewport();
	}

	return true;
}

// Stop the service
void FNDIConnectionService::Shutdown()
{
	// Wait for the sync context locks
	FScopeLock AudioLock(&AudioSyncContext);
	FScopeLock RenderLock(&RenderSyncContext);

	// reset the initialization properties
	bIsInitialized = false;

	// unbind our handlers for our frame events
	FCoreDelegates::OnEndFrame.RemoveAll(this);
	FCoreDelegates::OnEndFrameRT.RemoveAll(this);

	// Cleanup the broadcasting of the active viewport
	StopBroadcastingActiveViewport();
}

// Handler for when the Non render thread frame has ended
void FNDIConnectionService::OnEndFrame()
{
	FScopeLock Lock(&AudioSyncContext);

	if (bIsInitialized)
	{
		int64 ticks = FDateTime::Now().GetTimeOfDay().GetTicks();

		if (FNDIConnectionService::EventOnSendAudioFrame.IsBound())
		{
			FNDIConnectionService::EventOnSendAudioFrame.Broadcast(ticks);
		}
	}
}

// Handler for when the render thread frame has ended
void FNDIConnectionService::OnEndRenderFrame()
{
	FScopeLock Lock(&RenderSyncContext);

	if (bIsInitialized)
	{
		int64 ticks = FDateTime::Now().GetTimeOfDay().GetTicks();

		if (FNDIConnectionService::EventOnSendVideoFrame.IsBound())
		{
			FNDIConnectionService::EventOnSendVideoFrame.Broadcast(ticks);
		}
	}
}

bool FNDIConnectionService::BeginBroadcastingActiveViewport()
{
	if (!bIsBroadcastingActiveViewport && IsValid(ActiveViewportSender))
	{
		// Load the plugin settings for broadcasting the active viewport
		if (auto* CoreSettings = NewObject<UNDIIOPluginSettings>())
		{
			// Define some basic properties
			FNDIBroadcastConfiguration Configuration;
			FString BroadcastName = TEXT("Unreal Engine");

			// Define the configuration properties
			Configuration.FrameRate = CoreSettings->BroadcastRate;
			Configuration.FrameSize = FIntPoint(FMath::Clamp(CoreSettings->PreferredFrameSize.X, 240, 3840),
												FMath::Clamp(CoreSettings->PreferredFrameSize.Y, 240, 3840));

			// Set the broadcast name
			BroadcastName = CoreSettings->ApplicationStreamName;

			// clean-up the settings object
			CoreSettings->ConditionalBeginDestroy();
			CoreSettings = nullptr;

			// Update the active viewport sender, with the properties defined in the settings configuration
			this->ActiveViewportSender->ChangeSourceName(BroadcastName);
			this->ActiveViewportSender->ChangeBroadcastConfiguration(Configuration);
		}

		// we don't want to perform the linear conversion for the active viewport,
		// since it's already had the conversion completed by the engine before passing to the sender
		ActiveViewportSender->PerformLinearTosRGBConversion(false);

		// Do not enable PTZ capabilities for active viewport sender
		ActiveViewportSender->EnablePTZ(false);

		// Initialize the sender, this will automatically start rendering output via NDI
		ActiveViewportSender->Initialize();

		// We've initialized the active viewport
		bIsBroadcastingActiveViewport = true;

		// However we need to update the 'Video Texture' to the active viewport back buffer...
		FSlateApplication::Get().GetRenderer()->OnPreResizeWindowBackBuffer().AddRaw(
			this, &FNDIConnectionService::OnActiveViewportBackbufferPreResize);
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(
			this, &FNDIConnectionService::OnActiveViewportBackbufferReadyToPresent);
	}

	// always return true
	return true;
}

// Handler for when the active viewport back buffer has been resized
void FNDIConnectionService::OnActiveViewportBackbufferPreResize(void* Backbuffer)
{
	check(IsInGameThread());

	// Ensure we have a valid video texture
	if (GetVideoTextureResource() != nullptr)
	{
		GetVideoTextureResource()->TextureRHI.SafeRelease();
		VideoTexture->UpdateResource();
		this->ActiveViewportSender->ChangeVideoTexture(VideoTexture);
	}
}

// Handler for when the back buffer is read to present to the end user
void FNDIConnectionService::OnActiveViewportBackbufferReadyToPresent(SWindow& Window,
																	 const FTexture2DRHIRef& Backbuffer)
{
	if (Window.GetType() == EWindowType::GameWindow || (Window.IsRegularWindow() && IsRunningInPIE()))
	{
		if (GetVideoTextureResource() != nullptr)
		{
			// Lets improve the performance a bit
			if (GetVideoTextureResource()->TextureRHI != Backbuffer)
			{
				GetVideoTextureResource()->TextureRHI = (FTexture2DRHIRef&)Backbuffer;
				RHIUpdateTextureReference(VideoTexture->TextureReference.TextureReferenceRHI, Backbuffer);
				this->ActiveViewportSender->ChangeVideoTexture(VideoTexture);
			}
		}
	}
}

void FNDIConnectionService::StopBroadcastingActiveViewport()
{
	// Wait for the sync context locks
	FScopeLock RenderLock(&RenderSyncContext);

	// reset the initialization properties
	bIsInPIEMode = false;

	// Ensure that if the active viewport sender is active, that we shut it down
	if (IsValid(this->ActiveViewportSender))
	{
		// shutdown the active viewport sender (just in case it was activated)
		this->ActiveViewportSender->Shutdown();

		// reset the broadcasting flag, so that we can restart the broadcast later
		this->bIsBroadcastingActiveViewport = false;
	}
}


FTextureResource* FNDIConnectionService::GetVideoTextureResource() const
{
	if(IsValid(this->VideoTexture))
#if ENGINE_MAJOR_VERSION == 5
		return this->VideoTexture->GetResource();
#elif ENGINE_MAJOR_VERSION == 4
		return this->VideoTexture->Resource;
#else
		#error "Unsupported engine major version"
		return nullptr;
#endif

	return nullptr;
}
