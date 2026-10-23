// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UScreenshotService.generated.h"

/**
 * Result of a screenshot operation
 */
USTRUCT(BlueprintType)
struct FScreenshotResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Screenshot")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadWrite, Category = "Screenshot")
	FString FilePath;

	UPROPERTY(BlueprintReadWrite, Category = "Screenshot")
	FString Message;

	UPROPERTY(BlueprintReadWrite, Category = "Screenshot")
	int32 Width = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Screenshot")
	int32 Height = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Screenshot")
	FString CapturedWindowTitle;
};

/**
 * Information about an open editor tab/window
 */
USTRUCT(BlueprintType)
struct FEditorTabInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Editor")
	FString TabLabel;

	UPROPERTY(BlueprintReadWrite, Category = "Editor")
	FString TabType;

	UPROPERTY(BlueprintReadWrite, Category = "Editor")
	FString AssetPath;

	UPROPERTY(BlueprintReadWrite, Category = "Editor")
	bool bIsForeground = false;
};

/**
 * Service for capturing screenshots of editor windows and viewports.
 * Provides Python-accessible methods for AI vision capabilities.
 * 
 * This service supports:
 * - Level viewport screenshots (using existing UE functionality)
 * - Cross-platform Slate editor window screenshots
 * - Active tab/window information retrieval
 * 
 * Usage from Python:
 *   import unreal
 *   result = unreal.ScreenshotService.capture_active_window("E:/Screenshots/capture.png")
 *   if result.success:
 *       print(f"Captured: {result.file_path}")
 */
UCLASS(BlueprintType)
class VIBEUE_API UScreenshotService : public UObject
{
	GENERATED_BODY()

public:
	UScreenshotService();

	/**
	 * Capture the level viewport to a file.
	 * This uses Unreal's built-in screenshot functionality and only works for level viewports.
	 * @param FilePath - Output file path (PNG format recommended)
	 * @param Width - Desired width (0 for viewport size)
	 * @param Height - Desired height (0 for viewport size)
	 * @return Screenshot result with success status and file info
	 */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|Screenshot")
	static FScreenshotResult CaptureViewport(const FString& FilePath, int32 Width = 1920, int32 Height = 1080);

	/**
	 * Capture the current Unreal Editor top-level Slate window to a file.
	 * Works cross-platform for editor UI content including Blueprint graphs, material editors,
	 * and Widget Designer tabs as long as the target window is currently being rendered by Slate.
	 * @param FilePath - Output file path (PNG format recommended)
	 * @return Screenshot result with success status and file info
	 */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|Screenshot")
	static FScreenshotResult CaptureEditorWindow(const FString& FilePath);

	/**
	 * Capture the currently active top-level Slate window to a file.
	 * This captures whatever editor window is currently focused inside Unreal.
	 * @param FilePath - Output file path (PNG format recommended)
	 * @return Screenshot result with success status and file info
	 */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|Screenshot")
	static FScreenshotResult CaptureActiveWindow(const FString& FilePath);

	/**
	 * Capture the Widget Designer content for a specific Widget Blueprint asset.
	 * This targets the live Widget Designer preview instead of the editor's top-level window.
	 * To avoid racing transient preview rebuilds, this function may return a retry message if it had to
	 * open the asset editor or switch the editor into Designer mode in the same call.
	 * @param WidgetPath - Widget Blueprint asset package or object path
	 * @param FilePath - Output file path (PNG format recommended)
	 * @param bOpenIfNeeded - Open the widget editor first if needed
	 * @param bSwitchToDesigner - Switch the asset editor to Designer mode before capture
	 * @return Screenshot result with success status and file info
	 */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|Screenshot")
	static FScreenshotResult CaptureWidgetDesigner(
		const FString& WidgetPath,
		const FString& FilePath,
		bool bOpenIfNeeded = true,
		bool bSwitchToDesigner = true);

	/**
	 * Get information about open editor tabs.
	 * Useful for understanding what the user is currently viewing.
	 * @return Array of editor tab information
	 */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|Screenshot")
	static TArray<FEditorTabInfo> GetOpenEditorTabs();

	/**
	 * Get the title of the currently focused window.
	 * @return Window title string
	 */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|Screenshot")
	static FString GetActiveWindowTitle();

	/**
	 * Check if the Unreal Editor main window is in focus.
	 * @return True if the editor is the foreground window
	 */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|Screenshot")
	static bool IsEditorWindowActive();

private:
	/**
	 * Find the Unreal Editor main window handle
	 * @return Window handle or nullptr if not found
	 */
	static void* FindEditorWindowHandle();

	/**
	 * Capture a window by its handle to a file
	 * @param WindowHandle - Native window handle
	 * @param FilePath - Output file path
	 * @param OutResult - Result structure to populate
	 */
	static void CaptureWindowToFile(void* WindowHandle, const FString& FilePath, FScreenshotResult& OutResult);

	/**
	 * Save raw bitmap data to PNG file
	 * @param BitmapData - Raw BGRA pixel data
	 * @param Width - Image width
	 * @param Height - Image height
	 * @param FilePath - Output file path
	 * @return True if save succeeded
	 */
	static bool SaveBitmapAsPNG(const TArray<uint8>& BitmapData, int32 Width, int32 Height, const FString& FilePath);
};
