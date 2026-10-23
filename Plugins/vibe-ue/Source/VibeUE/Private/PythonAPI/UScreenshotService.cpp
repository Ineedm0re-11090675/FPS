// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "PythonAPI/UScreenshotService.h"
#include "BlueprintModes/WidgetBlueprintApplicationModes.h"
#include "Designer/SDesignerView.h"
#include "EditorAssetLibrary.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Docking/SDockTab.h"
#include "UnrealClient.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"

namespace
{
	static const FName DesignerTabId(TEXT("SlatePreview"));

	bool EnsureOutputDirectoryExists(const FString& FilePath)
	{
		const FString Directory = FPaths::GetPath(FilePath);
		if (Directory.IsEmpty())
		{
			return true;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.DirectoryExists(*Directory))
		{
			return true;
		}

		return PlatformFile.CreateDirectoryTree(*Directory);
	}

	bool SaveColorArrayAsPNG(const TArray<FColor>& ColorData, int32 Width, int32 Height, const FString& FilePath)
	{
		if (Width <= 0 || Height <= 0 || ColorData.Num() < Width * Height)
		{
			return false;
		}

		if (!EnsureOutputDirectoryExists(FilePath))
		{
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid())
		{
			return false;
		}

		if (!ImageWrapper->SetRaw(ColorData.GetData(), ColorData.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			return false;
		}

		const TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(0);
		if (CompressedData.Num() == 0)
		{
			return false;
		}

		return FFileHelper::SaveArrayToFile(CompressedData, *FilePath);
	}

	TSharedPtr<SWindow> ResolveSlateWindow(bool bPreferActiveWindow)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return nullptr;
		}

		FSlateApplication& SlateApp = FSlateApplication::Get();

		if (bPreferActiveWindow)
		{
			const TSharedPtr<SWindow> ActiveWindow = SlateApp.GetActiveTopLevelWindow();
			if (ActiveWindow.IsValid())
			{
				return ActiveWindow;
			}
		}

		TArray<TSharedRef<SWindow>> Windows = SlateApp.GetInteractiveTopLevelWindows();
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			const FString Title = Window->GetTitle().ToString();
			if (Title.Contains(TEXT("Unreal Editor")) || Title.Contains(TEXT("Widget Blueprint")) || Title.Contains(TEXT("Blueprint")))
			{
				return Window;
			}
		}

		if (Windows.Num() > 0)
		{
			return Windows[0];
		}

		const TArray<TSharedRef<SWindow>> AllWindows = SlateApp.GetTopLevelWindows();
		if (AllWindows.Num() > 0)
		{
			return AllWindows[0];
		}

		return nullptr;
	}

	bool LoadWidgetBlueprint(const FString& WidgetPath, UWidgetBlueprint*& OutWidgetBlueprint)
	{
		OutWidgetBlueprint = nullptr;
		if (WidgetPath.IsEmpty())
		{
			return false;
		}

		auto TryLoad = [&OutWidgetBlueprint](const FString& CandidatePath) -> bool
		{
			if (CandidatePath.IsEmpty())
			{
				return false;
			}

			if (UObject* Asset = UEditorAssetLibrary::LoadAsset(CandidatePath))
			{
				OutWidgetBlueprint = Cast<UWidgetBlueprint>(Asset);
				return OutWidgetBlueprint != nullptr;
			}

			return false;
		};

		if (TryLoad(WidgetPath))
		{
			return true;
		}

		if (!WidgetPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(WidgetPath);
			if (!AssetName.IsEmpty())
			{
				return TryLoad(WidgetPath + TEXT(".") + AssetName);
			}
		}

		return false;
	}

	bool IsWidgetBlueprintEditorInstance(IAssetEditorInstance* EditorInstance)
	{
		return EditorInstance && EditorInstance->GetEditorName() == TEXT("WidgetBlueprintEditor");
	}

	FWidgetBlueprintEditor* GetWidgetBlueprintEditor(
		UWidgetBlueprint* WidgetBlueprint,
		bool bOpenIfNeeded,
		bool* bOutOpenedEditor = nullptr)
	{
		if (bOutOpenedEditor)
		{
			*bOutOpenedEditor = false;
		}

		if (!WidgetBlueprint || !GEditor)
		{
			return nullptr;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			return nullptr;
		}

		IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(WidgetBlueprint, false);
		if (!EditorInstance && bOpenIfNeeded)
		{
			if (!AssetEditorSubsystem->OpenEditorForAsset(WidgetBlueprint))
			{
				return nullptr;
			}

			if (bOutOpenedEditor)
			{
				*bOutOpenedEditor = true;
			}

			EditorInstance = AssetEditorSubsystem->FindEditorForAsset(WidgetBlueprint, false);
		}

		if (!IsWidgetBlueprintEditorInstance(EditorInstance))
		{
			return nullptr;
		}

		return static_cast<FWidgetBlueprintEditor*>(EditorInstance);
	}

	TSharedPtr<SDockTab> GetDesignerTab(
		FWidgetBlueprintEditor* WidgetEditor,
		bool bSwitchToDesigner,
		bool* bOutChangedEditorState = nullptr)
	{
		if (bOutChangedEditorState)
		{
			*bOutChangedEditorState = false;
		}

		if (!WidgetEditor)
		{
			return nullptr;
		}

		if (bSwitchToDesigner && WidgetEditor->GetCurrentMode() != FWidgetBlueprintApplicationModes::DesignerMode)
		{
			WidgetEditor->SetCurrentMode(FWidgetBlueprintApplicationModes::DesignerMode);
			if (bOutChangedEditorState)
			{
				*bOutChangedEditorState = true;
			}
		}

		const TSharedPtr<FTabManager> TabManager = WidgetEditor->GetTabManager();
		if (!TabManager.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<SDockTab> DesignerTab = TabManager->FindExistingLiveTab(FTabId(DesignerTabId));
		return DesignerTab;
	}

	void CaptureSlateWidgetToFile(const TSharedPtr<SWidget>& Widget, const FString& FilePath, const FString& WidgetTitle, FScreenshotResult& OutResult)
	{
		if (!Widget.IsValid())
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("No valid Slate widget was available to capture");
			return;
		}

		if (!EnsureOutputDirectoryExists(FilePath))
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("Failed to create screenshot output directory");
			return;
		}

		TArray<FColor> PixelData;
		FIntVector ImageSize(0, 0, 0);
		const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), PixelData, ImageSize);
		if (!bCaptured)
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("Slate failed to capture the requested widget");
			return;
		}

		OutResult.Width = ImageSize.X;
		OutResult.Height = ImageSize.Y;
		OutResult.CapturedWindowTitle = WidgetTitle;

		if (SaveColorArrayAsPNG(PixelData, ImageSize.X, ImageSize.Y, FilePath))
		{
			OutResult.bSuccess = true;
			OutResult.Message = FString::Printf(TEXT("Widget screenshot saved successfully (%dx%d)"), ImageSize.X, ImageSize.Y);
		}
		else
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("Failed to save widget screenshot as PNG");
		}
	}

	void CaptureSlateWindowToFile(const TSharedPtr<SWindow>& Window, const FString& FilePath, FScreenshotResult& OutResult)
	{
		if (!Window.IsValid())
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("No valid Slate window was available to capture");
			return;
		}

		if (!EnsureOutputDirectoryExists(FilePath))
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("Failed to create screenshot output directory");
			return;
		}

		TArray<FColor> PixelData;
		FIntVector ImageSize(0, 0, 0);
		const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), PixelData, ImageSize);
		if (!bCaptured)
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("Slate failed to capture the requested editor window");
			return;
		}

		OutResult.Width = ImageSize.X;
		OutResult.Height = ImageSize.Y;
		OutResult.CapturedWindowTitle = Window->GetTitle().ToString();

		if (SaveColorArrayAsPNG(PixelData, ImageSize.X, ImageSize.Y, FilePath))
		{
			OutResult.bSuccess = true;
			OutResult.Message = FString::Printf(TEXT("Editor window screenshot saved successfully (%dx%d)"), ImageSize.X, ImageSize.Y);
		}
		else
		{
			OutResult.bSuccess = false;
			OutResult.Message = TEXT("Failed to save Slate screenshot as PNG");
		}
	}
}

UScreenshotService::UScreenshotService()
{
}

FScreenshotResult UScreenshotService::CaptureViewport(const FString& FilePath, int32 Width, int32 Height)
{
	FScreenshotResult Result;
	Result.FilePath = FilePath;

	EnsureOutputDirectoryExists(FilePath);

	// Use the automation library's screenshot functionality
	// This only works for level viewports
	if (Width <= 0) Width = 1920;
	if (Height <= 0) Height = 1080;

	// Request the screenshot through the viewport
	FScreenshotRequest::RequestScreenshot(FilePath, true, false);
	
	Result.bSuccess = true;
	Result.Message = TEXT("Viewport screenshot requested. Note: This only captures the level viewport, not editor UI.");
	Result.Width = Width;
	Result.Height = Height;
	Result.CapturedWindowTitle = TEXT("Level Viewport");

	return Result;
}

FScreenshotResult UScreenshotService::CaptureEditorWindow(const FString& FilePath)
{
	FScreenshotResult Result;
	Result.FilePath = FilePath;

	const TSharedPtr<SWindow> SlateWindow = ResolveSlateWindow(false);
	if (SlateWindow.IsValid())
	{
		CaptureSlateWindowToFile(SlateWindow, FilePath, Result);
		if (Result.bSuccess)
		{
			return Result;
		}
	}

#if PLATFORM_WINDOWS
	void* WindowHandle = FindEditorWindowHandle();
	if (WindowHandle)
	{
		CaptureWindowToFile(WindowHandle, FilePath, Result);
	}
	else
	{
		Result.bSuccess = false;
		Result.Message = TEXT("Failed to find Unreal Editor window handle");
	}
#endif

	if (!Result.bSuccess && Result.Message.IsEmpty())
	{
		Result.Message = TEXT("Failed to capture Unreal Editor window");
	}

	return Result;
}

FScreenshotResult UScreenshotService::CaptureActiveWindow(const FString& FilePath)
{
	FScreenshotResult Result;
	Result.FilePath = FilePath;

	const TSharedPtr<SWindow> SlateWindow = ResolveSlateWindow(true);
	if (SlateWindow.IsValid())
	{
		CaptureSlateWindowToFile(SlateWindow, FilePath, Result);
		if (Result.bSuccess)
		{
			return Result;
		}
	}

#if PLATFORM_WINDOWS
	HWND ForegroundWindow = GetForegroundWindow();
	if (ForegroundWindow)
	{
		CaptureWindowToFile(ForegroundWindow, FilePath, Result);
	}
	else
	{
		Result.bSuccess = false;
		Result.Message = TEXT("No active window found");
	}
#endif

	if (!Result.bSuccess && Result.Message.IsEmpty())
	{
		Result.Message = TEXT("Failed to capture active editor window");
	}

	return Result;
}

FScreenshotResult UScreenshotService::CaptureWidgetDesigner(
	const FString& WidgetPath,
	const FString& FilePath,
	bool bOpenIfNeeded,
	bool bSwitchToDesigner)
{
	FScreenshotResult Result;
	Result.FilePath = FilePath;

	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprint(WidgetPath, WidgetBlueprint) || !WidgetBlueprint)
	{
		Result.bSuccess = false;
		Result.Message = TEXT("Failed to load Widget Blueprint asset");
		return Result;
	}

	bool bOpenedEditor = false;
	FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditor(WidgetBlueprint, bOpenIfNeeded, &bOpenedEditor);
	if (!WidgetEditor)
	{
		Result.bSuccess = false;
		Result.Message = TEXT("Failed to find or open Widget Blueprint editor");
		return Result;
	}

	if (bOpenedEditor)
	{
		Result.bSuccess = false;
		Result.Message = TEXT("Widget Blueprint editor was opened for capture. Retry once the designer preview has initialized.");
		return Result;
	}

	bool bChangedDesignerState = false;
	const TSharedPtr<SDockTab> DesignerTab = GetDesignerTab(WidgetEditor, bSwitchToDesigner, &bChangedDesignerState);
	if (bChangedDesignerState)
	{
		Result.bSuccess = false;
		Result.Message = TEXT("Designer mode was activated for capture. Retry once the preview has stabilized.");
		return Result;
	}

	if (!DesignerTab.IsValid())
	{
		Result.bSuccess = false;
		Result.Message = TEXT("Widget Designer tab is not live. Open the widget in Designer mode and retry capture.");
		return Result;
	}

	if (UUserWidget* PreviewWidgetObject = WidgetEditor->GetPreview())
	{
		const TSharedPtr<SWidget> PreviewSlateWidget = PreviewWidgetObject->GetCachedWidget();
		if (PreviewSlateWidget.IsValid())
		{
			CaptureSlateWidgetToFile(PreviewSlateWidget, FilePath, TEXT("WidgetPreview"), Result);
			return Result;
		}
	}

	Result.bSuccess = false;
	Result.Message = TEXT("Widget preview Slate content is not cached yet. Retry after the designer preview finishes rebuilding.");
	return Result;
}

TArray<FEditorTabInfo> UScreenshotService::GetOpenEditorTabs()
{
	TArray<FEditorTabInfo> Tabs;

	if (GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
			for (UObject* Asset : EditedAssets)
			{
				if (Asset)
				{
					FEditorTabInfo TabInfo;
					TabInfo.TabLabel = Asset->GetName();
					TabInfo.AssetPath = Asset->GetPathName();
					TabInfo.TabType = Asset->GetClass()->GetName();
					TabInfo.bIsForeground = false; // Would need more complex logic to determine
					Tabs.Add(TabInfo);
				}
			}
		}
	}

	return Tabs;
}

FString UScreenshotService::GetActiveWindowTitle()
{
	if (FSlateApplication::IsInitialized())
	{
		const TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (ActiveWindow.IsValid())
		{
			return ActiveWindow->GetTitle().ToString();
		}
	}

#if PLATFORM_WINDOWS
	HWND ForegroundWindow = GetForegroundWindow();
	if (ForegroundWindow)
	{
		int Length = GetWindowTextLengthW(ForegroundWindow) + 1;
		if (Length > 1)
		{
			TArray<WCHAR> Buffer;
			Buffer.SetNum(Length);
			GetWindowTextW(ForegroundWindow, Buffer.GetData(), Length);
			return FString(Buffer.GetData());
		}
	}
#endif
	return FString();
}

bool UScreenshotService::IsEditorWindowActive()
{
	if (FSlateApplication::IsInitialized())
	{
		const TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		const TSharedPtr<SWindow> EditorWindow = ResolveSlateWindow(false);
		return ActiveWindow.IsValid() && EditorWindow.IsValid() && ActiveWindow == EditorWindow;
	}

#if PLATFORM_WINDOWS
	HWND ForegroundWindow = GetForegroundWindow();
	void* EditorHandle = FindEditorWindowHandle();
	return ForegroundWindow == EditorHandle;
#endif

	return false;
}

void* UScreenshotService::FindEditorWindowHandle()
{
#if PLATFORM_WINDOWS
	// Use Slate's native window handle
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SWindow> MainWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (MainWindow.IsValid())
		{
			TSharedPtr<FGenericWindow> NativeWindow = MainWindow->GetNativeWindow();
			if (NativeWindow.IsValid())
			{
				return NativeWindow->GetOSWindowHandle();
			}
		}

		// Fallback: try to find via window enumeration
		struct WindowFinder
		{
			static BOOL CALLBACK EnumCallback(HWND hwnd, LPARAM lParam)
			{
				HWND* pResult = reinterpret_cast<HWND*>(lParam);
				
				int Length = GetWindowTextLengthW(hwnd) + 1;
				if (Length > 1)
				{
					TArray<WCHAR> Buffer;
					Buffer.SetNum(Length);
					GetWindowTextW(hwnd, Buffer.GetData(), Length);
					FString Title(Buffer.GetData());
					
					// Look for main Unreal Editor window
					if (Title.Contains(TEXT("Unreal Editor")))
					{
						*pResult = hwnd;
						return 0; // Stop enumeration
					}
				}
				return 1; // Continue enumeration
			}
		};

		HWND FoundWindow = nullptr;
		EnumWindows(WindowFinder::EnumCallback, reinterpret_cast<LPARAM>(&FoundWindow));
		return FoundWindow;
	}
#endif
	return nullptr;
}

void UScreenshotService::CaptureWindowToFile(void* WindowHandle, const FString& FilePath, FScreenshotResult& OutResult)
{
#if PLATFORM_WINDOWS
	HWND hwnd = static_cast<HWND>(WindowHandle);
	
	// Get window title for result
	int TitleLength = GetWindowTextLengthW(hwnd) + 1;
	if (TitleLength > 1)
	{
		TArray<WCHAR> TitleBuffer;
		TitleBuffer.SetNum(TitleLength);
		GetWindowTextW(hwnd, TitleBuffer.GetData(), TitleLength);
		OutResult.CapturedWindowTitle = FString(TitleBuffer.GetData());
	}

	// Get window dimensions
	RECT WindowRect;
	if (!GetWindowRect(hwnd, &WindowRect))
	{
		OutResult.bSuccess = false;
		OutResult.Message = TEXT("Failed to get window dimensions");
		return;
	}

	int Width = WindowRect.right - WindowRect.left;
	int Height = WindowRect.bottom - WindowRect.top;

	if (Width <= 0 || Height <= 0)
	{
		OutResult.bSuccess = false;
		OutResult.Message = TEXT("Invalid window dimensions");
		return;
	}

	OutResult.Width = Width;
	OutResult.Height = Height;

	// Create compatible DC and bitmap
	HDC WindowDC = GetWindowDC(hwnd);
	if (!WindowDC)
	{
		OutResult.bSuccess = false;
		OutResult.Message = TEXT("Failed to get window DC");
		return;
	}

	HDC MemDC = CreateCompatibleDC(WindowDC);
	if (!MemDC)
	{
		ReleaseDC(hwnd, WindowDC);
		OutResult.bSuccess = false;
		OutResult.Message = TEXT("Failed to create compatible DC");
		return;
	}

	HBITMAP Bitmap = CreateCompatibleBitmap(WindowDC, Width, Height);
	if (!Bitmap)
	{
		DeleteDC(MemDC);
		ReleaseDC(hwnd, WindowDC);
		OutResult.bSuccess = false;
		OutResult.Message = TEXT("Failed to create bitmap");
		return;
	}

	HGDIOBJ OldBitmap = SelectObject(MemDC, Bitmap);

	// Try PrintWindow with full content flag first (better for layered/DWM windows)
	// PW_RENDERFULLCONTENT = 2 - captures DWM composed content
	BOOL PrintResult = PrintWindow(hwnd, MemDC, 2);
	
	if (!PrintResult)
	{
		// Fallback to BitBlt (works for some windows but may produce black for DirectX)
		BitBlt(MemDC, 0, 0, Width, Height, WindowDC, 0, 0, SRCCOPY);
	}

	// Prepare bitmap info for pixel extraction
	BITMAPINFOHEADER BitmapInfoHeader = {};
	BitmapInfoHeader.biSize = sizeof(BITMAPINFOHEADER);
	BitmapInfoHeader.biWidth = Width;
	BitmapInfoHeader.biHeight = -Height; // Negative for top-down
	BitmapInfoHeader.biPlanes = 1;
	BitmapInfoHeader.biBitCount = 32;
	BitmapInfoHeader.biCompression = BI_RGB;

	// Calculate buffer size (4 bytes per pixel for BGRA)
	int32 RowSize = ((Width * 32 + 31) / 32) * 4;
	int32 ImageSize = RowSize * Height;
	
	TArray<uint8> PixelData;
	PixelData.SetNum(ImageSize);

	// Get the bitmap bits
	int ScanLines = GetDIBits(MemDC, Bitmap, 0, Height, PixelData.GetData(), 
		reinterpret_cast<BITMAPINFO*>(&BitmapInfoHeader), DIB_RGB_COLORS);

	// Cleanup GDI objects
	SelectObject(MemDC, OldBitmap);
	DeleteObject(Bitmap);
	DeleteDC(MemDC);
	ReleaseDC(hwnd, WindowDC);

	if (ScanLines == 0)
	{
		OutResult.bSuccess = false;
		OutResult.Message = TEXT("Failed to get bitmap bits");
		return;
	}

	// Save as PNG
	if (SaveBitmapAsPNG(PixelData, Width, Height, FilePath))
	{
		OutResult.bSuccess = true;
		OutResult.Message = FString::Printf(TEXT("Screenshot saved successfully (%dx%d)"), Width, Height);
	}
	else
	{
		OutResult.bSuccess = false;
		OutResult.Message = TEXT("Failed to save PNG file");
	}
#else
	OutResult.bSuccess = false;
	OutResult.Message = TEXT("Screenshot capture only supported on Windows");
#endif
}

bool UScreenshotService::SaveBitmapAsPNG(const TArray<uint8>& BitmapData, int32 Width, int32 Height, const FString& FilePath)
{
	// Ensure directory exists
	FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*Directory))
		{
			PlatformFile.CreateDirectoryTree(*Directory);
		}
	}

	// Convert BGRA to RGBA for image wrapper
	TArray<FColor> Colors;
	Colors.SetNum(Width * Height);

	int32 RowSize = ((Width * 32 + 31) / 32) * 4;
	
	for (int32 Y = 0; Y < Height; Y++)
	{
		for (int32 X = 0; X < Width; X++)
		{
			int32 SrcIndex = Y * RowSize + X * 4;
			int32 DstIndex = Y * Width + X;
			
			if (SrcIndex + 3 < BitmapData.Num())
			{
				// BGRA to RGBA
				Colors[DstIndex].B = BitmapData[SrcIndex + 0];
				Colors[DstIndex].G = BitmapData[SrcIndex + 1];
				Colors[DstIndex].R = BitmapData[SrcIndex + 2];
				Colors[DstIndex].A = 255; // Force full opacity
			}
		}
	}

	// Use Unreal's image wrapper to save as PNG
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

	if (!ImageWrapper.IsValid())
	{
		return false;
	}

	// Set raw data
	TArray<uint8> RawData;
	RawData.SetNum(Width * Height * 4);
	FMemory::Memcpy(RawData.GetData(), Colors.GetData(), RawData.Num());

	if (!ImageWrapper->SetRaw(RawData.GetData(), RawData.Num(), Width, Height, ERGBFormat::RGBA, 8))
	{
		return false;
	}

	// Get compressed PNG data
	TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(0);
	if (CompressedData.Num() == 0)
	{
		return false;
	}

	// Save to file
	return FFileHelper::SaveArrayToFile(CompressedData, *FilePath);
}
