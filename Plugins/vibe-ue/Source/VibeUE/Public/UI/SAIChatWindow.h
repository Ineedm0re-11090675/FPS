// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateColor.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAIChatWindow, Log, All);

class SMultiLineEditableTextBox;
class STextBlock;
class SWindow;

class VIBEUE_API SAIChatWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAIChatWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SAIChatWindow() override = default;

	static void OpenWindow();
	static void CloseWindow();
	static void ToggleWindow();
	static bool IsWindowOpen();

private:
	FText GetMCPStatusText() const;
	FText GetMCPStatusTooltipText() const;
	FSlateColor GetMCPStatusColor() const;

	FText GetMCPDashboardToolsCountText() const;
	FText GetMCPDashboardServerSummaryText() const;
	FText GetMCPDashboardToolCallsText() const;
	FText GetMCPDashboardRecentEventsText() const;

	FReply OnToolsClicked();
	FReply OnSettingsClicked();
	void CloseToolsPopup();

	TSharedPtr<STextBlock> ToolsCountText;
	TSharedPtr<STextBlock> MCPStatusText;
	TSharedPtr<SMultiLineEditableTextBox> ToolCallsTextBox;
	TSharedPtr<SMultiLineEditableTextBox> RecentEventsTextBox;
	TWeakPtr<SWindow> ToolsPopupWindow;

	static TWeakPtr<SWindow> WindowInstance;
	static TSharedPtr<SAIChatWindow> WidgetInstance;
};
